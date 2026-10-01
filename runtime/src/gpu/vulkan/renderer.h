#pragma once

// The Vulkan backend: renders the command processor's draws, clears and
// copies, and presents the TV scan buffer in the window.
//
// Synchronous with the command processor: work is recorded into one
// command buffer and submitted, and waited for, before the command
// processor retires a submission or completes a frame. Everything the
// guest can observe therefore happens after the GPU work it depends on,
// and guest memory the GPU reads in place (vertex and uniform buffers)
// cannot change underneath it.
//
// Surfaces: render targets and depth buffers live on the GPU, keyed by
// address, format and size; textures are images created from guest memory
// (detiled) or copied from a render target at the same address when the
// GPU wrote it last.

#include "gpu/backend.h"
#include "gpu/vulkan/context.h"
#include "gpu/vulkan/formats.h"
#include "latte/program.h"
#include "latte/shader_abi.h"
#include "latte/translate.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct shaderc_compiler;

namespace cafe::gpu::vk {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; // all levels and layers, identity swizzle
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, layers = 1, levels = 1;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
};

// A color or depth buffer the GPU renders into.
struct Target {
    uint32_t address = 0;   // of the level/slice rendered to, swizzle removed
    uint32_t format = 0;    // GX2 surface format
    uint32_t pitch = 0;     // elements
    uint32_t height = 0;    // aligned rows
    uint32_t tile_mode = 0;
    bool depth = false;
    Image image;
    uint64_t written = 0;   // event stamp of the last GPU write
    uint64_t overwritten = 0; // event stamp of the last CPU write to its memory
};

struct Texture {
    std::array<uint32_t, 7> words{}; // the resource, key
    Image image;
    VkImageView view = VK_NULL_HANDLE; // with the resource's component selects
    uint32_t memory[2][2] = {}; // guest ranges [begin, end): base levels, mip levels
    uint64_t loaded = 0;     // event stamp the contents correspond to
    uint64_t hash = 0;       // of the guest data last loaded
    bool dirty = true;       // the CPU wrote its memory since it was loaded
    const Target* source = nullptr; // render target the contents came from
};

struct ShaderModule {
    VkShaderModule module = VK_NULL_HANDLE;
    latte::TranslatedShader info;
    bool failed = false;
};

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const ShaderModule* vs = nullptr;
    const ShaderModule* ps = nullptr;
};

