// Textures and samplers.
//
// A texture is a GPU image for one texture resource (the seven
// SQ_TEX_RESOURCE words). Its contents come from guest memory, detiled,
// or from a render target at the same address if the GPU wrote that last.
// GX2Invalidate (SURFACE_SYNC) marks textures whose memory the CPU wrote;
// a dirty texture reloads if the data's hash changed.

#include "gpu/vulkan/renderer.h"

#include "gpu/latte.h"
#include "gpu/tiling.h"
#include "gx2/internal.h"

#include "cafe/runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace cafe::gpu::vk {
namespace {

using namespace latte;

uint64_t hash_words(const uint32_t* words, size_t count) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < count; ++i) h = (h ^ words[i]) * 0x100000001B3ull;
    return h;
}

// Fast hash of guest data, 8 bytes at a time.
uint64_t hash_bytes(const uint8_t* data, size_t size) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ size;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t v;
        std::memcpy(&v, data + i, 8);
        h = (h ^ (v * 0xBF58476D1CE4E5B9ull)) * 0x94D049BB133111EBull;
        h ^= h >> 29;
    }
    for (; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}

VkImageViewType view_type_of(uint32_t dim) {
    switch (dim) {
    case gx2::kDim1D: return VK_IMAGE_VIEW_TYPE_1D;
    case gx2::kDim3D: return VK_IMAGE_VIEW_TYPE_3D;
    case gx2::kDimCube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case gx2::kDim1DArray: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case gx2::kDim2DArray: case gx2::kDim2DMsaaArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

struct Resource {
    uint32_t dim, tile_mode, width, height, depth, format, base, mips, base_level, last_level, first_slice, last_slice;
    bool depth_tiles;
    uint32_t sel[4];
};

Resource decode(const uint32_t* w) {
    Resource r{};
    r.dim = w[0] & 7;
    r.tile_mode = (w[0] >> 3) & 0xF;
    r.depth_tiles = (w[0] >> 7) & 1;
    r.width = ((w[0] >> 19) & 0x1FFF) + 1;
    r.height = (w[1] & 0x1FFF) + 1;
    r.depth = ((w[1] >> 13) & 0x1FFF) + 1;
    r.format = texture_surface_format(w[1], w[4]);
    r.base = w[2] << 8;
    r.mips = w[3] << 8;
    for (int i = 0; i < 4; ++i) r.sel[i] = (w[4] >> (16 + 3 * i)) & 7;
    r.base_level = (w[4] >> 28) & 0xF;
    r.last_level = w[5] & 0xF;
    r.first_slice = (w[5] >> 4) & 0x1FFF;
    r.last_slice = (w[5] >> 17) & 0x1FFF;
    if (r.dim == gx2::kDim1D || r.dim == gx2::kDim1DArray) r.height = 1;
    if (r.dim == gx2::kDimCube) r.depth *= 6;
    if (r.dim == gx2::kDim1D || r.dim == gx2::kDim2D || r.dim == gx2::kDim2DMsaa) r.depth = 1;
    if (r.dim == gx2::kDim2DMsaa || r.dim == gx2::kDim2DMsaaArray) r.last_level = 0; // LAST_LEVEL holds the samples
    return r;
}

// The GX2 surface a resource describes, laid out as GX2 laid it out.
gx2::Surface surface_of(const Resource& r) {
    gx2::Surface s{};
    s.dim = r.dim == gx2::kDim2DMsaa ? gx2::kDim2D : r.dim == gx2::kDim2DMsaaArray ? gx2::kDim2DArray : r.dim;
    s.width = r.width;
    s.height = r.height;
    s.depth = r.depth;
    s.mip_levels = r.last_level + 1;
    s.format = r.format;
    s.use = gx2::surface_use::kTexture | (r.depth_tiles ? gx2::surface_use::kDepthBuffer : 0);
    s.tile_mode = r.tile_mode;
    s.swizzle = gx2::is_macro_tiled(r.tile_mode) ? (r.base & 0x700) : 0;
    s.image = gx2::is_macro_tiled(r.tile_mode) ? r.base & ~0x700u : r.base;
    s.mipmaps = gx2::is_macro_tiled(r.tile_mode) ? r.mips & ~0x700u : r.mips;
    gx2::calc_surface_size_and_alignment(s);
    return s;
}

VkComponentSwizzle component(const FormatInfo& f, uint32_t sel) {
    static constexpr VkComponentSwizzle kRgba[] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                                   VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    switch (sel) {
    case 0: case 1: case 2: case 3: {
        const VkComponentSwizzle m[] = {f.swizzle.r, f.swizzle.g, f.swizzle.b, f.swizzle.a};
        return m[sel] == VK_COMPONENT_SWIZZLE_IDENTITY ? kRgba[sel] : m[sel];
    }
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

} // namespace

Texture* Renderer::texture(const uint32_t words[7]) {
    const uint64_t key = hash_words(words, 7);
    auto& entry = textures_[key];
    if (entry && !std::equal(entry->words.begin(), entry->words.end(), words)) {
        fatal("GPU: texture key collision");
    }
    if (!entry) {
        const Resource r = decode(words);
        const FormatInfo f = color_format(r.format);
        if (f.format == VK_FORMAT_UNDEFINED) {
            static uint32_t reported = 0;
            if (reported != r.format) {
                std::fprintf(stderr, "ttt2: gpu: unsupported texture format 0x%03X (dim %u, %ux%u)\n", r.format, r.dim,
                             r.width, r.height);
            }
            reported = r.format;
            textures_.erase(key);
            return nullptr;
        }
        auto t = std::make_unique<Texture>();
        std::copy(words, words + 7, t->words.begin());
        const uint32_t layers = r.dim == gx2::kDim3D ? r.depth : r.depth;
        t->image = create_image(f.format, VK_IMAGE_ASPECT_COLOR_BIT, r.width, r.height, layers, r.last_level + 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                view_type_of(r.dim));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = t->image.image;
        vci.viewType = view_type_of(r.dim);
        vci.format = f.format;
        vci.components = {component(f, r.sel[0]), component(f, r.sel[1]), component(f, r.sel[2]), component(f, r.sel[3])};
        const uint32_t base_level = std::min(r.base_level, r.last_level);
        const uint32_t array_layers = r.dim == gx2::kDim3D ? 1 : layers;
        uint32_t first = std::min(r.first_slice, array_layers - 1);
        uint32_t count = std::min(r.last_slice, array_layers - 1) - first + 1;
        if (vci.viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
            first = 0;
            count = 6;
        } else if (vci.viewType == VK_IMAGE_VIEW_TYPE_2D || vci.viewType == VK_IMAGE_VIEW_TYPE_1D ||
                   vci.viewType == VK_IMAGE_VIEW_TYPE_3D) {
            count = 1;
        }
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_level, r.last_level - base_level + 1, first, count};
        VK_CHECK(vkCreateImageView(ctx_.device, &vci, nullptr, &t->view));
        const gx2::Surface s = surface_of(r);
        t->memory[0][0] = s.image;
        t->memory[0][1] = s.image + s.image_size;
        t->memory[1][0] = s.mipmaps;
        t->memory[1][1] = s.mip_levels > 1 ? uint32_t{s.mipmaps} + s.mipmap_size : uint32_t{s.mipmaps};
        entry = std::move(t);
    }
    load_texture(*entry);
    return entry.get();
}

void Renderer::copy_depth_to_texture(const Target& src, Texture& t, uint32_t width, uint32_t height) {
    const uint32_t bytes = src.image.format == VK_FORMAT_D16_UNORM ? 2 : 4;
    width = std::min({width, src.image.width, t.image.width});
    height = std::min({height, src.image.height, t.image.height});
    VkDeviceSize offset = 0;
    upload(VkDeviceSize{width} * height * bytes, 16, offset);
    end_rendering();
    barrier();
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd(), src.image.image, VK_IMAGE_LAYOUT_GENERAL, ring_.buffer(), 1, &region);
    barrier();
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cmd(), ring_.buffer(), t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrier();
}

// Render targets holding a texture's levels: written by the GPU after the
// CPU last wrote that memory, with elements of the texture's size. Elements
// are texels, or 4x4 blocks of a compressed texture: the title compresses
// textures on the GPU by rendering each block as one 64- or 128-bit texel
// (BC1 over R16G16B16A16, BC3 over R32G32B32A32), and the console reads the
// same memory either way. Returns false when level 0 has no such target.
bool Renderer::load_texture_from_targets(Texture& t) {
    const Resource r = decode(t.words.data());
    const FormatInfo f = color_format(r.format);
    const gx2::Surface s = surface_of(r);
    const uint32_t levels = std::min(r.last_level + 1, 14u);
    const Target* sources[14] = {};
    uint64_t newest = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        uint32_t address = level == 0 ? uint32_t{s.image}
                                      : s.mipmaps + (level > 1 ? uint32_t{s.mip_level_offset[level - 1]} : 0u);
        if (gx2::is_macro_tiled(r.tile_mode)) address &= ~0x700u;
        const Target* src = find_target(address, 0, false);
        if (src == nullptr || src->written <= src->overwritten) {
            if (level == 0) return false;
            continue;
        }
        if (color_format(src->format).bytes != f.bytes) {
            static std::set<uint64_t> reported;
            if (reported.insert(uint64_t{address} << 32 | r.format << 12 | src->format).second) {
                std::fprintf(stderr,
                             "ttt2: gpu: texture 0x%08X level %u (fmt 0x%03X, %ux%u) over a render target of format "
                             "0x%03X\n",
                             address, level, r.format, r.width, r.height, src->format);
            }
            if (level == 0) return false;
            continue;
        }
        sources[level] = src;
        newest = std::max(newest, src->written);
    }
    if (t.source == sources[0] && t.loaded >= newest) return true;
    const uint32_t block = f.compressed ? 4 : 1;
    end_rendering();
    barrier();
    for (uint32_t level = 0; level < levels; ++level) {
        const Target* src = sources[level];
        if (src == nullptr) continue;
        const uint32_t width = std::max(1u, r.width >> level), height = std::max(1u, r.height >> level);
        // In source texels, which are the destination's blocks.
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.extent = {std::min((width + block - 1) / block, src->image.width),
                         std::min((height + block - 1) / block, src->image.height), 1};
        vkCmdCopyImage(cmd(), src->image.image, VK_IMAGE_LAYOUT_GENERAL, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                       &region);
    }
    barrier();
    t.source = sources[0];
    t.loaded = stamp();
    t.dirty = false;
    return true;
}

