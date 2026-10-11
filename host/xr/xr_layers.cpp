/*
 * xr_layers.cpp -- a front's quad layers on the headset (app/layers.h): each its own swapchain (one 2D sRGB
 * image at a time, the eye swapchain's format), drawn straight into its acquired image by the front's offscreen
 * passes, released after the frame's submit, and submitted after the projection layer every frame it is up
 * (an underlay: before it, seen through layer_punch's hole). A frame that draws no new picture submits the last
 * released image (OpenXR allows it).
 */
#include "xr_host.h"

#include <algorithm>

#include "../app/layers.h"
#include "../app/log.h"
#include "../gpu/vmath.h"

namespace vrcore {

struct Layer {
    LayerDesc desc;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> images;
    std::vector<gpu::Target *> targets;     // one per image
    bool acquired = false;                  // an image is held for this frame's draw (released after its submit)
    bool drawn = false;                     // and it was waited for and handed to the front
    bool has_image = false;                 // a drawn image was released: there is a picture to show
    XrPosef pose = {{0, 0, 0, 1}, {0, 0, 0}};
    XrExtent2Df size = {1, 1};
    bool up = false;
    uint32_t crop[2] = {0, 0};              // the shown part's pixels (layer_crop; 0: the whole picture)
};

namespace xr {

namespace {

Host *g_host = nullptr;
std::vector<Layer *> g_layers;

const uint32_t kPunchVert[] = {
#include "layer_quad.vert.h"
};
const uint32_t kPunchFrag[] = {
#include "layer_punch.frag.h"
};
gpu::Pipeline *g_punch = nullptr;           // made with the first underlay, gone with the last
int g_underlays = 0;

}

void layers_bind(Host &h)
{
    g_host = &h;
}

void layers_release(Host &h)
{
    (void)h;
    for (Layer *l : g_layers) {
        if (!l->acquired) continue;
        XrResult r = xrReleaseSwapchainImage(l->swapchain, nullptr);
        if (XR_FAILED(r)) log("openxr: xrReleaseSwapchainImage (layer): %d", (int)r);
        else l->has_image |= l->drawn;
        l->acquired = l->drawn = false;
    }
}

void layers_compose(Host &h, std::vector<XrCompositionLayerQuad> &under, std::vector<XrCompositionLayerQuad> &over)
{
    under.clear();
    over.clear();
    for (Layer *l : g_layers) {
        if (!l->up || !l->has_image) continue;
        XrCompositionLayerQuad q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = l->desc.opaque ? 0
                                      : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                            XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        q.space = h.local;
        q.eyeVisibility = l->desc.eye == 1   ? XR_EYE_VISIBILITY_LEFT
                          : l->desc.eye == 2 ? XR_EYE_VISIBILITY_RIGHT
                                             : XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = l->swapchain;
        q.subImage.imageRect.extent = {(int32_t)(l->crop[0] ? std::min(l->crop[0], l->desc.width) : l->desc.width),
                                       (int32_t)(l->crop[1] ? std::min(l->crop[1], l->desc.height) : l->desc.height)};
        q.subImage.imageArrayIndex = 0;
        q.pose = l->pose;
        q.size = l->size;
        (l->desc.underlay ? under : over).push_back(q);
    }
}

}

Layer *layer_create(gpu::Renderer &r, const LayerDesc &d)
{
    xr::Host *h = xr::g_host;
    if (!h || !h->session || !d.width || !d.height) {
        log("xr: layer_create %ux%u: no session", d.width, d.height);
        return nullptr;
    }
    Layer *l = new Layer;
    l->desc = d;
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;     // the front's offscreen pipelines' format, whatever the eyes use
    ci.sampleCount = 1;
    ci.width = d.width;
    ci.height = d.height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult xr = xrCreateSwapchain(h->session, &ci, &l->swapchain);
    uint32_t n = 0;
    if (XR_SUCCEEDED(xr)) xr = xrEnumerateSwapchainImages(l->swapchain, 0, &n, nullptr);
    if (XR_SUCCEEDED(xr)) {
        l->images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xr = xrEnumerateSwapchainImages(l->swapchain, n, &n,
                                        reinterpret_cast<XrSwapchainImageBaseHeader *>(l->images.data()));
    }
    if (XR_FAILED(xr)) {
        log("openxr: a layer's swapchain %ux%u: %d", d.width, d.height, (int)xr);
        layer_destroy(r, l);
        return nullptr;
    }
    for (const XrSwapchainImageVulkanKHR &img : l->images) {
        gpu::Target *t = gpu::renderer_wrap_target(*r.impl, img.image, d.width, d.height, VK_FORMAT_R8G8B8A8_SRGB,
                                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (!t) {
            layer_destroy(r, l);
            return nullptr;
        }
        l->targets.push_back(t);
    }
    if (d.underlay && xr::g_underlays++ == 0) {
        gpu::PipelineDesc pd;
        pd.vert = {xr::kPunchVert, sizeof xr::kPunchVert};
        pd.frag = {xr::kPunchFrag, sizeof xr::kPunchFrag};
        pd.topology = gpu::Topology::TriangleStrip;
        pd.cull = gpu::Cull::None;
        pd.depth_test = pd.depth_write = true;  // in front of it stays; behind it is cut
        xr::g_punch = r.create_pipeline(pd);
    }
    xr::g_layers.push_back(l);
    log("xr: layer %ux%u, %u images, %s%s", d.width, d.height, n, d.opaque ? "opaque" : "straight alpha",
        d.underlay ? ", underlay" : "");
    return l;
}

void layer_destroy(gpu::Renderer &r, Layer *l)
{
    if (!l) return;
    bool listed = std::find(xr::g_layers.begin(), xr::g_layers.end(), l) != xr::g_layers.end();
    xr::g_layers.erase(std::remove(xr::g_layers.begin(), xr::g_layers.end(), l), xr::g_layers.end());
    if (listed && l->desc.underlay && --xr::g_underlays == 0) {
        r.destroy_pipeline(xr::g_punch);
        xr::g_punch = nullptr;
    }
    for (gpu::Target *t : l->targets) r.destroy_target(t);
    if (l->acquired) xrReleaseSwapchainImage(l->swapchain, nullptr);
    if (l->swapchain) xrDestroySwapchain(l->swapchain);
    delete l;
}

gpu::Target *layer_target(gpu::Frame &f, Layer *l)
{
    (void)f;
    if (l->acquired) {
        log("xr: a layer drawn twice in a frame; the second draw is not taken");
        return nullptr;
    }
    uint32_t index = 0;
    XrResult r = xrAcquireSwapchainImage(l->swapchain, nullptr, &index);
    if (XR_FAILED(r)) {
        log("openxr: xrAcquireSwapchainImage (layer): %d", (int)r);
        return nullptr;
    }
    l->acquired = true;                     // released after the frame's submit, whatever happens next
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    r = xrWaitSwapchainImage(l->swapchain, &wi);
    if (XR_FAILED(r) || r == XR_TIMEOUT_EXPIRED || index >= l->targets.size()) {
        log("openxr: xrWaitSwapchainImage (layer): %d", (int)r);
        return nullptr;
    }
    l->drawn = true;
    return l->targets[index];
}

void layer_place(Layer *l, const float pos[3], const Quat &rot, float width_m, float height_m, bool up)
{
    l->pose.position = {pos[0], pos[1], pos[2]};
    l->pose.orientation = {rot.x, rot.y, rot.z, rot.w};
    l->size = {width_m, height_m};
    l->up = up;
}

void layer_crop(Layer *l, uint32_t width, uint32_t height)
{
    l->crop[0] = width;
    l->crop[1] = height;
}

void layer_punch(gpu::Frame &f, Layer *l)
{
    // as layers_compose submits it: up, with a picture (or one drawn this frame, released before the submit)
    if (!l || !l->desc.underlay || !xr::g_punch || !l->up || !(l->has_image || l->drawn)) return;
    const XrPosef &p = l->pose;
    struct {
        Mat4 model;
        int32_t eye;
        int32_t pad;
        float crop[2];
    } push = {mat4_mul(mat4_translate(p.position.x, p.position.y, p.position.z),
                       mat4_mul(mat4_rotate(Quat{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}),
                                mat4_scale(l->size.width, l->size.height, 1.f))),
              l->desc.eye, 0, {1.f, 1.f}};
    f.bind(xr::g_punch);
    f.push(&push, sizeof push);
    f.draw(4);
}

}
