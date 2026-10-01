#include "cafe/vfs.h"

#include "cafe/runtime.h"

// Third-party header; not held to this project's warning flags.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wnested-anon-types"
#endif
#include <zarchive/zarchivereader.h>
#pragma GCC diagnostic pop

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace cafe::vfs {
namespace {

namespace fs = std::filesystem;

struct Source {
    std::unique_ptr<ZArchiveReader> archive; // null: host directory
    std::string archive_root;                // "<titleid>_v<n>" inside the archive
    fs::path directory;
};

Source g_source;
fs::path g_save_root;
std::mutex g_mutex;

struct File {
    bool archive = false;
    ZArchiveNodeHandle node = ZARCHIVE_INVALID_NODE;
    int fd = -1;
    uint64_t size = 0;
    uint64_t position = 0;
    bool readable = false;
    bool writable = false;
    bool append = false;
};
struct Dir {
    std::vector<DirEntry> entries;
    size_t next = 0;
};
std::unordered_map<int32_t, File> g_files;
std::unordered_map<int32_t, Dir> g_dirs;
int32_t g_next_handle = 1;

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// "/vol/content/./a//b/../c" -> {"vol", "content", "a", "c"}
std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        size_t j = i;
        while (j < path.size() && path[j] != '/') ++j;
        const std::string_view part = path.substr(i, j - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.emplace_back(part);
        }
        i = j;
    }
    return parts;
}

enum class Area { kGame, kSave, kNone };

struct Resolved {
    Area area = Area::kNone;
    std::string relative;              // inside the game source: "content/hdd/x.bin"
    std::vector<std::string> save_parts; // inside the save root
};

Resolved resolve(std::string_view path) {
    const auto parts = components(path);
    Resolved r;
    if (parts.size() < 2 || !iequals(parts[0], "vol")) return r;
    if (iequals(parts[1], "save")) {
        r.area = Area::kSave;
        r.save_parts.assign(parts.begin() + 2, parts.end());
        return r;
    }
    if (iequals(parts[1], "content") || iequals(parts[1], "code") || iequals(parts[1], "meta")) {
        r.area = Area::kGame;
        for (size_t i = 1; i < parts.size(); ++i) {
            if (!r.relative.empty()) r.relative += '/';
            std::string part = parts[i];
            if (i == 1) std::transform(part.begin(), part.end(), part.begin(), ::tolower);
            r.relative += part;
        }
    }
    return r;
}

// Finds a host path component case-insensitively; keeps `name` if none
// exists yet (for creating files).
fs::path host_resolve(const fs::path& root, const std::vector<std::string>& parts) {
    fs::path current = root;
    for (const std::string& part : parts) {
        fs::path exact = current / part;
        std::error_code ec;
        if (fs::exists(exact, ec)) {
            current = exact;
            continue;
        }
        fs::path found;
        for (const auto& entry : fs::directory_iterator(current, ec)) {
            if (iequals(entry.path().filename().string(), part)) {
                found = entry.path();
                break;
            }
        }
        current = found.empty() ? exact : found;
    }
    return current;
}

fs::path game_host_path(const std::string& relative) {
    return host_resolve(g_source.directory, components(relative));
}

Status from_errno(int error) {
    switch (error) {
    case ENOENT: return kNotFound;
    case EISDIR: return kNotFile;
    case ENOTDIR: return kNotDir;
    case EACCES: case EPERM: case EROFS: return kPermissionError;
    case EEXIST: return kExists;
    case ENOSPC: return kStorageFull;
    case EMFILE: case ENFILE: return kMax;
    default: return kMediaError;
    }
}

Status open_host(const fs::path& path, std::string_view mode, int32_t& handle) {
    File file;
    int flags = 0;
    const bool plus = mode.find('+') != std::string_view::npos;
    switch (mode.empty() ? 'r' : mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; file.readable = true; file.writable = plus; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; file.writable = true; file.readable = plus; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT; file.writable = true; file.append = true; file.readable = plus; break;
    default: return kAccessError;
    }
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return kNotFile;
    file.fd = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
    if (file.fd < 0) return from_errno(errno);
    fstat(file.fd, &st);
    file.size = static_cast<uint64_t>(st.st_size);
    handle = g_next_handle++;
    g_files[handle] = file;
    return kOk;
}

