// Presentation: finished frames go to the window from a thread of their own.
//
// The command processor copies the TV scan buffer into one of a few
// presentation images and queues it with the time it should appear; the
// presentation thread waits until the GPU has finished it and the time has
// come, then scales it, letterboxed, into the swapchain and presents. So
// the command processor never waits for the display, and with frame
// interpolation the extra frames appear between the real ones at even
// intervals: the extra frames of a frame first, its real image last,
// 1/(59.94 * frames_per_frame_) s apart.
//
// Presentation runs on a queue of its own, a compute queue (AMD's
// asynchronous compute), with a small compute shader doing the scaling: on
// the rendering queue a present waits for everything submitted before it,
// which by then includes the next frame. Without such a queue it falls back
// to a blit on the rendering queue (queue_mutex_ serialises its use).

#include "gpu/vulkan/renderer.h"

#include "host/window.h"

#include "cafe/runtime.h"

#include <algorithm>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shaderc/shaderc.h>
#include <thread>

namespace cafe::gpu::vk {
namespace {

constexpr uint32_t kPresentImages = 8;
// One frame of the title: the console's 59.94 Hz.
constexpr double kFramePeriod = 1001.0 / 60000.0;

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// The compute shader that scales a presentation image into the swapchain.
constexpr const char* kScaleShader = R"(#version 460
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) writeonly uniform image2D target;
layout(push_constant) uniform Rect { ivec2 offset; ivec2 size; ivec2 extent; } rect;
void main() {
    const ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(xy, rect.extent))) return;
    const ivec2 p = xy - rect.offset;
    vec4 c = vec4(0.0, 0.0, 0.0, 1.0);
    if (all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, rect.size))) {
        c = vec4(textureLod(source, (vec2(p) + 0.5) / vec2(rect.size), 0.0).rgb, 1.0);
    }
    imageStore(target, xy, c);
}
)";

void Renderer::start_presenter() {
    // The compute path needs a presenting compute queue and storage writes
    // to the swapchain.
    VkBool32 supported = VK_FALSE;
    if (ctx_.present_queue != VK_NULL_HANDLE) {
        vkGetPhysicalDeviceSurfaceSupportKHR(ctx_.physical, ctx_.present_family, surface_, &supported);
    }
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical, surface_, &caps));
    compute_present_ = supported && ctx_.storage_without_format && (caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) &&
                       !std::getenv("TTT2_PRESENT_BLIT");
    present_family_ = compute_present_ ? ctx_.present_family : ctx_.queue_family;
    present_queue_handle_ = compute_present_ ? ctx_.present_queue : ctx_.queue;
    std::fprintf(stderr, "ttt2: GPU: presenting from %s\n", compute_present_ ? "a compute queue" : "the rendering queue");
    if (compute_present_) {
        shaderc_compile_options_t options = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler_, kScaleShader, std::strlen(kScaleShader),
                                                                       shaderc_compute_shader, "scale", "main", options);
        shaderc_compile_options_release(options);
        if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
            fatal("Vulkan: the presentation shader does not compile: %s", shaderc_result_get_error_message(result));
        }
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = shaderc_result_get_length(result);
        mci.pCode = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(result));
        VkShaderModule module;
        VK_CHECK(vkCreateShaderModule(ctx_.device, &mci, nullptr, &module));
        shaderc_result_release(result);
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        dli.bindingCount = 2;
        dli.pBindings = bindings;
        VkDescriptorSetLayout set_layout;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device, &dli, nullptr, &set_layout));
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(ctx_.device, &pli, nullptr, &scale_layout_));
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main",
                     nullptr};
        cpi.layout = scale_layout_;
        VK_CHECK(vkCreateComputePipelines(ctx_.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &scale_pipeline_));
        vkDestroyShaderModule(ctx_.device, module, nullptr);
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(ctx_.device, &sci, nullptr, &scale_sampler_));
    }
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = present_family_;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx_.device, &pci, nullptr, &present_pool_));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = present_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx_.device, &cai, &present_cmd_));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(ctx_.device, &fci, nullptr, &present_fence_));
    std::thread([this] { presenter_main(); }).detach();
}