class Renderer final : public Backend {
public:
    Renderer();
    const char* name() const override { return "Vulkan"; }
    void draw(const Registers& regs, const Draw& draw) override;
    void clear_color(const Registers& regs, const gx2::ColorBuffer& buffer, const float rgba[4]) override;
    void clear_depth_stencil(const Registers& regs, const gx2::DepthBuffer& buffer, uint32_t flags, float depth,
                             uint32_t stencil) override;
    void copy_surface(const gx2::Surface& src, uint32_t src_level, uint32_t src_slice, const gx2::Surface& dst,
                      uint32_t dst_level, uint32_t dst_slice) override;
    void resolve_color(const gx2::ColorBuffer& src, const gx2::Surface& dst, uint32_t dst_level,
                       uint32_t dst_slice) override;
    void expand_depth(const gx2::DepthBuffer&) override {}
    void convert_depth(const gx2::DepthBuffer& src, const gx2::Surface& dst, uint32_t dst_level,
                       uint32_t dst_slice) override;
    void copy_to_scan_buffer(const gx2::ColorBuffer& buffer, uint32_t scan_target) override;
    void swap() override;
    void invalidate(uint32_t address, uint32_t size, uint32_t coherency_flags) override;
    void cpu_wrote(uint32_t address, uint32_t size) override;
    void sync() override;

private:
    // ----------------------------------------------------- commands (renderer.cpp)
    VkCommandBuffer cmd();
    void submit(bool wait);
    void barrier();
    uint8_t* upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset);
    Image create_image(VkFormat format, VkImageAspectFlags aspect, uint32_t width, uint32_t height, uint32_t layers,
                       uint32_t levels, VkImageUsageFlags usage, VkImageViewType view_type);
    void destroy_image(Image& image);
    void end_rendering();
    uint64_t stamp() { return ++events_; }

    // ------------------------------------------------- targets (targets.cpp)
    Target* color_target(uint32_t base_reg, uint32_t size_reg, uint32_t info_reg);
    Target* color_target(const gx2::ColorBuffer& buffer);
    Target* depth_target(uint32_t base_reg, uint32_t size_reg, uint32_t info_reg);
    Target* depth_target(const gx2::DepthBuffer& buffer);
    Target* find_target(uint32_t address, uint32_t format, bool depth);
    void copy_target_region(const Target& src, const Image& dst, uint32_t dst_layer, uint32_t width, uint32_t height);

    // ----------------------------------------------- textures (textures.cpp)
    Texture* texture(const uint32_t words[7]);
    void load_texture(Texture& t);
    void copy_depth_to_texture(const Target& src, Texture& t, uint32_t width, uint32_t height);
    VkSampler sampler(const uint32_t words[3], const float border[4], bool compare);

    // ---------------------------------------------------- draws (draw.cpp)
    const ShaderModule* shader(latte::Stage stage, uint32_t address, uint32_t size, uint32_t fetch_address,
                               uint32_t fetch_size, const latte::ShaderEnvironment& env);
    const latte::Program* program(uint32_t address, uint32_t size, uint64_t& hash);
    VkPipelineLayout pipeline_layout(uint32_t vs_textures, uint32_t ps_textures, bool vs_registers,
                                     bool ps_registers);
    const Pipeline* pipeline(const Registers& regs, const ShaderModule* vs, const ShaderModule* ps,
                             const VkFormat* colors, uint32_t color_count, VkFormat depth, uint32_t topology_class);
    latte::abi::Buffer buffer_descriptor(uint32_t address, uint32_t size, uint32_t stride) const;

    // -------------------------------------------------- presentation (renderer.cpp)
    void load_pipeline_cache();
    void save_pipeline_cache();
    void create_swapchain();
    void present();
    void capture();
    void save_image(const Image& image, const std::string& path);

    Context ctx_;
    GuestMemory guest_;
    UploadRing ring_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool recording_ = false;
    VkFence fence_ = VK_NULL_HANDLE;
    uint64_t events_ = 1;

    // Current dynamic rendering pass.
    bool rendering_ = false;
    std::array<const Target*, 8> pass_colors_{};
    const Target* pass_depth_ = nullptr;
    uint32_t pass_width_ = 0, pass_height_ = 0;
    const Pipeline* bound_pipeline_ = nullptr;

    std::vector<std::unique_ptr<Target>> targets_;
    std::unordered_map<uint64_t, std::unique_ptr<Texture>> textures_;
    std::unordered_map<uint64_t, VkSampler> samplers_;

    shaderc_compiler* compiler_ = nullptr;
    struct ProgramEntry {
        uint64_t hash;
        latte::Program program;
        bool ok;
    };
    std::unordered_map<uint64_t, ProgramEntry> programs_;  // by address/size
    std::unordered_map<uint64_t, std::unique_ptr<ShaderModule>> shaders_;
    std::unordered_map<uint64_t, VkPipelineLayout> layouts_;
    std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> pipelines_;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    uint32_t new_pipelines_ = 0;

    // Scan-out: the last frame copied to each scan buffer (TV, DRC).
    Image scan_[2];
    bool scan_valid_[2] = {};

    // Window presentation.
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered_;

    // TTT2_TRACE_FRAME=n: log every command of frame n (counted from 0).
    bool tracing() const { return frame_number_ == trace_frame_; }
    uint64_t frame_number_ = 0;
    uint64_t trace_frame_ = UINT64_MAX;

    // Statistics, printed every ten seconds.
    uint64_t frames_ = 0, draws_ = 0, skipped_draws_ = 0;
    double window_start_ = 0;
};

} // namespace cafe::gpu::vk
