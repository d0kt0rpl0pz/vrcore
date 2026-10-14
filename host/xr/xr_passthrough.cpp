/*
 * xr_passthrough.cpp -- passthrough (app.h's set_passthrough): the room through the headset's cameras where the eye
 * picture's alpha is below 1. The way is picked once at start, never by device: the core blend mode
 * XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND when the system lists it (the portable path), else XR_FB_passthrough (its
 * layer first in the frame, under the eyes and every quad, the blend mode opaque), else none. --passthrough-fb picks
 * FB over ALPHA_BLEND, to test that path. Off until a front asks: a front that never asks ends every frame as before.
 * The FB objects are made the first time it is turned on and paused when it goes off; a failure is logged and leaves
 * passthrough unavailable for the run.
 */
#include "xr_host.h"

#include <string>

#include "../app/log.h"

namespace vrcore::xr {

namespace {

const char *blend_name(XrEnvironmentBlendMode m)
{
    switch (m) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE: return "OPAQUE";
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE: return "ADDITIVE";
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return "ALPHA_BLEND";
    default: return "?";
    }
}

const char *way_name(Passthrough p)
{
    return p == Passthrough::AlphaBlend ? "alpha blend" : p == Passthrough::FB ? "XR_FB_passthrough" : "none";
}

void fb_destroy(Host &h)
{
    if (h.fb_passthrough_layer) h.destroy_passthrough_layer(h.fb_passthrough_layer);
    if (h.fb_passthrough) h.destroy_passthrough(h.fb_passthrough);
    h.fb_passthrough_layer = XR_NULL_HANDLE;
    h.fb_passthrough = XR_NULL_HANDLE;
}

// The FB objects made (the first time) and started. False (logged): passthrough unavailable from now on.
bool fb_start(Host &h)
{
    XrResult r = XR_SUCCESS;
    const char *step = "";
    if (!h.fb_passthrough) {                // made into locals: a failed create's output is not trusted
        XrPassthroughCreateInfoFB ci = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        XrPassthroughFB pt = XR_NULL_HANDLE;
        step = "xrCreatePassthroughFB";
        r = h.create_passthrough(h.session, &ci, &pt);
        if (XR_SUCCEEDED(r)) {
            h.fb_passthrough = pt;
            XrPassthroughLayerCreateInfoFB li = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            li.passthrough = pt;
            li.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            XrPassthroughLayerFB layer = XR_NULL_HANDLE;
            step = "xrCreatePassthroughLayerFB";
            r = h.create_passthrough_layer(h.session, &li, &layer);
            if (XR_SUCCEEDED(r)) h.fb_passthrough_layer = layer;
        }
    }
    if (XR_SUCCEEDED(r)) step = "xrPassthroughStartFB", r = h.passthrough_start(h.fb_passthrough);
    if (XR_SUCCEEDED(r)) step = "xrPassthroughLayerResumeFB", r = h.passthrough_layer_resume(h.fb_passthrough_layer);
    if (XR_SUCCEEDED(r)) return true;
    log("xr: passthrough: %s failed: %d; no passthrough this run", step, (int)r);
    fb_destroy(h);
    return false;
}

void fb_pause(Host &h)
{
    XrResult a = h.passthrough_layer_pause(h.fb_passthrough_layer);
    XrResult b = h.passthrough_pause(h.fb_passthrough);
    if (XR_FAILED(a) || XR_FAILED(b)) log("xr: passthrough: pausing failed: layer %d, passthrough %d", (int)a, (int)b);
}

}

void passthrough_pick(Host &h)
{
    std::vector<XrEnvironmentBlendMode> modes;
    uint32_t n = 0;
    if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(h.instance, h.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                      0, &n, nullptr)) && n) {
        modes.resize(n);
        if (XR_FAILED(xrEnumerateEnvironmentBlendModes(h.instance, h.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       n, &n, modes.data())))
            n = 0;
        modes.resize(n);
    }
    std::string list;
    bool alpha = false;
    for (XrEnvironmentBlendMode m : modes) {
        list += ' ';
        list += blend_name(m);
        if (m != XR_ENVIRONMENT_BLEND_MODE_OPAQUE && m != XR_ENVIRONMENT_BLEND_MODE_ADDITIVE &&
            m != XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
            list += std::to_string((int)m);
        alpha |= m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    }

    bool fb = h.ext.fb_passthrough;
    if (fb) {
        load(h.instance, "xrCreatePassthroughFB", h.create_passthrough);
        load(h.instance, "xrDestroyPassthroughFB", h.destroy_passthrough);
        load(h.instance, "xrPassthroughStartFB", h.passthrough_start);
        load(h.instance, "xrPassthroughPauseFB", h.passthrough_pause);
        load(h.instance, "xrCreatePassthroughLayerFB", h.create_passthrough_layer);
        load(h.instance, "xrDestroyPassthroughLayerFB", h.destroy_passthrough_layer);
        load(h.instance, "xrPassthroughLayerResumeFB", h.passthrough_layer_resume);
        load(h.instance, "xrPassthroughLayerPauseFB", h.passthrough_layer_pause);
        fb = h.create_passthrough && h.destroy_passthrough && h.passthrough_start && h.passthrough_pause &&
             h.create_passthrough_layer && h.destroy_passthrough_layer && h.passthrough_layer_resume &&
             h.passthrough_layer_pause;
    }

    h.passthrough = alpha && !(fb && h.opt->passthrough_fb) ? Passthrough::AlphaBlend
                    : fb                                     ? Passthrough::FB
                                                             : Passthrough::None;
    set_passthrough_available(h.passthrough != Passthrough::None);
    log("openxr: passthrough: blend modes%s; XR_FB_passthrough %d%s; picked %s%s", list.empty() ? " none" : list.c_str(),
        (int)h.ext.fb_passthrough, h.ext.fb_passthrough && !fb ? " (entry points missing)" : "",
        way_name(h.passthrough), h.opt->passthrough_fb ? " (--passthrough-fb)" : "");
}

bool passthrough_frame(Host &h, std::vector<const XrCompositionLayerBaseHeader *> &layers,
                       XrEnvironmentBlendMode &mode)
{
    mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const bool want = passthrough_wanted() && h.passthrough != Passthrough::None;
    if (want != h.passthrough_on) {
        bool ok = true;
        if (h.passthrough == Passthrough::FB) {
            if (want) ok = fb_start(h);
            else fb_pause(h);
        }
        if (ok) {
            h.passthrough_on = want;
            log("xr: passthrough %s (%s)", want ? "on" : "off", way_name(h.passthrough));
        } else {
            h.passthrough = Passthrough::None;
            set_passthrough_available(false);
        }
    }
    if (!h.passthrough_on) return false;
    if (h.passthrough == Passthrough::AlphaBlend) {
        mode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    } else {
        static XrCompositionLayerPassthroughFB layer = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        layer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layer.space = XR_NULL_HANDLE;
        layer.layerHandle = h.fb_passthrough_layer;
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer));
    }
    return true;
}

void passthrough_failed(Host &h, XrResult r)
{
    log("xr: passthrough: xrEndFrame refused the %s frame: %d; no passthrough this run", way_name(h.passthrough), (int)r);
    if (h.passthrough == Passthrough::FB) fb_pause(h);
    h.passthrough = Passthrough::None;
    h.passthrough_on = false;
    set_passthrough_available(false);
}

void passthrough_destroy(Host &h)
{
    fb_destroy(h);                          // the handles are null unless FB made them
    h.passthrough_on = false;
}

}
