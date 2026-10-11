/*
 * vk.h -- Vulkan through volk (every entry point is loaded at run time: libvulkan on the Quest,
 * MoltenVK on the Mac) and VMA. Include this, never <vulkan/vulkan.h>.
 */
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include "../app/log.h"

// Logs the failing call and returns `ret` from the enclosing function.
#define VRCORE_VK(call, ret)                                                              \
    do {                                                                                  \
        VkResult vr_ = (call);                                                            \
        if (vr_ != VK_SUCCESS) {                                                          \
            ::vrcore::log("vulkan: %s failed: %d (%s:%d)", #call, (int)vr_, __FILE__, __LINE__); \
            return ret;                                                                   \
        }                                                                                 \
    } while (0)
