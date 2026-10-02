// Renderer setup, command recording and synchronisation, scan-out and
// presentation.

#include "gpu/vulkan/renderer.h"

#include "gpu/vulkan/png.h"
#include "host/window.h"

#include "cafe/runtime.h"

#include <shaderc/shaderc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cafe::gpu {

std::unique_ptr<Backend> make_vulkan_backend() { return std::make_unique<vk::Renderer>(); }

namespace vk {
namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr VkDeviceSize kRingSize = 256ull << 20;

} // namespace

Renderer::Renderer() {
    ctx_.init(host::vulkan_instance_extensions());
    init_formats(ctx_.physical);
    guest_.init(ctx_);
    ring_.init(ctx_, kRingSize);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = ctx_.queue_family;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx_.device, &pci, nullptr, &pool_));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx_.device, &cai, &cmd_));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(ctx_.device, &fci, nullptr, &fence_));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(ctx_.device, &sci, nullptr, &acquired_));

    if (const char* f = std::getenv("TTT2_TRACE_FRAME")) trace_frame_ = std::strtoull(f, nullptr, 10);
    // Render scale: TTT2_SCALE=1-4, else enough to cover the display
    // (2 on 1080p and 1440p, 3 on 4K).
    if (const char* sc = std::getenv("TTT2_SCALE")) {
        scale_ = static_cast<uint32_t>(std::clamp(std::atoi(sc), 1, 4));
    } else {
        uint32_t dw = 0, dh = 0;
        host::display_size(dw, dh);
        scale_ = std::clamp((dh + 719) / 720, 1u, 4u);
    }
    std::fprintf(stderr, "ttt2: GPU: rendering at %ux%u (scale %u)\n", 1280 * scale_, 720 * scale_, scale_);
    compiler_ = shaderc_compiler_initialize();
    load_pipeline_cache();
    surface_ = host::create_vulkan_surface(ctx_.instance);
    create_swapchain();
    window_start_ = now_seconds();
}

// ------------------------------------------------------------- commands

VkCommandBuffer Renderer::cmd() {
    if (!recording_) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));
        recording_ = true;
    }
    return cmd_;
}

void Renderer::submit(bool wait) {
    if (!recording_) return;
    end_rendering();
    VK_CHECK(vkEndCommandBuffer(cmd_));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    VK_CHECK(vkQueueSubmit(ctx_.queue, 1, &si, fence_));
    recording_ = false;
    bound_pipeline_ = nullptr;
    if (wait) {
        const double begin = now_seconds();
        const VkResult r = vkWaitForFences(ctx_.device, 1, &fence_, VK_TRUE, 10'000'000'000ull);
        wait_seconds_ += now_seconds() - begin;
        ++waits_;
        if (r != VK_SUCCESS) fatal("Vulkan: the GPU did not finish a submission (%d)", static_cast<int>(r));
        VK_CHECK(vkResetFences(ctx_.device, 1, &fence_));
        VK_CHECK(vkResetCommandPool(ctx_.device, pool_, 0));
        ring_.reset();
    }
}

void Renderer::sync() { submit(true); }

void Renderer::barrier() {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd(), &di);
}

uint8_t* Renderer::upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
    if (uint8_t* p = ring_.allocate(size, alignment, offset)) return p;
    submit(true);
    if (uint8_t* p = ring_.allocate(size, alignment, offset)) return p;
    fatal("Vulkan: an upload of %llu bytes does not fit the upload ring", static_cast<unsigned long long>(size));
}

void Renderer::end_rendering() {
    if (!rendering_) return;
    vkCmdEndRendering(cmd_);
    rendering_ = false;
    bound_pipeline_ = nullptr;
    barrier();
}