void Renderer::load_texture(Texture& t) {
    const Resource r = decode(t.words.data());
    const FormatInfo f = color_format(r.format);
    // A depth buffer read as a texture.
    const uint32_t hw = r.format & 0x3F;
    if (hw == fmt::k8_24 || hw == fmt::k8_24Float || hw == fmt::k32Float || hw == fmt::k16 ||
        hw == fmt::kX24_8_32Float) {
        const uint32_t depth_address = gx2::is_macro_tiled(r.tile_mode) ? r.base & ~0x700u : r.base;
        if (const Target* src = find_target(depth_address, 0, true); src && src->written > src->overwritten) {
            const bool compatible = (hw == fmt::k16) == (src->image.format == VK_FORMAT_D16_UNORM) &&
                                    hw != fmt::kX24_8_32Float;
            if (!compatible) {
                std::fprintf(stderr, "ttt2: gpu: texture format 0x%03X over a depth buffer of format 0x%03X\n",
                             r.format, src->format);
                return;
            }
            if (t.source != src || t.loaded < src->written) {
                copy_depth_to_texture(*src, t, r.width, r.height);
                t.source = src;
                t.loaded = stamp();
                t.dirty = false;
            }
            return;
        }
    }
    // Render targets at the texture's address, written after the CPU last
    // wrote the memory, hold the contents.
    if (load_texture_from_targets(t)) return;
    if (!t.dirty && t.source == nullptr) return;

    const gx2::Surface s = surface_of(r);
    if (!guest_memory_committed(s.image, s.image_size) ||
        (s.mip_levels > 1 && !guest_memory_committed(s.mipmaps, s.mipmap_size))) {
        t.dirty = false;
        return;
    }
    uint64_t h = hash_bytes(guest_pointer(s.image), s.image_size);
    if (const char* w = std::getenv("TTT2_WATCH")) {
        if (static_cast<uint32_t>(std::strtoul(w, nullptr, 16)) == s.image) {
            std::fprintf(stderr, "watch: texture load 0x%08X hash %016llx (frame %llu)\n", uint32_t{s.image},
                         static_cast<unsigned long long>(h), static_cast<unsigned long long>(frame_number_));
        }
    }
    if (s.mip_levels > 1) h ^= hash_bytes(guest_pointer(s.mipmaps), s.mipmap_size) * 31;
    t.dirty = false;
    if (h == t.hash && t.source == nullptr && t.loaded != 0) return;
    t.hash = h;
    t.source = nullptr;
    t.loaded = stamp();

    const uint32_t first_non_macro = (s.swizzle >> 16) & 0xFF;
    const uint32_t block = f.compressed ? 4 : 1;
    end_rendering();
    barrier();
    for (uint32_t level = 0; level < s.mip_levels; ++level) {
        const gx2::SurfaceInfo li = gx2::surface_info(s, level);
        uint32_t level_address = level == 0 ? uint32_t{s.image} : s.mipmaps + (level > 1 ? uint32_t{s.mip_level_offset[level - 1]} : 0u);
        const uint32_t swizzle = gx2::is_macro_tiled(li.tile_mode) && level < first_non_macro ? (s.swizzle & 0x700) : 0;
        const uint32_t width = std::max(1u, r.width >> level), height = std::max(1u, r.height >> level);
        const uint32_t ew = (width + block - 1) / block, eh = (height + block - 1) / block;
        const uint32_t slices = r.dim == gx2::kDim3D ? std::max(1u, r.depth >> level) : r.depth;
        const VkDeviceSize bytes = VkDeviceSize{ew} * eh * slices * f.bytes;
        VkDeviceSize offset = 0;
        uint8_t* staging = upload(bytes, 16, offset);
        TiledLevel tl{};
        tl.data = guest_pointer(level_address);
        tl.base256b = (level_address | swizzle) >> 8;
        tl.tile_mode = li.tile_mode;
        tl.bpp = li.bpp;
        tl.pitch = li.pitch;
        tl.height = li.height;
        tl.slices = std::max(li.depth, 1u);
        tl.samples = 1;
        tl.depth = r.depth_tiles;
        detile(tl, ew, eh, 0, slices, staging);
        if (hw == fmt::k8_24) { // 24-bit unorm depth in the low bits, as a float
            auto* v = reinterpret_cast<uint32_t*>(staging);
            for (VkDeviceSize i = 0; i < bytes / 4; ++i) {
                const float d = static_cast<float>(v[i] & 0xFFFFFF) / 16777215.0f;
                std::memcpy(&v[i], &d, 4);
            }
        }
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.bufferRowLength = ew * block;
        region.bufferImageHeight = eh * block;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, r.dim == gx2::kDim3D ? 1 : slices};
        region.imageExtent = {width, height, r.dim == gx2::kDim3D ? slices : 1};
        vkCmdCopyBufferToImage(cmd(), ring_.buffer(), t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }
    barrier();
    // TTT2_DUMP_TEXTURES=<directory>: every texture loaded from memory, as PNG.
    static const char* dump = std::getenv("TTT2_DUMP_TEXTURES");
    if (dump && r.dim == gx2::kDim2D) {
        Image rgba = create_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, r.width, r.height, 1, 1,
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_VIEW_TYPE_2D);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(r.width), static_cast<int32_t>(r.height), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1] = blit.srcOffsets[1];
        vkCmdBlitImage(cmd(), t.image.image, VK_IMAGE_LAYOUT_GENERAL, rgba.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                       VK_FILTER_NEAREST);
        char name[128];
        std::snprintf(name, sizeof(name), "/tex_%08X_%03X_%ux%u_t%u.png", uint32_t{s.image}, r.format, r.width, r.height,
                      r.tile_mode);
        save_image(rgba, std::string(dump) + name);
        destroy_image(rgba);
    }
}

