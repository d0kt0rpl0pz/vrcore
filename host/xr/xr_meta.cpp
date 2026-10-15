/*
 * xr_meta.cpp -- the optional Meta / Khronos extras, each used only when its extension is there:
 * the display refresh rate (XR_FB_display_refresh_rate: the listed rate nearest the wish, a tie the
 * lower), fixed foveation (XR_FB_foveation: levels 0-3, dynamic off, vertical offset 0),
 * the CPU / GPU performance levels
 * (XR_EXT_performance_settings; not set unless asked) and the render-thread hint
 * (XR_KHR_android_thread_settings), compositor sharpening (XR_FB_composition_layer_settings on the
 * projection layer: Off, Normal, Quality). The rate, foveation and sharpening
 * change while the session runs (the menu's rows: app.h's request_graphics).
 */
#include "xr_host.h"

#include <cmath>
#include <string>
#include <unistd.h>

#include "../app/log.h"

namespace vrcore::xr {

namespace {

const char *const kFoveationNames[4] = {"Off", "Low", "Medium", "High"};

}

std::vector<float> meta_rates(Host &h)
{
    std::vector<float> rates;
    uint32_t n = 0;
    if (!h.enumerate_refresh_rates || XR_FAILED(h.enumerate_refresh_rates(h.session, 0, &n, nullptr)) || !n)
        return rates;
    rates.resize(n);
    if (XR_FAILED(h.enumerate_refresh_rates(h.session, n, &n, rates.data()))) n = 0;
    rates.resize(n);
    return rates;
}

void refresh_set(Host &h)
{
    if (!h.enumerate_refresh_rates || !h.request_refresh_rate) {
        log("xr: no refresh rate extension; the runtime's rate");
        return;
    }
    std::vector<float> rates = meta_rates(h);
    if (rates.empty()) return;
    std::string list;
    float pick = rates[0];
    for (float r : rates) {
        list += " " + std::to_string((int)std::lround(r));
        float d = std::fabs(r - h.opt->hz), best = std::fabs(pick - h.opt->hz);
        if (d < best - 0.01f || (std::fabs(d - best) <= 0.01f && r < pick)) pick = r;
    }
    XrResult r = h.request_refresh_rate(h.session, pick);
    float now = 0;
    if (h.get_refresh_rate) h.get_refresh_rate(h.session, &now);
    h.refresh_hz = now;
    log("xr: refresh rates:%s; asked %.0f, requested %.1f (%d), in force %.1f", list.c_str(), h.opt->hz, pick,
        (int)r, now);
}

namespace {

void perf_start(Host &h)
{
    static const XrPerfSettingsLevelEXT kLevels[4] = {
        XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT,
        XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT};
    static const char *const kNames[4] = {"power savings", "sustained low", "sustained high", "boost"};
    const struct { XrPerfSettingsDomainEXT domain; int level; const char *name; } asks[2] = {
        {XR_PERF_SETTINGS_DOMAIN_CPU_EXT, h.opt->cpu_level, "cpu"},
        {XR_PERF_SETTINGS_DOMAIN_GPU_EXT, h.opt->gpu_level, "gpu"}};
    for (const auto &a : asks) {
        if (a.level < 0) continue;                      // not set: the runtime's own
        if (!h.set_performance_level) {
            log("xr: no performance settings extension; %s level not set", a.name);
            continue;
        }
        XrResult r = h.set_performance_level(h.session, a.domain, kLevels[a.level]);
        log("xr: %s level %s: %d", a.name, kNames[a.level], (int)r);
    }
}

}

void meta_load(Host &h)
{
    if (h.ext.refresh_rate) {
        load(h.instance, "xrEnumerateDisplayRefreshRatesFB", h.enumerate_refresh_rates);
        load(h.instance, "xrGetDisplayRefreshRateFB", h.get_refresh_rate);
        load(h.instance, "xrRequestDisplayRefreshRateFB", h.request_refresh_rate);
    }
    if (h.ext.foveation) {
        load(h.instance, "xrCreateFoveationProfileFB", h.create_foveation_profile);
        load(h.instance, "xrDestroyFoveationProfileFB", h.destroy_foveation_profile);
        load(h.instance, "xrUpdateSwapchainFB", h.update_swapchain);
        if (!h.create_foveation_profile || !h.destroy_foveation_profile || !h.update_swapchain) h.ext.foveation = false;
    }
    if (h.ext.perf_settings) load(h.instance, "xrPerfSettingsSetPerformanceLevelEXT", h.set_performance_level);
    if (h.ext.thread_settings) load(h.instance, "xrSetAndroidApplicationThreadKHR", h.set_thread);
}

void meta_session_start(Host &h)
{
    refresh_set(h);
    perf_start(h);
    if (h.set_thread) {
        XrResult r = h.set_thread(h.session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, (uint32_t)gettid());
        log("xr: thread %d marked RENDERER_MAIN: %d", (int)gettid(), (int)r);
    }
}

bool foveation_set(Host &h, int level)
{
    if (!h.swapchain_fdm || level < 0 || level > 3) return false;
    static const XrFoveationLevelFB kLevels[4] = {XR_FOVEATION_LEVEL_NONE_FB, XR_FOVEATION_LEVEL_LOW_FB,
                                                  XR_FOVEATION_LEVEL_MEDIUM_FB, XR_FOVEATION_LEVEL_HIGH_FB};
    XrFoveationLevelProfileCreateInfoFB lp = {XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    lp.level = kLevels[level];
    lp.verticalOffset = 0.f;
    lp.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
    XrFoveationProfileCreateInfoFB pci = {XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
    pci.next = &lp;
    XrFoveationProfileFB profile = XR_NULL_HANDLE;
    VRCORE_XR(h.create_foveation_profile(h.session, &pci, &profile), false);
    XrSwapchainStateFoveationFB state = {XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
    state.profile = profile;
    XrResult r = h.update_swapchain(h.swapchain, reinterpret_cast<const XrSwapchainStateBaseHeaderFB *>(&state));
    if (XR_SUCCEEDED(r) && h.overlay) {
        XrResult o = h.update_swapchain(h.overlay, reinterpret_cast<const XrSwapchainStateBaseHeaderFB *>(&state));
        if (XR_FAILED(o)) log("openxr: xrUpdateSwapchainFB (overlay) failed: %d", (int)o);
    }
    h.destroy_foveation_profile(profile);       // the swapchain keeps the level (XR_FB_foveation)
    if (XR_FAILED(r)) {
        log("openxr: xrUpdateSwapchainFB failed: %d", (int)r);
        return false;
    }
    h.foveation = level;
    log("xr: foveation %s (%d)", kFoveationNames[level], level);
    return true;
}

const void *sharpen_settings(Host &h)
{
    static XrCompositionLayerSettingsFB settings = {XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    if (!h.ext.layer_settings || h.opt->sharpen <= 0) return nullptr;
    settings.next = nullptr;
    settings.layerFlags = h.opt->sharpen >= 2 ? XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB
                                              : XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
    return &settings;
}

void meta_event(Host &h, const XrEventDataBuffer &e)
{
    if (e.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
        const auto &c = *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB *>(&e);
        h.refresh_hz = c.toDisplayRefreshRate;
        log("xr: refresh rate %.1f -> %.1f", c.fromDisplayRefreshRate, c.toDisplayRefreshRate);
    }
}

}
