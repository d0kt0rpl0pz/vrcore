/*
 * eye_target.h -- where both eyes are drawn in one multiview pass (viewMask 0b11): 4x MSAA colour and
 * depth in transient, lazily allocated images (one pair, shared by every framebuffer: the pass's
 * external dependencies order frames that overlap), resolved inside the pass into a 2-layer array
 * image (the XR swapchain's, or the sim's own), plus the fragment density map when foveation is on.
 *
 *   attachment 0  colour, MSAA, CLEAR, store DONT_CARE
 *   attachment 1  depth-stencil, MSAA, CLEAR, DONT_CARE; reverse-Z depth cleared to 0, stencil to 0.
 *                 D24_UNORM_S8_UINT (the Quest: the true-depth layer pulls were tuned
 *                 for its step), else D32_SFLOAT_S8_UINT (Apple has no D24S8), each only where the format can
 *                 be a lazily allocated MSAA attachment; logged
 *   attachment 2  resolve: the array image, load DONT_CARE, STORE; UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
 *   attachment 3  FDM (foveation only): R8G8_UNORM, 2 layers, FRAGMENT_DENSITY_MAP_OPTIMAL throughout
 *
 * Host-internal.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "device.h"

namespace vrcore::gpu {

struct EyeTargetDesc {
    uint32_t width = 0, height = 0;          // one eye
    VkFormat color_format = VK_FORMAT_UNDEFINED;   // the array images' (sRGB)
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_4_BIT;
    std::vector<VkImage> images;             // 2-layer array images, one framebuffer each
    std::vector<VkImage> fdm_images;         // empty: no foveation; else one per image
    uint32_t fdm_width = 0, fdm_height = 0;
};

struct EyeTarget {
    VkRenderPass pass = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    VkFormat color_format = VK_FORMAT_UNDEFINED, depth_format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool fdm = false;
    bool stencil = false;                    // the depth format has a stencil

    VkImage msaa_color = VK_NULL_HANDLE, msaa_depth = VK_NULL_HANDLE;
    VmaAllocation msaa_color_mem = VK_NULL_HANDLE, msaa_depth_mem = VK_NULL_HANDLE;
    VkImageView msaa_color_view = VK_NULL_HANDLE, msaa_depth_view = VK_NULL_HANDLE;

    std::vector<VkImageView> views;          // per image: 2D_ARRAY, 2 layers
    std::vector<VkImageView> fdm_views;
    std::vector<VkFramebuffer> framebuffers;
};

bool eye_target_create(Device &dev, EyeTarget &t, const EyeTargetDesc &desc);
void eye_target_destroy(Device &dev, EyeTarget &t);

}
