#pragma once

// The title screen's pictures without "Wii U Edition", made from the Wii U
// ones (the first textures of the title screen's set, entry 76C7E3CF of
// data007): the logo ends under its "TAG TOURNAMENT 2" banner, the kanji
// tucked behind it, and sits lower to fill the space; the copyright lines
// lose the words.

#include "title/image.h"

#include <cstdint>
#include <optional>

namespace cafe::title::title_screen {

constexpr uint32_t kTextures = 0x76C7E3CF; // data007

// The 1280x720 still of the logo, without the subtitle. Rows move down by
// kLogoShift (`shifted`) or stay (not `shifted`, for comparing with the
// Wii U still).
constexpr uint32_t kLogoShift = 54;
std::optional<Rgba> logo(const Rgba& wiiu_still, bool shifted);

// A copyright line ("TEKKEN TAG TOURNAMENT 2 Wii U EDITION & (C) 2012 ...")
// without "Wii U EDITION", centred in the same size; none for lines
// without it.
std::optional<Rgba> copyright_line(const Rgba& line);

} // namespace cafe::title::title_screen
