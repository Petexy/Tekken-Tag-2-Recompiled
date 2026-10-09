#include "title/title_screen.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace cafe::title::title_screen {
namespace {

// The logo still: "TAG TOURNAMENT 2" ends at row 406 and the subtitle's
// brush lettering starts at row 412 (one stroke of its "N" curls up
// against the banner from row 392); the kanji go on behind both.
constexpr uint32_t kStill = 1280, kStillHeight = 720;
constexpr uint32_t kCurlTop = 392, kSubtitleTop = 412, kSubtitleBelow = 418;
constexpr uint32_t kFadeTop = 406, kFadeBottom = 418;

struct Mask {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bits;
    Mask(uint32_t w, uint32_t h) : width(w), height(h), bits(size_t{w} * h, 0) {}
    uint8_t& operator()(uint32_t x, uint32_t y) { return bits[size_t{y} * width + x]; }
    uint8_t operator()(uint32_t x, uint32_t y) const { return bits[size_t{y} * width + x]; }
};

// Square neighbourhoods, a row pass then a column pass; outside counts as
// clear.
Mask spread(const Mask& m, int radius, bool erode) {
    Mask rows(m.width, m.height), out(m.width, m.height);
    const auto at = [](const Mask& s, int64_t x, int64_t y) {
        return x >= 0 && y >= 0 && x < s.width && y < s.height && s(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    };
    for (int pass = 0; pass < 2; ++pass) {
        const Mask& in = pass == 0 ? m : rows;
        Mask& to = pass == 0 ? rows : out;
        for (uint32_t y = 0; y < m.height; ++y) {
            for (uint32_t x = 0; x < m.width; ++x) {
                bool all = true, any = false;
                for (int k = -radius; k <= radius; ++k) {
                    const bool v = pass == 0 ? at(in, int64_t{x} + k, y) : at(in, x, int64_t{y} + k);
                    all = all && v;
                    any = any || v;
                }
                to(x, y) = erode ? all : any;
            }
        }
    }
    return out;
}

// Dilation by the four neighbours, `times` over.
Mask grow(Mask m, int times) {
    for (int t = 0; t < times; ++t) {
        Mask next = m;
        for (uint32_t y = 0; y < m.height; ++y) {
            for (uint32_t x = 0; x < m.width; ++x) {
                if (m(x, y)) continue;
                next(x, y) = (x > 0 && m(x - 1, y)) || (x + 1 < m.width && m(x + 1, y)) || (y > 0 && m(x, y - 1)) ||
                             (y + 1 < m.height && m(x, y + 1));
            }
        }
        m = std::move(next);
    }
    return m;
}

// The parts of `m` (eight-connected) that reach below `row`.
Mask reaching_below(const Mask& m, uint32_t row) {
    Mask out(m.width, m.height), seen(m.width, m.height);
    std::vector<std::pair<uint32_t, uint32_t>> stack, part;
    for (uint32_t y = 0; y < m.height; ++y) {
        for (uint32_t x = 0; x < m.width; ++x) {
            if (!m(x, y) || seen(x, y)) continue;
            part.clear();
            stack.push_back({x, y});
            seen(x, y) = 1;
            uint32_t lowest = y;
            while (!stack.empty()) {
                const auto [px, py] = stack.back();
                stack.pop_back();
                part.push_back({px, py});
                lowest = std::max(lowest, py);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int64_t nx = int64_t{px} + dx, ny = int64_t{py} + dy;
                        if (nx < 0 || ny < 0 || nx >= m.width || ny >= m.height) continue;
                        const auto ux = static_cast<uint32_t>(nx), uy = static_cast<uint32_t>(ny);
                        if (m(ux, uy) && !seen(ux, uy)) {
                            seen(ux, uy) = 1;
                            stack.push_back({ux, uy});
                        }
                    }
                }
            }
            if (lowest > row) {
                for (const auto& [px, py] : part) out(px, py) = 1;
            }
        }
    }
    return out;
}

} // namespace