// A free presentation image the size and format of the TV scan buffer.
uint32_t Renderer::take_present_image() {
    const Image& scan = scan_[0];
    std::unique_lock lock(present_mutex_);
    // Display-ready values: kept as they are (sRGB data in a UNORM image).
    const VkFormat format = scan.format == VK_FORMAT_R8G8B8A8_SRGB   ? VK_FORMAT_R8G8B8A8_UNORM
                            : scan.format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM
                                                                     : scan.format;
    const bool fits = !present_images_.empty() && present_images_[0].width == scan.width &&
                      present_images_[0].height == scan.height && present_images_[0].format == format;
    if (!fits) {
        // Wait until the presentation thread is done with the old images.
        present_cv_.wait(lock, [&] {
            return present_queue_.empty() && std::all_of(present_free_.begin(), present_free_.end(), [](uint8_t f) { return f; });
        });
        submit(true);
        for (Image& img : present_images_) destroy_image(img);
        present_images_.clear();
        for (uint32_t i = 0; i < kPresentImages; ++i) {
            present_images_.push_back(create_image(format, VK_IMAGE_ASPECT_COLOR_BIT, scan.width, scan.height, 1, 1,
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                       VK_IMAGE_USAGE_SAMPLED_BIT,
                                                   VK_IMAGE_VIEW_TYPE_2D, compute_present_));
        }
        present_free_.assign(kPresentImages, 1);
    }
    present_cv_.wait(lock, [&] { return std::find(present_free_.begin(), present_free_.end(), 1) != present_free_.end(); });
    const uint32_t i = static_cast<uint32_t>(std::find(present_free_.begin(), present_free_.end(), 1) - present_free_.begin());
    present_free_[i] = 0;
    return i;
}

// Copies the TV scan buffer into a presentation image; `ready` is the
// timeline value at which the copy has finished.
uint32_t Renderer::copy_scan_to_present_image(uint64_t& ready) {
    const uint32_t i = take_present_image();
    end_rendering();
    barrier();
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {scan_[0].width, scan_[0].height, 1};
    vkCmdCopyImage(cmd(), scan_[0].image, VK_IMAGE_LAYOUT_GENERAL, present_images_[i].image, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &region);
    ready = submit(false);
    return i;
}

void Renderer::queue_present(uint32_t image, double time, uint64_t ready) {
    std::lock_guard lock(present_mutex_);
    present_queue_.push_back({image, time, ready, slot_reference_});
    present_cv_.notify_all();
}

// Frame pacing. Every frame the title renders gets a display slot: one
// title frame (1/59.94 s) after the previous slot, or, if the title fell
// behind, a lead time after its swap. Its extra frames show in the slot at
// even steps and its real image last, so the screen gets a frame every
// 1/(59.94 K) s however unevenly the frames finish. The lead time is the
// time a frame needs to finish on the GPU: it grows when the presentation
// thread finds a frame not ready at its time and shrinks slowly while every
// frame is early.
double Renderer::frame_slot() {
    const double now = now_seconds();
    slot_reference_ = now;
    const double lead = present_lead_.load();
    double slot = last_slot_ + kFramePeriod;
    if (slot < now + lead - kFramePeriod / 2 || slot > now + lead + kFramePeriod) slot = now + lead; // behind, or a stall
    slot = std::max(slot, now + lead * 0.5);
    last_slot_ = slot;
    return slot;
}

