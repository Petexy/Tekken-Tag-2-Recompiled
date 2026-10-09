#pragma once

// Changing what the title shows without changing the title: textures
// recognised by their contents as the title loads them get other contents,
// and video frames pass through a filter on their way to the GPU.
// Registered before the title starts; used by the renderer.

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace cafe::gpu {

struct TextureReplacement {
    // The texture: its size, GX2 surface format (low six bits) and the MD5
    // of its level 0 as the title stores it (blocks of a compressed format
    // in rows, texels of others).
    uint32_t width = 0, height = 0, format = 0;
    std::array<uint8_t, 16> md5{};
    // What to show instead now: RGBA8, width x height (smaller levels are
    // made from it); null for the original contents.
    std::function<const std::vector<uint8_t>*()> contents;
};

void add_texture_replacement(TextureReplacement replacement);
const std::vector<TextureReplacement>& texture_replacements();
// A replacement's contents() now answers differently: the textures it
// matched load again. (Callable from any thread.)
void texture_replacements_changed();
uint64_t texture_replacements_generation();

// Called with each level 0 of an 8-bit one-channel texture (the planes of
// video frames) as it loads, texels in rows; may change them.
using PlaneFilter = std::function<void(uint32_t address, uint32_t width, uint32_t height, uint8_t* texels)>;
void set_plane_filter(PlaneFilter filter);
const PlaneFilter& plane_filter();

} // namespace cafe::gpu
