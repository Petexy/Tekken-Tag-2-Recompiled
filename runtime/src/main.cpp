#include "cafe/export.h"
#include "cafe/generated.h"
#include "cafe/layout.h"
#include "cafe/runtime.h"
#include "cafe/sysmem.h"
#include "cafe/vfs.h"

#include <cstdlib>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace cafe::os {
void init_heaps(uint32_t mem2_begin);
int run_main_thread(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_size,
                    uint32_t sda_base, uint32_t sda2_base);
} // namespace cafe::os

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s GAME\n"
                 "  GAME is the title's .wua archive, or a directory with code/,\n"
                 "  content/ and meta/. Saves go to $TTT2_SAVE_DIR, default\n"
                 "  ~/.local/share/ttt2/save.\n",
                 argv0);
}

// Objects the game imports as data are created by the library that owns
// them; anything the runtime does not provide yet stays zeroed.
void init_data_imports() {
    for (size_t i = 0; i < cafe_guest_import_count; ++i) {
        const cafe::GuestImport& import = cafe_guest_imports[i];
        if (!import.is_data) continue;
        if (const cafe::DataExport* data = cafe::find_data_export(import.module, import.name)) {
            data->initialize(import.address);
        } else {
            std::fprintf(stderr, "ttt2: warning: data import %s:%s is not provided\n", import.module,
                         import.name);
        }
    }
}

uint32_t guest_strdup(const char* text) {
    const uint32_t address = cafe::system_alloc(static_cast<uint32_t>(std::strlen(text) + 1));
    std::memcpy(cafe::guest_pointer(address), text, std::strlen(text) + 1);
    return address;
}

} // namespace

int main(int argc, char** argv) {
    using namespace cafe;
    if (argc != 2) {
        usage(argv[0]);
        return 2;
    }
    install_fault_handler();
    vfs::open_game(argv[1]);
    if (const char* save = std::getenv("TTT2_SAVE_DIR")) {
        vfs::set_save_root(save);
    } else {
        const char* home = std::getenv("HOME");
        vfs::set_save_root(std::filesystem::path(home ? home : ".") / ".local/share/ttt2/save");
    }
    const auto rpx = vfs::read_whole("/vol/code/Tekken.rpx");
    if (!rpx) fatal("the game source has no code/Tekken.rpx");
    reserve_guest_memory();
    init_system_heap();
    const LoadedImage image = load_image(*rpx);
    build_dispatch_table();
    os::init_heaps(image.data_end);
    init_data_imports();
    std::fprintf(stderr, "ttt2: loaded image, entry 0x%08X, %zu/%zu guest entries compiled\n",
                 image.entry_point, cafe_program_info.compiled_entries,
                 cafe_program_info.total_entries);

    // argv[0] is the executable name, as the Cafe loader passes it.
    const uint32_t guest_argv = system_alloc(8);
    *reinterpret_cast<uint32_t*>(guest_pointer(guest_argv)) = __builtin_bswap32(guest_strdup("Tekken.rpx"));
    return os::run_main_thread(image.entry_point, 1, guest_argv, image.stack_size, image.sda_base,
                               image.sda2_base);
}