std::optional<Rgba> logo(const Rgba& wiiu, bool shifted) {
    if (wiiu.width != kStill || wiiu.height != kStillHeight) return std::nullopt;
    Mask bright(kStill, kStillHeight);
    for (uint32_t y = 0; y < kStillHeight; ++y) {
        for (uint32_t x = 0; x < kStill; ++x) {
            const uint8_t* p = wiiu.at(x, y);
            bright(x, y) = std::max({p[0], p[1], p[2]}) > 150;
        }
    }
    // Letters are thick, the subtitle's curling stroke thin: thin bright
    // parts below the banner's top, and all bright ones below the
    // subtitle's top, connected to what goes on below it, are the subtitle.
    const Mask thick = spread(spread(bright, 3, true), 3, false);
    const Mask thick_near = grow(thick, 2);
    Mask candidates(kStill, kStillHeight);
    for (uint32_t y = kCurlTop + 1; y < kStillHeight; ++y) {
        for (uint32_t x = 0; x < kStill; ++x) {
            candidates(x, y) = bright(x, y) && (!thick_near(x, y) || y > kSubtitleTop);
        }
    }
    const Mask subtitle = grow(reaching_below(candidates, kSubtitleBelow), 4);
    Rgba out = wiiu;
    for (uint32_t y = 0; y < kStillHeight; ++y) {
        const double keep = std::clamp((static_cast<double>(kFadeBottom) - y) / (kFadeBottom - kFadeTop), 0.0, 1.0);
        for (uint32_t x = 0; x < kStill; ++x) {
            uint8_t* p = out.at(x, y);
            const bool gone = y > kCurlTop && subtitle(x, y) && !thick(x, y);
            for (int k = 0; k < 3; ++k) p[k] = gone ? 0 : static_cast<uint8_t>(p[k] * keep + 0.5);
        }
    }
    if (!shifted) return out;
    Rgba moved = blank(kStill, kStillHeight);
    for (uint32_t y = 0; y < kStillHeight; ++y) {
        for (uint32_t x = 0; x < kStill; ++x) moved.at(x, y)[3] = 255; // black
    }
    put(moved, crop(out, 0, 0, kStill, kStillHeight - kLogoShift), 0, kLogoShift);
    return moved;
}

std::optional<Rgba> copyright_line(const Rgba& line) {
    // Words: runs of columns with letters (bright texels) apart by five
    // empty ones or more.
    std::vector<std::pair<uint32_t, uint32_t>> words;
    uint32_t x = 0;
    const auto ink = [&](uint32_t column) {
        for (uint32_t y = 0; y < line.height; ++y) {
            const uint8_t* p = line.at(column, y);
            if (std::min({p[0], p[1], p[2]}) > 150 && p[3] > 128) return true;
        }
        return false;
    };
    while (x < line.width) {
        if (!ink(x)) {
            ++x;
            continue;
        }
        const uint32_t start = x;
        uint32_t end = x, gap = 0;
        while (x < line.width && gap < 5) {
            if (ink(x)) {
                gap = 0;
                end = x + 1;
            } else {
                ++gap;
            }
            ++x;
        }
        words.push_back({start, end});
    }
    // "... 2 Wii U EDITION & (C) 2012 NAMCO BANDAI Games Inc.": the seven
    // words from "&" on end the line.
    if (words.size() < 10) return std::nullopt;
    constexpr uint32_t kOutline = 3; // letters' dark edge, before their first bright column
    const uint32_t cut_from = words[words.size() - 10].first - kOutline;
    const uint32_t cut_to = words[words.size() - 7].first - kOutline;
    Rgba joined = blank(line.width - (cut_to - cut_from), line.height);
    put(joined, crop(line, 0, 0, cut_from, line.height), 0, 0);
    put(joined, crop(line, cut_to, 0, line.width - cut_to, line.height), cut_from, 0);
    // Centred as before.
    uint32_t left = joined.width, right = 0;
    for (uint32_t y = 0; y < joined.height; ++y) {
        for (uint32_t c = 0; c < joined.width; ++c) {
            if (joined.at(c, y)[3] != 0) {
                left = std::min(left, c);
                right = std::max(right, c + 1);
            }
        }
    }
    if (right <= left) return std::nullopt;
    Rgba out = blank(line.width, line.height);
    put(out, crop(joined, left, 0, right - left, joined.height), (line.width - (right - left)) / 2, 0);
    return out;
}

} // namespace cafe::title::title_screen