// The title finished a frame (swap): shown in its slot, or, with
// interpolation, after the extra frames the replays render.
void Renderer::show_frame() {
    if (!scan_valid_[0]) return;
    uint64_t ready = 0;
    const uint32_t image = copy_scan_to_present_image(ready);
    if (frames_per_frame_ == 1) {
        queue_present(image, frame_slot(), ready);
        return;
    }
    if (pending_real_ != UINT32_MAX) queue_present(pending_real_, frame_slot(), pending_ready_); // a frame without replays
    pending_real_ = image;
    pending_ready_ = ready;
    extra_frames_ = 0;
}

// A replay finished: the extra frames go out first in the frame's slot,
// 1/K of a frame apart.
void Renderer::show_extra_frame() {
    if (!scan_valid_[0]) return;
    // TTT2_CAPTURE: the extra frames of a captured frame as tvx_<index>_<n>.png.
    if (captured_frame_ >= 0 && static_cast<uint64_t>(captured_frame_) + 1 == frame_number_) {
        if (const char* dir = std::getenv("TTT2_CAPTURE")) {
            char name[64];
            std::snprintf(name, sizeof(name), "/tvx_%04u_%u.png", captured_index_, extra_frames_ + 1);
            save_image(scan_[0], std::string(dir) + name);
        }
    }
    uint64_t ready = 0;
    const uint32_t image = copy_scan_to_present_image(ready);
    if (extra_frames_ == 0) frame_base_time_ = frame_slot();
    queue_present(image, frame_base_time_ + extra_frames_ * kFramePeriod / frames_per_frame_, ready);
    ++extra_frames_;
}

void Renderer::finish_frame_presentation() {
    if (pending_real_ == UINT32_MAX) return;
    const double time = extra_frames_ ? frame_base_time_ + extra_frames_ * kFramePeriod / frames_per_frame_ : frame_slot();
    queue_present(pending_real_, time, pending_ready_);
    pending_real_ = UINT32_MAX;
}

// ---------------------------------------------------------- presentation thread

// Pacing statistics, every ten seconds: the spacing of presents and how late
// they are against their scheduled time (presentation thread only).
void Renderer::note_presentation(double scheduled, double presented) {
    static std::vector<double> intervals;
    static double last = 0, window = presented, late_total = 0;
    static uint32_t late = 0;
    if (last != 0) intervals.push_back((presented - last) * 1000.0);
    last = presented;
    const double lateness = (presented - scheduled) * 1000.0;
    late_total += std::max(lateness, 0.0);
    if (lateness > 2.0) ++late;
    if (presented - window < 10.0 || intervals.empty()) return;
    std::vector<double> sorted = intervals;
    std::sort(sorted.begin(), sorted.end());
    double mean = 0;
    for (double v : sorted) mean += v;
    mean /= sorted.size();
    const auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()))]; };
    std::fprintf(stderr,
                 "ttt2: present: %.1f/s, interval mean %.2f ms, 5%% %.2f, 50%% %.2f, 95%% %.2f, max %.2f; "
                 "%u more than 2 ms late, %.2f ms late on average; lead %.1f ms\n",
                 sorted.size() / (presented - window), mean, pct(0.05), pct(0.5), pct(0.95), sorted.back(), late,
                 late_total / (sorted.size() + 1), present_lead_.load() * 1000.0);
    intervals.clear();
    late = 0;
    late_total = 0;
    window = presented;
}

