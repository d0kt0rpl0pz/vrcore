/*
 * device.h -- the Vulkan device: instance, physical device, device, one graphics queue, VMA and a
 * command pool. The caller says how the instance and device are made: OpenXR makes them through
 * XR_KHR_vulkan_enable2 (xr/), the desktop sim makes them itself with SDL's surface extensions (sim/).
 * Host-internal: a front never includes this.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "vk.h"

namespace vrcore::gpu {

struct DeviceHooks {
    // Vulkan's entry point. Null: volkInitialize() finds the system's loader (libvulkan.so on Android).
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;

    // Extensions the caller needs on top of the host's own (portability and FDM are added here when
    // offered / asked).
    std::vector<const char *> instance_extensions;
    std::vector<const char *> device_extensions;

    // Ask for VK_EXT_fragment_density_map with fragmentDensityMap and
    // fragmentDensityMapNonSubsampledImages; enabled only when the device has both (Device::fdm says).
    bool want_fdm = false;

    // Optional overrides (OpenXR). Each gets the create info the host built and returns a VkResult.
    void *user = nullptr;
    VkResult (*create_instance)(void *user, const VkInstanceCreateInfo *ci, VkInstance *out) = nullptr;
    VkResult (*pick_physical_device)(void *user, VkInstance instance, VkPhysicalDevice *out) = nullptr;
    VkResult (*create_device)(void *user, VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, VkDevice *out) = nullptr;
};

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator vma = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;     // RESET_COMMAND_BUFFER, on queue_family
    VkPhysicalDeviceProperties props = {};
    bool fdm = false;                        // fragment density map enabled
    bool lazy_memory = false;                // a LAZILY_ALLOCATED memory type exists
    bool portability = false;                // VK_KHR_portability_subset enabled (MoltenVK)
    bool clip_distance = false;              // shaderClipDistance enabled
    bool gather_extended = false;            // shaderImageGatherExtended enabled
    bool astc_ldr = false;                   // textureCompressionASTC_LDR enabled
};

// The API version the host asks for (and OpenXR's requirements are checked against).
constexpr uint32_t kApiVersion = VK_API_VERSION_1_1;

bool device_create(Device &dev, const DeviceHooks &hooks, const char *app_name);
void device_destroy(Device &dev);

// A one-off command buffer, submitted and waited for (uploads, layout set-up).
VkCommandBuffer device_begin_once(Device &dev);
bool device_end_once(Device &dev, VkCommandBuffer cmd);

}
