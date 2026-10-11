/*
 * xr_swapchain.cpp -- the eye swapchain: one, arraySize 2 (a layer per eye, drawn by one multiview pass),
 * sRGB, one sample (the MSAA is resolved into it inside the pass), at the recommended size x the eye
 * scale. With foveation it is made with the fragment-density-map flag and each image comes with its
 * density map (XR_FB_foveation_vulkan); the runtime fills the maps, xrUpdateSwapchainFB picks the level.
 * A new eye resolution makes it again inside the running session (the old one destroyed first, the GPU idle).
 */
#include "xr_host.h"

#include <chrono>
#include <cmath>
#include <string>

#include "../app/log.h"

namespace vrcore::xr {

namespace {

// The swapchain, with density maps when `fdm`. Sets h.swapchain_fdm only when every image has its map.
bool create(Host &h, bool fdm)
{
    uint32_t n = 0;
    VRCORE_XR(xrEnumerateSwapchainFormats(h.session, 0, &n, nullptr), false);
    std::vector<int64_t> formats(n);
    VRCORE_XR(xrEnumerateSwapchainFormats(h.session, n, &n, formats.data()), false);
    std::string list;
    for (int64_t f : formats) list += " " + std::to_string(f);
    h.format = VK_FORMAT_UNDEFINED;
    for (VkFormat want : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB})
        for (int64_t f : formats)
            if (h.format == VK_FORMAT_UNDEFINED && f == want) h.format = want;
    log("openxr: swapchain formats:%s; using %d", list.c_str(), (int)h.format);
    if (h.format == VK_FORMAT_UNDEFINED) {
        log("openxr: no sRGB swapchain format");
        return false;
    }

    const XrViewConfigurationView &v = h.config_views[0];
    h.width = (uint32_t)std::lround(v.recommendedImageRectWidth * h.opt->eye_scale);
    h.height = (uint32_t)std::lround(v.recommendedImageRectHeight * h.opt->eye_scale);
    if (h.width > v.maxImageRectWidth) h.width = v.maxImageRectWidth;
    if (h.height > v.maxImageRectHeight) h.height = v.maxImageRectHeight;

    XrSwapchainCreateInfoFoveationFB fov = {XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
    fov.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
    h.swapchain_fdm = fdm;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.next = h.swapchain_fdm ? &fov : nullptr;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = h.format;
    ci.sampleCount = 1;
    ci.width = h.width;
    ci.height = h.height;
    ci.faceCount = 1;
    ci.arraySize = 2;
    ci.mipCount = 1;
    VRCORE_XR(xrCreateSwapchain(h.session, &ci, &h.swapchain), false);

    VRCORE_XR(xrEnumerateSwapchainImages(h.swapchain, 0, &n, nullptr), false);
    h.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (h.swapchain_fdm) {
        h.fdm_images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
        for (uint32_t i = 0; i < n; ++i) h.images[i].next = &h.fdm_images[i];
    }
    VRCORE_XR(xrEnumerateSwapchainImages(h.swapchain, n, &n,
                                         reinterpret_cast<XrSwapchainImageBaseHeader *>(h.images.data())), false);
    log("openxr: eye swapchain %ux%u x2 layers (x%.2f of %ux%u), %u images%s", h.width, h.height, h.opt->eye_scale,
        v.recommendedImageRectWidth, v.recommendedImageRectHeight, n,
        h.swapchain_fdm ? ", with density maps" : (h.ext.foveation ? ", no density maps" : ""));
    if (h.swapchain_fdm) {
        for (uint32_t i = 0; i < n; ++i)
            if (!h.fdm_images[i].image) {
                log("openxr: image %u came without a density map", i);
                h.swapchain_fdm = false;
                return true;
            }
        log("openxr: density maps %ux%u", h.fdm_images[0].width, h.fdm_images[0].height);
    }
    return true;
}

}

bool swapchain_create(Host &h)
{
    bool fdm = h.ext.foveation && h.dev.fdm;
    if (!create(h, fdm)) return false;
    if (fdm && !h.swapchain_fdm) {
        // Its images may still be subsampled, and the eye pass would have no density map to match:
        // made again without the foveation flag.
        log("openxr: eye swapchain made again without foveation");
        swapchain_destroy(h);
        if (!create(h, false)) return false;
    }
    if (h.ext.foveation && !h.dev.fdm) log("openxr: the device has no FDM; no foveation");
    return true;
}

void swapchain_destroy(Host &h)
{
    if (h.swapchain) xrDestroySwapchain(h.swapchain);
    h.swapchain = XR_NULL_HANDLE;
    h.images.clear();
    h.fdm_images.clear();
}

}

namespace vrcore::xr {

bool eye_target_make(Host &h)
{
    gpu::EyeTargetDesc desc;
    desc.width = h.width;
    desc.height = h.height;
    desc.color_format = h.format;
    for (const XrSwapchainImageVulkanKHR &i : h.images) desc.images.push_back(i.image);
    if (h.swapchain_fdm) {
        for (const XrSwapchainImageFoveationVulkanFB &f : h.fdm_images) desc.fdm_images.push_back(f.image);
        desc.fdm_width = h.fdm_images[0].width;
        desc.fdm_height = h.fdm_images[0].height;
    }
    return gpu::eye_target_create(h.dev, h.target, desc);
}

bool eye_remake(Host &h)
{
    auto t0 = std::chrono::steady_clock::now();
    vkDeviceWaitIdle(h.dev.device);
    gpu::eye_target_destroy(h.dev, h.target);
    bool fdm = h.swapchain_fdm;
    swapchain_destroy(h);
    if (!swapchain_create(h)) return false;
    if (h.swapchain_fdm != fdm) {           // the renderer's eye pipelines were made for the old render pass
        log("xr: the remade eye swapchain %s density maps; cannot go on", fdm ? "lost its" : "gained");
        return false;
    }
    if (!eye_target_make(h)) return false;
    if (h.swapchain_fdm && h.foveation >= 0) foveation_set(h, h.foveation);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log("xr: eye remade: %ux%u (x%.2f) in %.1f ms", h.width, h.height, h.opt->eye_scale, ms);
    return true;
}

}