void Renderer::invalidate(uint32_t address, uint32_t size, uint32_t coherency_flags) {
    if (const char* w = std::getenv("TTT2_WATCH")) {
        const uint32_t watch = static_cast<uint32_t>(std::strtoul(w, nullptr, 16));
        if (address < uint64_t{watch} + 0x30000 && uint64_t{address} + size > watch) {
            std::fprintf(stderr, "watch: surface sync 0x%08X+0x%X flags 0x%08X (frame %llu)\n", address, size,
                         coherency_flags, static_cast<unsigned long long>(frame_number_));
        }
    }
    // A texture cache invalidation: textures loaded from this memory check
    // whether it changed (the DMA engine or the CPU may have written it).
    if (size == 0 || !(coherency_flags & (1u << 23))) return;
    // Titles flush the whole cache every frame; CPU writes come through
    // cpu_wrote(), so only a ranged invalidation suggests changed data.
    if (size >= 0x80000000u) return;
    const uint64_t end = uint64_t{address} + size;
    for (auto& [key, t] : textures_) {
        for (const auto& range : t->memory) {
            if (range[0] < end && range[1] > address) t->dirty = true;
        }
    }
}

void Renderer::cpu_wrote(uint32_t address, uint32_t size) {
    const uint64_t end = uint64_t{address} + size;
    static const uint32_t watch = [] {
        const char* w = std::getenv("TTT2_WATCH");
        return w ? static_cast<uint32_t>(std::strtoul(w, nullptr, 16)) : 0u;
    }();
    if (watch && address < uint64_t{watch} + 0x30000 && end > watch) {
        std::fprintf(stderr, "watch: cpu wrote 0x%08X-0x%08X (frame %llu)\n", address, static_cast<uint32_t>(end),
                     static_cast<unsigned long long>(frame_number_));
    }
    for (auto& [key, t] : textures_) {
        for (const auto& range : t->memory) {
            if (range[0] < end && range[1] > address) t->dirty = true;
        }
    }
    // The CPU's data is newer than a render target over the same memory.
    // (A flush of everything says nothing about render targets.)
    if (size < 0x10000000) {
        const uint64_t now = stamp();
        for (auto& t : targets_) {
            const uint64_t bytes = uint64_t{t->pitch} * t->height * std::max(1u, color_format(t->format).bytes);
            if (t->address < end && t->address + bytes > address) t->overwritten = now;
        }
    }
    for (auto it = programs_.begin(); it != programs_.end();) {
        const uint32_t p_address = static_cast<uint32_t>(it->first >> 32), p_size = static_cast<uint32_t>(it->first);
        if (p_address < end && uint64_t{p_address} + p_size > address) it = programs_.erase(it);
        else ++it;
    }
}

