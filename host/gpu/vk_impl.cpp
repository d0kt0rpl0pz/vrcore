/*
 * vk_impl.cpp -- Vulkan Memory Allocator's implementation. Every Vulkan entry point comes from volk at
 * run time, so VMA fetches its own from vkGetInstanceProcAddr / vkGetDeviceProcAddr (device.cpp hands
 * them over) and links none statically.
 */
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include "vk.h"