Image Renderer::create_image(VkFormat format, VkImageAspectFlags aspect, uint32_t width, uint32_t height,
                             uint32_t layers, uint32_t levels, VkImageUsageFlags usage, VkImageViewType view_type) {
    Image img;
    img.format = format;
    img.aspect = aspect;
    img.width = std::max(width, 1u);
    img.height = std::max(height, 1u);
    img.layers = std::max(layers, 1u);
    img.levels = std::max(levels, 1u);
    img.view_type = view_type;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                    : (view_type == VK_IMAGE_VIEW_TYPE_1D || view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) ? VK_IMAGE_TYPE_1D
                                                                                                           : VK_IMAGE_TYPE_2D;
    if (view_type == VK_IMAGE_VIEW_TYPE_CUBE || view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    ici.format = format;
    ici.extent = {img.width, ici.imageType == VK_IMAGE_TYPE_1D ? 1 : img.height,
                  ici.imageType == VK_IMAGE_TYPE_3D ? img.layers : 1};
    ici.mipLevels = img.levels;
    ici.arrayLayers = ici.imageType == VK_IMAGE_TYPE_3D ? 1 : img.layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx_.device, &ici, nullptr, &img.image));
    img.memory = ctx_.allocate_image_memory(img.image);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = view_type;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, img.levels, 0, ici.arrayLayers};
    VK_CHECK(vkCreateImageView(ctx_.device, &vci, nullptr, &img.view));

    // Every image lives in the general layout.
    end_rendering();
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img.image;
    ib.subresourceRange = vci.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cmd(), &di);
    return img;
}

void Renderer::destroy_image(Image& img) {
    // Only for images no recorded command uses.
    if (img.view) vkDestroyImageView(ctx_.device, img.view, nullptr);
    if (img.image) vkDestroyImage(ctx_.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(ctx_.device, img.memory, nullptr);
    img = Image{};
}

// ------------------------------------------------------------- scan-out

void Renderer::copy_to_scan_buffer(const gx2::ColorBuffer& buffer, uint32_t scan_target) {
    const int index = scan_target == gx2::kScanDrc ? 1 : 0;
    Target* t = color_target(buffer);
    if (t == nullptr) return;
    const uint32_t width = std::min<uint32_t>(buffer.surface.width * t->scale, t->image.width);
    const uint32_t height = std::min<uint32_t>(buffer.surface.height * t->scale, t->image.height);
    Image& scan = scan_[index];
    if (scan.image == VK_NULL_HANDLE || scan.width != width || scan.height != height || scan.format != t->image.format) {
        if (scan.image != VK_NULL_HANDLE) {
            submit(true);
            destroy_image(scan);
        }
        scan = create_image(t->image.format, VK_IMAGE_ASPECT_COLOR_BIT, width, height, 1, 1,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_VIEW_TYPE_2D);
    }
    end_rendering();
    barrier();
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    vkCmdCopyImage(cmd(), t->image.image, VK_IMAGE_LAYOUT_GENERAL, scan.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrier();
    scan_valid_[index] = true;
    if (tracing()) {
        std::fprintf(stderr, "trace: copy to scan %u from 0x%08X fmt 0x%03X %ux%u\n", scan_target, t->address, t->format,
                     width, height);
    }
}

// --------------------------------------------------------- presentation

void Renderer::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical, surface_, &caps));
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, surface_, &count, formats.data());
    // The TV image holds display-ready values: present them unconverted.
    const bool srgb = scan_[0].format == VK_FORMAT_R8G8B8A8_SRGB || scan_[0].format == VK_FORMAT_B8G8R8A8_SRGB;
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        const bool f_srgb = f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB;
        const bool f_unorm = f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM;
        if ((srgb && f_srgb) || (!srgb && f_unorm)) {
            chosen = f;
            break;
        }
    }
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physical, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physical, surface_, &mode_count, modes.data());
    // The display clock paces the title; presentation must not block it.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
    }
    if (mode == VK_PRESENT_MODE_FIFO_KHR) {
        for (VkPresentModeKHR m : modes) {
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
        }
    }
    uint32_t width, height;
    host::drawable_size(width, height);
    if (caps.currentExtent.width != UINT32_MAX) {
        width = caps.currentExtent.width;
        height = caps.currentExtent.height;
    }
    width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
    height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = {width, height};
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR swapchain;
    VK_CHECK(vkCreateSwapchainKHR(ctx_.device, &sci, nullptr, &swapchain));
    if (swapchain_) {
        vkDeviceWaitIdle(ctx_.device);
        vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
    }
    swapchain_ = swapchain;
    swapchain_format_ = chosen.format;
    swapchain_extent_ = sci.imageExtent;
    vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &count, swapchain_images_.data());
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    while (rendered_.size() < count) {
        VkSemaphore s;
        VK_CHECK(vkCreateSemaphore(ctx_.device, &semaphore, nullptr, &s));
        rendered_.push_back(s);
    }
}