Status stat_host(const fs::path& path, Stat& stat) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return from_errno(errno);
    stat.directory = S_ISDIR(st.st_mode);
    stat.size = stat.directory ? 0 : static_cast<uint64_t>(st.st_size);
    return kOk;
}

Status list_host(const fs::path& path, Dir& dir) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return fs::exists(path, ec) ? kNotDir : kNotFound;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        DirEntry e;
        e.name = entry.path().filename().string();
        stat_host(entry.path(), e.stat);
        dir.entries.push_back(std::move(e));
    }
    return kOk;
}

} // namespace

void open_game(const fs::path& source) {
    std::error_code ec;
    if (fs::is_directory(source, ec)) {
        g_source.directory = source;
        return;
    }
    g_source.archive.reset(ZArchiveReader::OpenFromFile(source));
    if (!g_source.archive) fatal("cannot open game archive %s", source.c_str());
    // A title archive holds one directory per title/version.
    const ZArchiveNodeHandle root = g_source.archive->LookUp("", false, true);
    ZArchiveReader::DirEntry entry;
    for (uint32_t i = 0; i < g_source.archive->GetDirEntryCount(root); ++i) {
        if (g_source.archive->GetDirEntry(root, i, entry) && entry.isDirectory &&
            g_source.archive->LookUp(std::string(entry.name) + "/code/Tekken.rpx") != ZARCHIVE_INVALID_NODE) {
            g_source.archive_root = std::string(entry.name);
            return;
        }
    }
    fatal("%s does not contain a title with code/Tekken.rpx", source.c_str());
}

void set_save_root(const fs::path& root) {
    g_save_root = root;
    std::error_code ec;
    fs::create_directories(root, ec);
}

std::optional<std::vector<uint8_t>> read_whole(std::string_view path) {
    int32_t handle;
    if (open_file(path, "r", handle) != kOk) return std::nullopt;
    Stat stat;
    stat_file(handle, stat);
    std::vector<uint8_t> data(stat.size);
    uint64_t transferred = 0;
    read_file(handle, data.data(), data.size(), 0, transferred);
    close_file(handle);
    data.resize(transferred);
    return data;
}

Status open_file(std::string_view path, std::string_view mode, int32_t& handle) {
    std::lock_guard lock(g_mutex);
    const Resolved r = resolve(path);
    if (r.area == Area::kSave) return open_host(host_resolve(g_save_root, r.save_parts), mode, handle);
    if (r.area != Area::kGame) return kNotFound;
    const bool writing = !mode.empty() && (mode[0] != 'r' || mode.find('+') != std::string_view::npos);
    if (!g_source.archive) {
        if (writing) return kPermissionError;
        return open_host(game_host_path(r.relative), "r", handle);
    }
    const ZArchiveNodeHandle node = g_source.archive->LookUp(g_source.archive_root + "/" + r.relative);
    if (node == ZARCHIVE_INVALID_NODE) return kNotFound;
    if (!g_source.archive->IsFile(node)) return kNotFile;
    if (writing) return kPermissionError;
    File file;
    file.archive = true;
    file.node = node;
    file.size = g_source.archive->GetFileSize(node);
    file.readable = true;
    handle = g_next_handle++;
    g_files[handle] = file;
    return kOk;
}

Status close_file(int32_t handle) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    if (it->second.fd >= 0) ::close(it->second.fd);
    g_files.erase(it);
    return kOk;
}

Status read_file(int32_t handle, void* buffer, uint64_t size, std::optional<uint64_t> position,
                 uint64_t& transferred) {
    File file;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_files.find(handle);
        if (it == g_files.end()) return kFatal;
        if (!it->second.readable) return kAccessError;
        if (position) it->second.position = *position;
        file = it->second;
    }
    transferred = 0;
    if (file.position < file.size) {
        const uint64_t want = std::min(size, file.size - file.position);
        if (file.archive) {
            transferred = g_source.archive->ReadFromFile(file.node, file.position, want, buffer);
        } else {
            while (transferred < want) {
                const ssize_t n = ::pread(file.fd, static_cast<uint8_t*>(buffer) + transferred,
                                          want - transferred, static_cast<off_t>(file.position + transferred));
                if (n <= 0) break;
                transferred += static_cast<uint64_t>(n);
            }
        }
    }
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it != g_files.end()) it->second.position = file.position + transferred;
    return kOk;
}

