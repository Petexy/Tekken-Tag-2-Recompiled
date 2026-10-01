#pragma once

// The Vulkan device the renderer runs on, guest memory as the GPU sees it,
// and the small allocation helpers the renderer uses.
//
// Guest memory (MEM1, MEM2) is imported with VK_EXT_external_memory_host,
// so shaders and transfers read the title's buffers in place and stream-out
// writes land in guest memory: no copies, nothing to keep coherent.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace cafe::gpu::vk {

[[noreturn]] void check_failed(const char* expression, VkResult result, const char* file, int line);
#define VK_CHECK(expression)                                                                                  \
    do {                                                                                                      \
        const VkResult vk_check_result_ = (expression);                                                       \
        if (vk_check_result_ != VK_SUCCESS) ::cafe::gpu::vk::check_failed(#expression, vk_check_result_,       \
                                                                         __FILE__, __LINE__);                 \
    } while (0)

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
};

struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    bool custom_border_color = false;
    bool validation = false;

    PFN_vkGetMemoryHostPointerPropertiesEXT get_memory_host_pointer_properties = nullptr;
    PFN_vkCmdPushDescriptorSetKHR cmd_push_descriptor_set = nullptr;

    // Creates the instance (with the window's surface extensions) and device.
    void init(const std::vector<const char*>& instance_extensions);

    uint32_t memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags);
    void destroy_buffer(Buffer& buffer);
    VkDeviceMemory allocate_image_memory(VkImage image);
};

// Guest memory regions imported as buffers.
class GuestMemory {
public:
    void init(Context& context);
    // The buffer and offset holding guest bytes [address, address + size), or
    // false if the range is not in an imported region.
    bool locate(uint32_t address, uint32_t size, VkBuffer& buffer, VkDeviceSize& offset) const;
    // Device address of a guest address (0 if not imported) and how many bytes
    // from there are in the same region.
    VkDeviceAddress device_address(uint32_t address, uint32_t& available) const;

private:
    struct Region {
        uint32_t base;
        uint32_t size;
        Buffer buffer;
    };
    std::vector<Region> regions_;
};

// Host-visible memory the renderer writes per draw and per transfer
// (constants, indices, staging). Reset once the GPU has finished with it.
class UploadRing {
public:
    void init(Context& context, VkDeviceSize size);
    // Null when full: the caller submits, waits and resets.
    uint8_t* allocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset);
    void reset() { used_ = 0; }
    VkBuffer buffer() const { return buffer_.buffer; }
    VkDeviceAddress address() const { return buffer_.address; }
    VkDeviceSize capacity() const { return buffer_.size; }
    uint8_t* mapped() const { return buffer_.mapped; }

private:
    Buffer buffer_;
    VkDeviceSize used_ = 0;
};

} // namespace cafe::gpu::vk
