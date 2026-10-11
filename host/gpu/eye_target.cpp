#include "eye_target.h"

namespace vrcore::gpu {

namespace {

VkImageView array_view(Device &dev, VkImage image, VkFormat format, VkImageAspectFlags aspect)
{
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, 1, 0, 2};
    VkImageView view = VK_NULL_HANDLE;
    VRCORE_VK(vkCreateImageView(dev.device, &vci, nullptr, &view), VK_NULL_HANDLE);
    return view;
}

// A 2-layer multisampled attachment that lives only inside the pass: lazily allocated where the GPU
// has that memory (a tiler keeps it on tile and never backs it), else ordinary device memory. `quiet`: a
// failure is the caller's to log (it tries another format).
bool transient_image(Device &dev, const EyeTarget &t, VkFormat format, VkImageUsageFlags usage, VkImage &image,
                     VmaAllocation &mem, const char *what, bool quiet = false)
{
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {t.extent.width, t.extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 2;
    ici.samples = t.samples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci = {};
    aci.usage = dev.lazy_memory ? VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkResult vr = vmaCreateImage(dev.vma, &ici, &aci, &image, &mem, nullptr);
    if (vr != VK_SUCCESS) {
        if (!quiet) log("gpu: %s format %d: vmaCreateImage failed: %d", what, (int)format, (int)vr);
        image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryPropertyFlags flags = 0;
    vmaGetAllocationMemoryProperties(dev.vma, mem, &flags);
    log("gpu: %s %ux%u x2 layers x%d samples, format %d, memory flags 0x%x%s", what, t.extent.width, t.extent.height,
        (int)t.samples, (int)format, (unsigned)flags,
        (flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? " (lazily allocated)" : "");
    return true;
}

}

bool eye_target_create(Device &dev, EyeTarget &t, const EyeTargetDesc &desc)
{
    t.extent = {desc.width, desc.height};
    t.color_format = desc.color_format;
    t.fdm = dev.fdm && !desc.fdm_images.empty();

    // 4x MSAA (every Quest and Apple GPU has it for colour and depth).
    t.samples = desc.samples;
    if (!(dev.props.limits.framebufferColorSampleCounts & dev.props.limits.framebufferDepthSampleCounts & t.samples)) {
        log("gpu: %d samples not offered", (int)t.samples);
        return false;
    }

    if (!transient_image(dev, t, t.color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, t.msaa_color, t.msaa_color_mem,
                         "msaa colour"))
        return false;

    // Depth-stencil (Choice B of the M2 proposal): D24S8, else D32S8, each where the format is a depth-stencil
    // attachment and a transient image of it can be made; D32F without a stencil only as the last resort.
    t.depth_format = VK_FORMAT_UNDEFINED;
    for (VkFormat f : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(dev.phys, f, &fp);
        bool attachment = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (attachment && transient_image(dev, t, f, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, t.msaa_depth,
                                          t.msaa_depth_mem, "msaa depth", true)) {
            t.depth_format = f;
            break;
        }
        log("gpu: depth format %d %s; the next", (int)f,
            attachment ? "refused as a transient MSAA image" : "not a depth-stencil attachment here");
    }
    if (t.depth_format == VK_FORMAT_UNDEFINED) {
        log("gpu: no depth format");
        return false;
    }
    t.stencil = t.depth_format != VK_FORMAT_D32_SFLOAT;
    if (!t.stencil) log("gpu: no stencil in the eye pass: stencil draws will not be masked");
    VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (t.stencil) depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    t.msaa_color_view = array_view(dev, t.msaa_color, t.color_format, VK_IMAGE_ASPECT_COLOR_BIT);
    t.msaa_depth_view = array_view(dev, t.msaa_depth, t.depth_format, depth_aspect);
    if (!t.msaa_color_view || !t.msaa_depth_view) return false;

    // The pass. Nothing is loaded or stored but the resolve; the density map is read as the pass
    // begins and stays in its own layout (the runtime writes it; no barrier from UNDEFINED, which
    // could discard it).
    VkAttachmentDescription att[4] = {};
    att[0].format = t.color_format;
    att[0].samples = t.samples;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1] = att[0];
    att[1].format = t.depth_format;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[2] = att[0];
    att[2].samples = VK_SAMPLE_COUNT_1_BIT;
    att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[3].format = VK_FORMAT_R8G8_UNORM;
    att[3].samples = VK_SAMPLE_COUNT_1_BIT;
    att[3].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[3].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[3].initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    att[3].finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;

    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve_ref = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pResolveAttachments = &resolve_ref;
    sub.pDepthStencilAttachment = &depth_ref;

    // Frames in flight share the MSAA pair: each pass waits for the last one's attachment writes.
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    uint32_t view_mask = 0b11;
    VkRenderPassMultiviewCreateInfo mv = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    mv.subpassCount = 1;
    mv.pViewMasks = &view_mask;
    mv.correlationMaskCount = 1;
    mv.pCorrelationMasks = &view_mask;
    VkRenderPassFragmentDensityMapCreateInfoEXT fdm = {VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
    fdm.fragmentDensityMapAttachment = {3, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
    if (t.fdm) mv.pNext = &fdm;

    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.pNext = &mv;
    rpci.attachmentCount = t.fdm ? 4u : 3u;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    VRCORE_VK(vkCreateRenderPass(dev.device, &rpci, nullptr, &t.pass), false);

    for (size_t i = 0; i < desc.images.size(); ++i) {
        VkImageView v = array_view(dev, desc.images[i], t.color_format, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!v) return false;
        t.views.push_back(v);
        VkImageView fv = VK_NULL_HANDLE;
        if (t.fdm) {
            fv = array_view(dev, desc.fdm_images[i], VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
            if (!fv) return false;
            t.fdm_views.push_back(fv);
        }
        VkImageView list[4] = {t.msaa_color_view, t.msaa_depth_view, v, fv};
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = t.pass;
        fci.attachmentCount = t.fdm ? 4u : 3u;
        fci.pAttachments = list;
        fci.width = t.extent.width;
        fci.height = t.extent.height;
        fci.layers = 1;                         // multiview: the layers come from the view mask
        VkFramebuffer fb = VK_NULL_HANDLE;
        VRCORE_VK(vkCreateFramebuffer(dev.device, &fci, nullptr, &fb), false);
        t.framebuffers.push_back(fb);
    }
    log("gpu: eye target %ux%u x2 views, %d samples, depth format %d (%s), %u images%s", t.extent.width,
        t.extent.height, (int)t.samples, (int)t.depth_format,
        t.depth_format == VK_FORMAT_D24_UNORM_S8_UINT  ? "D24S8"
        : t.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32S8"
                                                        : "D32F, no stencil",
        (unsigned)desc.images.size(),
        t.fdm ? ", fragment density map" : "");
    if (t.fdm) log("gpu: density map %ux%u", desc.fdm_width, desc.fdm_height);
    return true;
}

void eye_target_destroy(Device &dev, EyeTarget &t)
{
    for (VkFramebuffer fb : t.framebuffers) vkDestroyFramebuffer(dev.device, fb, nullptr);
    for (VkImageView v : t.views) vkDestroyImageView(dev.device, v, nullptr);
    for (VkImageView v : t.fdm_views) vkDestroyImageView(dev.device, v, nullptr);
    if (t.pass) vkDestroyRenderPass(dev.device, t.pass, nullptr);
    if (t.msaa_color_view) vkDestroyImageView(dev.device, t.msaa_color_view, nullptr);
    if (t.msaa_depth_view) vkDestroyImageView(dev.device, t.msaa_depth_view, nullptr);
    if (t.msaa_color) vmaDestroyImage(dev.vma, t.msaa_color, t.msaa_color_mem);
    if (t.msaa_depth) vmaDestroyImage(dev.vma, t.msaa_depth, t.msaa_depth_mem);
    t = EyeTarget();
}

}