void Renderer::presenter_main() {
    pthread_setname_np(pthread_self(), "present");
    for (;;) {
        QueuedFrame f;
        {
            std::unique_lock lock(present_mutex_);
            present_cv_.wait(lock, [&] { return !present_queue_.empty(); });
            f = present_queue_.front();
            present_queue_.pop_front();
        }
        // Finished on the GPU first, then shown at its time.
        uint64_t done = 0;
        VK_CHECK(vkGetSemaphoreCounterValue(ctx_.device, timeline_, &done));
        const bool waited = done < f.ready;
        if (waited) {
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &timeline_;
            wi.pValues = &f.ready;
            VK_CHECK(vkWaitSemaphores(ctx_.device, &wi, 10'000'000'000ull));
        }
        const double ready_at = now_seconds();
        // The lead is how long frames take from their swap until the GPU has
        // finished them: the 98th percentile of the last 240, plus a
        // millisecond (stalls over 30 ms, such as loading, are not measures).
        static std::vector<double> needs;
        static size_t next_need = 0;
        // (A frame already finished when its turn came needed no more lead.)
        const double need = waited ? ready_at - f.reference : 0.0;
        if (need >= 0 && need < 0.030) {
            if (needs.size() < 240) needs.push_back(need);
            else needs[next_need++ % needs.size()] = need;
            if (needs.size() >= 30 && next_need % 30 == 0) {
                std::vector<double> sorted = needs;
                std::sort(sorted.begin(), sorted.end());
                const double p98 = sorted[sorted.size() * 98 / 100];
                present_lead_.store(std::clamp(p98 + 0.001, 0.001, 0.0167));
                static const bool trace = std::getenv("TTT2_TRACE_PACING") != nullptr;
                if (trace && next_need % 240 == 0) {
                    const auto at = [&](int p) { return sorted[std::min(sorted.size() - 1, sorted.size() * p / 100)] * 1000; };
                    size_t zero = 0;
                    for (double v : sorted) zero += v == 0;
                    std::fprintf(stderr, "pacing: need 50%% %.2f 90%% %.2f 98%% %.2f 99%% %.2f max %.2f ms; %zu/%zu ready in time\n",
                                 at(50), at(90), at(98), at(99), sorted.back() * 1000, zero, sorted.size());
                }
            }
        }
        const double wait = f.time - now_seconds();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wait, 0.1)));
        present_image(f.image, f.ready);
        note_presentation(f.time, now_seconds());
        {
            std::lock_guard lock(present_mutex_);
            present_free_[f.image] = 1;
            present_cv_.notify_all();
        }
    }
}

void Renderer::create_swapchain(VkFormat image_format) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical, surface_, &caps));
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, surface_, &count, formats.data());
    // The TV image holds display-ready values: present them unconverted
    // (a UNORM swapchain written by the scaling shader, or for the blit one
    // of the image's own kind).
    const bool srgb = !compute_present_ && (image_format == VK_FORMAT_R8G8B8A8_SRGB || image_format == VK_FORMAT_B8G8R8A8_SRGB);
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        const bool f_srgb = f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB;
        const bool f_unorm = f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM;
        const bool storage = !compute_present_ || (ctx_.format_features(f.format) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        if (((srgb && f_srgb) || (!srgb && f_unorm)) && storage) {
            chosen = f;
            break;
        }
    }
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physical, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_.physical, surface_, &mode_count, modes.data());
    // Frames are presented at chosen times: show each at the next refresh.
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
    sci.imageUsage = compute_present_ ? VK_IMAGE_USAGE_STORAGE_BIT
                                      : VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain_;
    VkSwapchainKHR swapchain;
    VK_CHECK(vkCreateSwapchainKHR(ctx_.device, &sci, nullptr, &swapchain));
    if (swapchain_) {
        if (compute_present_) {
            vkQueueWaitIdle(present_queue_handle_);
        } else {
            std::lock_guard lock(queue_mutex_);
            vkQueueWaitIdle(present_queue_handle_);
        }
        for (VkImageView v : swapchain_views_) vkDestroyImageView(ctx_.device, v, nullptr);
        swapchain_views_.clear();
        vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
    }
    swapchain_ = swapchain;
    swapchain_format_ = chosen.format;
    swapchain_extent_ = sci.imageExtent;
    vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(ctx_.device, swapchain_, &count, swapchain_images_.data());
    if (compute_present_) {
        for (VkImage image : swapchain_images_) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = chosen.format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view;
            VK_CHECK(vkCreateImageView(ctx_.device, &vci, nullptr, &view));
            swapchain_views_.push_back(view);
        }
    }
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (acquired_ == VK_NULL_HANDLE) VK_CHECK(vkCreateSemaphore(ctx_.device, &semaphore, nullptr, &acquired_));
    while (rendered_.size() < count) {
        VkSemaphore s;
        VK_CHECK(vkCreateSemaphore(ctx_.device, &semaphore, nullptr, &s));
        rendered_.push_back(s);
    }
}

