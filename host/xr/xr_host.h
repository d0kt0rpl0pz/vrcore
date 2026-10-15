/*
 * xr_host.h -- the OpenXR host's state, shared by xr.cpp (instance, session, the frame loop),
 * xr_swapchain.cpp (the eye swapchain and its density maps, the eye target on it) and xr_meta.cpp (refresh rate,
 * foveation, sharpening, performance levels, the thread hint). Android only. Host-internal.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "../gpu/vk.h"

#include <jni.h>
#include <time.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../app/app.h"
#include "../gpu/device.h"
#include "../gpu/eye_target.h"
#include "../gpu/render_host.h"

// Logs the failing call and returns `ret` from the enclosing function.
#define VRCORE_XR(call, ret)                                                              \
    do {                                                                                  \
        XrResult xr_ = (call);                                                            \
        if (XR_FAILED(xr_)) {                                                             \
            ::vrcore::log("openxr: %s failed: %d (%s:%d)", #call, (int)xr_, __FILE__, __LINE__); \
            return ret;                                                                   \
        }                                                                                 \
    } while (0)

namespace vrcore::xr {

struct Actions;                         // xr_actions.cpp

// The optional extensions, one flag each (the required ones fail instance creation).
struct Extensions {
    bool refresh_rate = false;          // XR_FB_display_refresh_rate
    bool foveation = false;             // XR_FB_foveation + _configuration + _vulkan + XR_FB_swapchain_update_state
    bool perf_settings = false;         // XR_EXT_performance_settings
    bool thread_settings = false;       // XR_KHR_android_thread_settings
    bool convert_timespec = false;      // XR_KHR_convert_timespec_time (the display time on CLOCK_MONOTONIC)
    bool touch_plus = false;            // XR_META_touch_controller_plus (the Quest 3's controllers' profile)
    bool pico = false;                  // XR_BD_controller_interaction (bytedance/pico4_controller, pico_neo3_controller)
    bool frame_controller = false;      // XR_VALVE_frame_controller_interaction
    bool layer_settings = false;        // XR_FB_composition_layer_settings (sharpening)
    bool fb_passthrough = false;        // XR_FB_passthrough (passthrough where the core ALPHA_BLEND is not listed)
};

// How passthrough reaches the compositor (xr_passthrough.cpp), picked once the system is known.
enum class Passthrough {
    None,                               // no way: passthrough_available() false
    AlphaBlend,                         // core OpenXR: the frame ends with XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
    FB,                                 // XR_FB_passthrough: its layer first, under everything, the blend mode opaque
};

struct Host {
    const AppInfo *info = nullptr;
    Options *opt = nullptr;             // the launch's, then the graphics in force (request_graphics changes them)

    jobject activity = nullptr;         // a global reference, for the loader and the instance

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool quit = false;
    Extensions ext;
    XrViewConfigurationView config_views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

    // extension entry points
    PFN_xrGetVulkanGraphicsRequirements2KHR get_vulkan_requirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR create_vulkan_instance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_vulkan_device = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_vulkan_device = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate_refresh_rates = nullptr;
    PFN_xrGetDisplayRefreshRateFB get_refresh_rate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request_refresh_rate = nullptr;
    PFN_xrCreateFoveationProfileFB create_foveation_profile = nullptr;
    PFN_xrDestroyFoveationProfileFB destroy_foveation_profile = nullptr;
    PFN_xrUpdateSwapchainFB update_swapchain = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT set_performance_level = nullptr;
    PFN_xrSetAndroidApplicationThreadKHR set_thread = nullptr;
    PFN_xrConvertTimeToTimespecTimeKHR time_to_timespec = nullptr;
    PFN_xrCreatePassthroughFB create_passthrough = nullptr;
    PFN_xrDestroyPassthroughFB destroy_passthrough = nullptr;
    PFN_xrPassthroughStartFB passthrough_start = nullptr;
    PFN_xrPassthroughPauseFB passthrough_pause = nullptr;
    PFN_xrCreatePassthroughLayerFB create_passthrough_layer = nullptr;
    PFN_xrDestroyPassthroughLayerFB destroy_passthrough_layer = nullptr;
    PFN_xrPassthroughLayerResumeFB passthrough_layer_resume = nullptr;
    PFN_xrPassthroughLayerPauseFB passthrough_layer_pause = nullptr;

    // the eye swapchain: one, two layers
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool swapchain_fdm = false;         // made with the density-map flag
    std::vector<XrSwapchainImageVulkanKHR> images;
    std::vector<XrSwapchainImageFoveationVulkanFB> fdm_images;

    // the overlay (app/layers.h, xr_layers.cpp): a second eye swapchain, the eyes' size, format and density maps,
    // made on its first ask; its own eye target, so the eye pipelines draw on it
    XrSwapchain overlay = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> overlay_images;
    std::vector<XrSwapchainImageFoveationVulkanFB> overlay_fdm_images;
    gpu::EyeTarget overlay_target;
    bool overlay_failed = false;        // could not be made: not tried again until the eyes are made again
    bool overlay_acquired = false;      // an image held for this frame (released after the submit)
    bool overlay_drawn = false;         // and drawn: its layer goes out this frame

    gpu::Device dev;
    gpu::EyeTarget target;
    gpu::RendererImpl r;
    bool front_up = false;

    Actions *actions = nullptr;         // the controllers (xr_actions.cpp)

    uint32_t recentres = 0;             // LOCAL space change events (the headset's recentre)
    float refresh_hz = 0;               // in force (0: unknown)
    int foveation = -1;                 // in force (-1: none)
    Passthrough passthrough = Passthrough::None;    // the way picked (passthrough_pick)
    bool passthrough_on = false;        // in force: the front asked and it started
    XrPassthroughFB fb_passthrough = XR_NULL_HANDLE;            // FB: made the first time it is turned on
    XrPassthroughLayerFB fb_passthrough_layer = XR_NULL_HANDLE;
    float eye_pending = 0;              // an eye resolution asked for, made at eye_at (steady seconds); 0: none
    double eye_at = 0;
};

// An entry point by name; null when the runtime has none.
template <typename T>
inline void load(XrInstance instance, const char *name, T &fn)
{
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(&fn)))) fn = nullptr;
}

// xr_swapchain.cpp
bool swapchain_create(Host &h);
void swapchain_destroy(Host &h);
bool eye_target_make(Host &h);              // the eye target (4x MSAA) on the swapchain's images
// A new eye resolution (opt's) while the session runs: the GPU idled, the swapchain made again (foveation set on the
// new one), the eye target made again on it. False (logged): the loop cannot go on.
bool eye_remake(Host &h);

// xr_layers.cpp (app/layers.h's quad layers)
void layers_bind(Host &h);                  // the session the layers are made in
void layers_release(Host &h);               // after the frame's submit: the images drawn this frame
// those up, in the order made: the underlays (under the projection layer) and the rest (over it)
void layers_compose(Host &h, std::vector<XrCompositionLayerQuad> &under, std::vector<XrCompositionLayerQuad> &over);
// After the eye pass, before the submit: the overlay's draws asked for this frame, in their own pass on the overlay
// (made on the first ask); none asked: nothing. layers_release releases its image.
void overlay_render(Host &h, gpu::Frame &f);
// Before xrEndFrame: the overlay's projection layer, its views the eyes' (as drawn), when it was drawn this frame
// (then true, and `layer` goes last, over the quads).
bool overlay_compose(Host &h, const XrCompositionLayerProjectionView eyes[2], XrCompositionLayerProjection &layer,
                     XrCompositionLayerProjectionView views[2]);
void overlay_destroy(Host &h);              // the GPU idle: before the eyes are made again, and at teardown

// xr_actions.cpp (app/input.h's hands and head)
bool actions_create(Host &h);               // after the instance: the action set, the profiles' bindings
bool actions_attach(Host &h);               // after the session: attached, the grip and head spaces
void actions_sync(Host &h, XrTime t);       // each frame after xrWaitFrame (unfocused: the hands zero)
void actions_profile_changed(Host &h);      // XrEventDataInteractionProfileChanged: logged
void actions_destroy(Host &h);

// xr_passthrough.cpp (app.h's passthrough)
// After the system: the blend modes listed and XR_FB_passthrough, the way picked and logged (one line), and
// app.h's passthrough_available set.
void passthrough_pick(Host &h);
// Each frame before xrEndFrame, the session running: the front's ask applied (FB: started or paused). On: the
// FB layer pushed into `layers` (which must be empty: it goes under everything) and true returned, so the caller
// gives the eye layer its alpha; `mode` the blend mode to end the frame with.
bool passthrough_frame(Host &h, std::vector<const XrCompositionLayerBaseHeader *> &layers,
                       XrEnvironmentBlendMode &mode);
// xrEndFrame refused a frame with passthrough in it (a runtime that lists ALPHA_BLEND but will not take it): logged,
// unavailable from now on; the caller ends the frame again without it.
void passthrough_failed(Host &h, XrResult r);
void passthrough_destroy(Host &h);          // before the session goes

// xr_meta.cpp
void meta_load(Host &h);                // the extension entry points
void meta_session_start(Host &h);       // refresh rate, performance levels, thread hint
std::vector<float> meta_rates(Host &h); // the runtime's refresh rates as listed (a session made; empty: none)
void refresh_set(Host &h);              // opt->hz: the listed rate nearest it (a tie the lower)
bool foveation_set(Host &h, int level); // 0-3: Off, Low, Medium, High; dynamic off (the overlay's too, when made)
// The projection layer's settings for opt->sharpen (chained on it each frame); null for Off or no extension.
const void *sharpen_settings(Host &h);
void meta_event(Host &h, const XrEventDataBuffer &e);

}