// ---------------------------------------------------------------- samplers

VkSampler Renderer::sampler(const uint32_t words[3], const float border[4], bool compare) {
    uint32_t key_words[8] = {words[0], words[1], words[2], compare};
    const uint32_t border_type = (words[0] >> 22) & 3;
    if (border_type == 3) {
        for (int i = 0; i < 4; ++i) std::memcpy(&key_words[4 + i], &border[i], 4);
    }
    const uint64_t key = hash_words(key_words, 8);
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;

    const auto address_mode = [](uint32_t clamp) {
        switch (clamp) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3: case 5: case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
    };
    const uint32_t w0 = words[0], w1 = words[1];
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.addressModeU = address_mode(w0 & 7);
    sci.addressModeV = address_mode((w0 >> 3) & 7);
    sci.addressModeW = address_mode((w0 >> 6) & 7);
    const uint32_t mag = (w0 >> 9) & 7, min = (w0 >> 12) & 7, mip = (w0 >> 17) & 3;
    sci.magFilter = (mag & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = (min & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.minLod = (w1 & 0x3FF) / 64.0f;
    sci.maxLod = ((w1 >> 10) & 0x3FF) / 64.0f;
    if (mip == 0) sci.maxLod = sci.minLod; // no mipmapping
    sci.mipLodBias = static_cast<float>(static_cast<int32_t>(w1 << 0) >> 20) / 64.0f;
    const uint32_t aniso = (w0 >> 19) & 7;
    if ((mag >= 2 || min >= 2) && aniso > 0) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = std::min<float>(static_cast<float>(1u << std::min(aniso, 4u)),
                                            ctx_.properties.limits.maxSamplerAnisotropy);
    }
    if (compare) {
        sci.compareEnable = VK_TRUE;
        sci.compareOp = static_cast<VkCompareOp>((w0 >> 26) & 7);
    }
    VkSamplerCustomBorderColorCreateInfoEXT custom{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    switch (border_type) {
    case 0: sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
    case 1: sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
    case 2: sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
    default:
        if (ctx_.custom_border_color) {
            sci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            for (int i = 0; i < 4; ++i) custom.customBorderColor.float32[i] = border[i];
            sci.pNext = &custom;
        } else {
            sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        }
        break;
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(ctx_.device, &sci, nullptr, &s));
    samplers_[key] = s;
    return s;
}

} // namespace cafe::gpu::vk
