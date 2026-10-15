/*
 * xr_layers.cpp -- a front's quad layers on the headset (app/layers.h): each its own swapchain (one 2D sRGB
 * image at a time, the eye swapchain's format), drawn straight into its acquired image by the front's offscreen
 * passes, released after the frame's submit, and submitted after the projection layer every frame it is up
 * (an underlay: before it, seen through layer_punch's hole). A frame that draws no new picture submits the last
 * released image (OpenXR allows it).
 *
 * And the overlay (app/layers.h): a second projection layer over the quads, on its own 2-layer swapchain of the
 * eye swapchain's size, format and density maps, so its eye target's render pass matches the eye pass's and every
 * eye pipeline draws on it. Made on the first frame that asks, kept until the eyes are made again; drawn and
 * submitted only on frames that ask.
 */
#include "xr_host.h"

#include <algorithm>
#include <functional>
#include <utility>

#include "../app/layers.h"
#include "../app/log.h"
#include "../gpu/render_host.h"
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

std::vector<std::function<void(gpu::Frame &)>> g_overlay;     // this frame's overlay draws (overlay_draw)

// The overlay's swapchain and eye target; false (logged, h.overlay_failed): none until the eyes are made again.
bool overlay_make(Host &h)
{
    if (h.overlay) return true;
    if (h.overlay_failed) return false;
    h.overlay_failed = true;                // until it is made
    XrSwapchainCreateInfoFoveationFB fov = {XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
    fov.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
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
    VRCORE_XR(xrCreateSwapchain(h.session, &ci, &h.overlay), false);
    uint32_t n = 0;
    XrResult r = xrEnumerateSwapchainImages(h.overlay, 0, &n, nullptr);
    if (XR_SUCCEEDED(r)) {
        h.overlay_images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        if (h.swapchain_fdm) {
            h.overlay_fdm_images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
            for (uint32_t i = 0; i < n; ++i) h.overlay_images[i].next = &h.overlay_fdm_images[i];
        }
        r = xrEnumerateSwapchainImages(h.overlay, n, &n,
                                       reinterpret_cast<XrSwapchainImageBaseHeader *>(h.overlay_images.data()));
    }
    gpu::EyeTargetDesc desc;
    desc.width = h.width;
    desc.height = h.height;
    desc.color_format = h.format;
    bool maps = true;
    for (uint32_t i = 0; XR_SUCCEEDED(r) && i < n; ++i) {
        desc.images.push_back(h.overlay_images[i].image);
        if (!h.swapchain_fdm) continue;
        maps = maps && h.overlay_fdm_images[i].image;
        desc.fdm_images.push_back(h.overlay_fdm_images[i].image);
    }
    if (h.swapchain_fdm && maps && n) {
        desc.fdm_width = h.overlay_fdm_images[0].width;
        desc.fdm_height = h.overlay_fdm_images[0].height;
    }
    // without its density maps the pass would not match the eye pass's, and the eye pipelines could not draw on it
    if (XR_FAILED(r) || !maps || !gpu::eye_target_create(h.dev, h.overlay_target, desc)) {
        log("openxr: the overlay could not be made (%d%s); none", (int)r, maps ? "" : ", no density maps");
        overlay_destroy(h);
        h.overlay_failed = true;
        return false;
    }
    h.overlay_failed = false;
    if (h.swapchain_fdm && h.foveation >= 0) foveation_set(h, h.foveation);
    log("xr: overlay %ux%u x2 layers, %u images%s", h.width, h.height, n, h.swapchain_fdm ? ", density maps" : "");
    return true;
}

}

void layers_bind(Host &h)
{
    g_host = &h;
}

void layers_release(Host &h)
{
    if (h.overlay_acquired) {
        XrResult r = xrReleaseSwapchainImage(h.overlay, nullptr);
        if (XR_FAILED(r)) {
            log("openxr: xrReleaseSwapchainImage (overlay): %d", (int)r);
            h.overlay_drawn = false;
        }
        h.overlay_acquired = false;
    }
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

void overlay_render(Host &h, gpu::Frame &f)
{
    h.overlay_drawn = false;
    if (g_overlay.empty()) return;
    std::vector<std::function<void(gpu::Frame &)>> draws;
    draws.swap(g_overlay);                  // forgotten whatever happens
    if (!overlay_make(h)) return;
    uint32_t index = 0;
    XrResult r = xrAcquireSwapchainImage(h.overlay, nullptr, &index);
    if (XR_FAILED(r)) {
        log("openxr: xrAcquireSwapchainImage (overlay): %d", (int)r);
        return;
    }
    h.overlay_acquired = true;              // released after the frame's submit (layers_release)
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    r = xrWaitSwapchainImage(h.overlay, &wi);
    if (XR_FAILED(r) || r == XR_TIMEOUT_EXPIRED) {
        // not waited on: releasing it would be out of order (XR_ERROR_CALL_ORDER_INVALID), so layers_release leaves it
        log("openxr: xrWaitSwapchainImage (overlay): %d", (int)r);
        h.overlay_acquired = false;
        return;
    }
    if (index >= h.overlay_target.framebuffers.size()) {
        // waited on, so it is still released after the submit; only nothing is drawn into it
        log("openxr: overlay image %u past its %zu framebuffers", index, h.overlay_target.framebuffers.size());
        return;
    }
    static const float kTransparent[4] = {0, 0, 0, 0};
    gpu::renderer_begin_eye_pass(h.r, index, kTransparent, &h.overlay_target);
    for (const auto &draw : draws) draw(f);
    gpu::renderer_end_pass(h.r);
    h.overlay_drawn = true;
}

bool overlay_compose(Host &h, const XrCompositionLayerProjectionView eyes[2], XrCompositionLayerProjection &layer,
                     XrCompositionLayerProjectionView views[2])
{
    if (!h.overlay_drawn) return false;
    h.overlay_drawn = false;
    for (int i = 0; i < 2; ++i) {
        views[i] = eyes[i];
        views[i].next = nullptr;
        views[i].subImage.swapchain = h.overlay;
    }
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    // premultiplied: cleared to transparent black, opaque draws cover, MSAA edges blend
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = h.local;
    layer.viewCount = 2;
    layer.views = views;
    return true;
}

void overlay_destroy(Host &h)
{
    if (h.dev.device) gpu::eye_target_destroy(h.dev, h.overlay_target);
    if (h.overlay_acquired) xrReleaseSwapchainImage(h.overlay, nullptr);
    if (h.overlay) xrDestroySwapchain(h.overlay);
    h.overlay = XR_NULL_HANDLE;
    h.overlay_images.clear();
    h.overlay_fdm_images.clear();
    h.overlay_failed = h.overlay_acquired = h.overlay_drawn = false;
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

void overlay_draw(std::function<void(gpu::Frame &f)> draw)
{
    if (draw) xr::g_overlay.push_back(std::move(draw));
}

}