void Renderer::present() {
    if (!scan_valid_[0]) return;
    uint32_t width, height;
    host::drawable_size(width, height);
    const bool srgb_scan = scan_[0].format == VK_FORMAT_R8G8B8A8_SRGB;
    const bool srgb_chain = swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format_ == VK_FORMAT_R8G8B8A8_SRGB;
    if (width != swapchain_extent_.width || height != swapchain_extent_.height || srgb_scan != srgb_chain) {
        submit(true);
        create_swapchain();
    }
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(ctx_.device, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        submit(true);
        create_swapchain();
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) check_failed("vkAcquireNextImageKHR", r, __FILE__, __LINE__);
    VkImage image = swapchain_images_[index];
    VkCommandBuffer c = cmd();
    end_rendering();
    const auto transition = [&](VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ib.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        ib.oldLayout = from;
        ib.newLayout = to;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(c, &di);
    };
    transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(c, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    // Letterboxed to the TV image's aspect ratio.
    const Image& scan = scan_[0];
    const double scale = std::min(double(swapchain_extent_.width) / scan.width, double(swapchain_extent_.height) / scan.height);
    const int32_t w = static_cast<int32_t>(scan.width * scale), h = static_cast<int32_t>(scan.height * scale);
    const int32_t x = (static_cast<int32_t>(swapchain_extent_.width) - w) / 2;
    const int32_t y = (static_cast<int32_t>(swapchain_extent_.height) - h) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(scan.width), static_cast<int32_t>(scan.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + w, y + h, 1};
    vkCmdBlitImage(c, scan.image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);
    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    VK_CHECK(vkEndCommandBuffer(cmd_));
    recording_ = false;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
    VK_CHECK(vkQueueSubmit(ctx_.queue, 1, &si, fence_));
    const double begin = now_seconds();
    VK_CHECK(vkWaitForFences(ctx_.device, 1, &fence_, VK_TRUE, UINT64_MAX));
    wait_seconds_ += now_seconds() - begin;
    ++waits_;
    VK_CHECK(vkResetFences(ctx_.device, 1, &fence_));
    VK_CHECK(vkResetCommandPool(ctx_.device, pool_, 0));
    ring_.reset();
    bound_pipeline_ = nullptr;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(ctx_.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vkQueueWaitIdle(ctx_.queue);
        create_swapchain();
    } else if (r != VK_SUCCESS) {
        check_failed("vkQueuePresentKHR", r, __FILE__, __LINE__);
    }
}

// Saves an 8-bit RGBA/BGRA image as a PNG (debugging); waits for the GPU.
void Renderer::save_image(const Image& img, const std::string& path) {
    const bool bgra = img.format == VK_FORMAT_B8G8R8A8_UNORM || img.format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool rgba = img.format == VK_FORMAT_R8G8B8A8_UNORM || img.format == VK_FORMAT_R8G8B8A8_SRGB;
    const bool depth = img.format == VK_FORMAT_D32_SFLOAT || img.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (!bgra && !rgba && !depth) return;
    const VkDeviceSize bytes = VkDeviceSize{img.width} * img.height * 4;
    VkDeviceSize offset = 0;
    upload(bytes, 16, offset);
    const uint8_t* data = ring_.mapped() + offset;
    end_rendering();
    barrier();
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {img.width, img.height, 1};
    vkCmdCopyImageToBuffer(cmd(), img.image, VK_IMAGE_LAYOUT_GENERAL, ring_.buffer(), 1, &region);
    // Wait without resetting the ring, then read.
    VK_CHECK(vkEndCommandBuffer(cmd_));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    VK_CHECK(vkQueueSubmit(ctx_.queue, 1, &si, fence_));
    VK_CHECK(vkWaitForFences(ctx_.device, 1, &fence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(ctx_.device, 1, &fence_));
    VK_CHECK(vkResetCommandPool(ctx_.device, pool_, 0));
    recording_ = false;
    bound_pipeline_ = nullptr;
    std::vector<uint8_t> pixels(data, data + bytes);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (depth) {
            float d;
            std::memcpy(&d, &pixels[i], 4);
            const uint8_t v = static_cast<uint8_t>(std::clamp(d, 0.0f, 1.0f) * 255.0f);
            pixels[i] = pixels[i + 1] = pixels[i + 2] = v;
        } else if (bgra) {
            std::swap(pixels[i], pixels[i + 2]);
        }
        pixels[i + 3] = 255;
    }
    write_png(path, pixels.data(), img.width, img.height);
    ring_.reset();
}

// TTT2_CAPTURE=<directory>: every TTT2_CAPTURE_INTERVAL seconds (default
// 5) the TV and GamePad images are saved as PNG files. The render targets
// of a traced frame (TTT2_TRACE_FRAME/AT) are saved there too.
void Renderer::capture() {
    static const char* dir = [] {
        const char* d = std::getenv("TTT2_CAPTURE");
        return (d != nullptr && *d != '\0') ? d : nullptr;
    }();
    static const double interval = [] {
        const char* v = std::getenv("TTT2_CAPTURE_INTERVAL");
        return v ? std::atof(v) : 5.0;
    }();
    static double last = now_seconds();
    static uint32_t index = 0;
    if (dir != nullptr && tracing()) {
        for (const auto& t : targets_) {
            char name[96];
            std::snprintf(name, sizeof(name), "/frame%llu_%s_%08X_%03X_%ux%u.png",
                          static_cast<unsigned long long>(frame_number_), t->depth ? "depth" : "color", t->address,
                          t->format, t->pitch, t->height);
            save_image(t->image, std::string(dir) + name);
        }
    }
    if (dir == nullptr || now_seconds() - last < interval) return;
    last = now_seconds();
    for (int s = 0; s < 2; ++s) {
        if (!scan_valid_[s]) continue;
        char name[64];
        std::snprintf(name, sizeof(name), "/%s_%04u.png", s == 0 ? "tv" : "drc", index);
        save_image(scan_[s], std::string(dir) + name);
    }
    ++index;
}

void Renderer::swap() {
    capture();
    present();
    submit(true);
    if (tracing()) std::fprintf(stderr, "trace: swap (end of frame %llu)\n", static_cast<unsigned long long>(frame_number_));
    // TTT2_TRACE_AT=seconds: trace the first frame after that time.
    static const double trace_at = [] {
        const char* v = std::getenv("TTT2_TRACE_AT");
        return v ? std::atof(v) : -1.0;
    }();
    static const double started = now_seconds();
    if (trace_at >= 0 && trace_frame_ == UINT64_MAX && now_seconds() - started >= trace_at) {
        trace_frame_ = frame_number_ + 1;
    }
    ++frame_number_;
    ++frames_;
    const double t = now_seconds();
    if (t - window_start_ >= 10.0) {
        save_pipeline_cache();
        std::fprintf(stderr, "ttt2: gpu: %.1f frames/s, %.0f draws per frame (%.0f skipped), %zu targets, %zu textures, "
                             "%zu shaders, %zu pipelines; %.1f GPU waits, %.1f ms waiting per frame\n",
                     frames_ / (t - window_start_), double(draws_) / std::max<uint64_t>(frames_, 1),
                     double(skipped_draws_) / std::max<uint64_t>(frames_, 1), targets_.size(), textures_.size(),
                     shaders_.size(), pipelines_.size(), double(waits_) / std::max<uint64_t>(frames_, 1),
                     wait_seconds_ * 1000.0 / std::max<uint64_t>(frames_, 1));
        window_start_ = t;
        frames_ = draws_ = skipped_draws_ = waits_ = 0;
        wait_seconds_ = 0;
    }
}

} // namespace vk
} // namespace cafe::gpu