Status write_file(int32_t handle, const void* buffer, uint64_t size, std::optional<uint64_t> position,
                  uint64_t& transferred) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    File& file = it->second;
    if (!file.writable || file.fd < 0) return kAccessError;
    if (position) file.position = *position;
    if (file.append) file.position = file.size;
    transferred = 0;
    while (transferred < size) {
        const ssize_t n = ::pwrite(file.fd, static_cast<const uint8_t*>(buffer) + transferred,
                                   size - transferred, static_cast<off_t>(file.position + transferred));
        if (n < 0) return from_errno(errno);
        transferred += static_cast<uint64_t>(n);
    }
    file.position += transferred;
    file.size = std::max(file.size, file.position);
    return kOk;
}

Status seek_file(int32_t handle, uint64_t position) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    it->second.position = position;
    return kOk;
}

Status tell_file(int32_t handle, uint64_t& position) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    position = it->second.position;
    return kOk;
}

Status stat_file(int32_t handle, Stat& stat) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    stat.directory = false;
    stat.size = it->second.size;
    return kOk;
}

Status flush_file(int32_t handle) {
    std::lock_guard lock(g_mutex);
    const auto it = g_files.find(handle);
    if (it == g_files.end()) return kFatal;
    if (it->second.fd >= 0) ::fsync(it->second.fd);
    return kOk;
}

Status stat_path(std::string_view path, Stat& stat) {
    std::lock_guard lock(g_mutex);
    const Resolved r = resolve(path);
    if (r.area == Area::kSave) return stat_host(host_resolve(g_save_root, r.save_parts), stat);
    if (r.area != Area::kGame) return kNotFound;
    if (!g_source.archive) return stat_host(game_host_path(r.relative), stat);
    const ZArchiveNodeHandle node = g_source.archive->LookUp(g_source.archive_root + "/" + r.relative);
    if (node == ZARCHIVE_INVALID_NODE) return kNotFound;
    stat.directory = g_source.archive->IsDirectory(node);
    stat.size = stat.directory ? 0 : g_source.archive->GetFileSize(node);
    return kOk;
}

Status make_dir(std::string_view path) {
    std::lock_guard lock(g_mutex);
    const Resolved r = resolve(path);
    if (r.area != Area::kSave) return kPermissionError;
    const fs::path host = host_resolve(g_save_root, r.save_parts);
    std::error_code ec;
    if (fs::exists(host, ec)) return kExists;
    if (!fs::create_directory(host, ec)) return from_errno(ec.value());
    return kOk;
}

Status open_dir(std::string_view path, int32_t& handle) {
    std::lock_guard lock(g_mutex);
    const Resolved r = resolve(path);
    Dir dir;
    Status status = kNotFound;
    if (r.area == Area::kSave) {
        status = list_host(host_resolve(g_save_root, r.save_parts), dir);
    } else if (r.area == Area::kGame && !g_source.archive) {
        status = list_host(game_host_path(r.relative), dir);
    } else if (r.area == Area::kGame) {
        const ZArchiveNodeHandle node =
            g_source.archive->LookUp(g_source.archive_root + "/" + r.relative, false, true);
        if (node != ZARCHIVE_INVALID_NODE) {
            ZArchiveReader::DirEntry entry;
            for (uint32_t i = 0; i < g_source.archive->GetDirEntryCount(node); ++i) {
                if (!g_source.archive->GetDirEntry(node, i, entry)) continue;
                DirEntry e;
                e.name = std::string(entry.name);
                e.stat.directory = entry.isDirectory;
                e.stat.size = entry.isFile ? entry.size : 0;
                dir.entries.push_back(std::move(e));
            }
            status = kOk;
        }
    }
    if (status != kOk) return status;
    handle = g_next_handle++;
    g_dirs[handle] = std::move(dir);
    return kOk;
}

Status read_dir(int32_t handle, DirEntry& entry) {
    std::lock_guard lock(g_mutex);
    const auto it = g_dirs.find(handle);
    if (it == g_dirs.end()) return kFatal;
    if (it->second.next >= it->second.entries.size()) return kEnd;
    entry = it->second.entries[it->second.next++];
    return kOk;
}

Status close_dir(int32_t handle) {
    std::lock_guard lock(g_mutex);
    return g_dirs.erase(handle) ? kOk : kFatal;
}

} // namespace cafe::vfs
