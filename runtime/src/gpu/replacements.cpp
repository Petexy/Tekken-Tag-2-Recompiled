#include "gpu/replacements.h"

#include <atomic>

namespace cafe::gpu {
namespace {

std::vector<TextureReplacement> g_replacements;
std::atomic<uint64_t> g_generation{1};
PlaneFilter g_plane_filter;

} // namespace

void add_texture_replacement(TextureReplacement replacement) { g_replacements.push_back(std::move(replacement)); }
const std::vector<TextureReplacement>& texture_replacements() { return g_replacements; }
void texture_replacements_changed() { g_generation.fetch_add(1, std::memory_order_release); }
uint64_t texture_replacements_generation() { return g_generation.load(std::memory_order_acquire); }

void set_plane_filter(PlaneFilter filter) { g_plane_filter = std::move(filter); }
const PlaneFilter& plane_filter() { return g_plane_filter; }

} // namespace cafe::gpu