void Renderer::present_image(uint32_t i, uint64_t ready) {
    const Image& src = present_images_[i];
    uint32_t width, height;
    host::drawable_size(width, height);
    if (width == 0 || height == 0) return; // minimised
    const bool srgb_image = src.format == VK_FORMAT_R8G8B8A8_SRGB || src.format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool srgb_chain = swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB || swapchain_format_ == VK_FORMAT_R8G8B8A8_SRGB;
    if (swapchain_ == VK_NULL_HANDLE || width != swapchain_extent_.width || height != swapchain_extent_.height ||
        (!compute_present_ && srgb_image != srgb_chain)) {
        create_swapchain(src.format);
    }
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(ctx_.device, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        create_swapchain(src.format);
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) check_failed("vkAcquireNextImageKHR", r, __FILE__, __LINE__);
    VkImage image = swapchain_images_[index];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(present_cmd_, &bi));
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
        vkCmdPipelineBarrier2(present_cmd_, &di);
    };
    // Letterboxed to the TV image's aspect ratio.
    const double scale = std::min(double(swapchain_extent_.width) / src.width, double(swapchain_extent_.height) / src.height);
    const int32_t w = static_cast<int32_t>(src.width * scale), h = static_cast<int32_t>(src.height * scale);
    const int32_t x = (static_cast<int32_t>(swapchain_extent_.width) - w) / 2;
    const int32_t y = (static_cast<int32_t>(swapchain_extent_.height) - h) / 2;
    if (compute_present_) {
        transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(present_cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, scale_pipeline_);
        VkDescriptorImageInfo source{scale_sampler_, src.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo target{VK_NULL_HANDLE, swapchain_views_[index], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &source;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &target;
        ctx_.cmd_push_descriptor_set(present_cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, scale_layout_, 0, 2, writes);
        const int32_t rect[6] = {x, y, w, h, static_cast<int32_t>(swapchain_extent_.width),
                                 static_cast<int32_t>(swapchain_extent_.height)};
        vkCmdPushConstants(present_cmd_, scale_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rect, rect);
        vkCmdDispatch(present_cmd_, (swapchain_extent_.width + 15) / 16, (swapchain_extent_.height + 15) / 16, 1);
        transition(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(present_cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(src.width), static_cast<int32_t>(src.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        vkCmdBlitImage(present_cmd_, src.image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
        transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    VK_CHECK(vkEndCommandBuffer(present_cmd_));

    // After the swapchain image is free and the frame's copy has finished.
    const VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    const VkSemaphore waits[2] = {acquired_, timeline_};
    const uint64_t wait_values[2] = {0, ready};
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = 2;
    ti.pWaitSemaphoreValues = wait_values;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ti;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = wait_stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &present_cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    if (compute_present_) {
        VK_CHECK(vkQueueSubmit(present_queue_handle_, 1, &si, present_fence_));
        r = vkQueuePresentKHR(present_queue_handle_, &pi);
    } else {
        std::lock_guard lock(queue_mutex_);
        VK_CHECK(vkQueueSubmit(present_queue_handle_, 1, &si, present_fence_));
        r = vkQueuePresentKHR(present_queue_handle_, &pi);
    }
    VK_CHECK(vkWaitForFences(ctx_.device, 1, &present_fence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(ctx_.device, 1, &present_fence_));
    VK_CHECK(vkResetCommandPool(ctx_.device, present_pool_, 0));
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        create_swapchain(src.format);
    } else if (r != VK_SUCCESS) {
        check_failed("vkQueuePresentKHR", r, __FILE__, __LINE__);
    }
}

} // namespace cafe::gpu::vk
