/*
 * xr.cpp -- the OpenXR host (Android): the loader, the instance (required / optional extensions), the
 * system, Vulkan through XR_KHR_vulkan_enable2, the session state machine, the LOCAL space and the frame
 * loop. xrWaitFrame paces the loop; each frame draws both eyes in one multiview pass and submits one
 * projection layer whose two views are exactly the ones it rendered with. Between frames the front's graphics asks
 * (app.h's request_graphics) are applied: the rate, foveation and sharpening at once, the eye resolution once it
 * has rested 0.5 s. The eye pass is always 4x MSAA.
 */
#include "xr_host.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <sys/system_properties.h>

#include "../app/input_host.h"
#include "../app/log.h"
#include "../app/picker_test.h"
#include "../app/soundtrack_probe.h"
#include "../app/shots.h"

namespace vrcore::xr {

namespace {

bool g_head_latched = false;                // --head-lock's latch (head_lock below); a session made again starts it over

double now_s()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t steady_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// An XrTime on steady_clock (CLOCK_MONOTONIC, as libc++ reads it on Android); without the extension, a
// period from now.
int64_t display_ns(const Host &h, XrTime t, int64_t period_ns)
{
    if (h.time_to_timespec) {
        timespec ts = {};
        if (XR_SUCCEEDED(h.time_to_timespec(h.instance, t, &ts))) return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    }
    return steady_ns() + period_ns;
}

void update(Host &h, int64_t when_ns, int64_t period_ns, bool shown)
{
    if (!h.info->update) return;
    Tick t;
    t.display_ns = when_ns;
    t.period_ns = period_ns;
    t.shown = shown;
    t.recentres = h.recentres;
    t.focused = h.running && h.state == XR_SESSION_STATE_FOCUSED;
    h.info->update(t);
    if (take_restart()) {                   // app.h's request_restart: the front again from its start, same session
        vkDeviceWaitIdle(h.dev.device);
        log("xr: restart: %s shut down and started again", h.info->name);
        h.info->shutdown(h.r.renderer);
        if (!h.info->init(h.r.renderer, *h.opt)) {
            log("xr: %s init failed on the restart", h.info->name);
            request_quit(1);
        }
    }
}

const char *state_name(XrSessionState s)
{
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

// ---- Vulkan through XR_KHR_vulkan_enable2 (gpu::DeviceHooks) ----

VkResult hook_instance(void *user, const VkInstanceCreateInfo *ci, VkInstance *out)
{
    Host &h = *static_cast<Host *>(user);
    XrVulkanInstanceCreateInfoKHR xci = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xci.systemId = h.system;
    xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xci.vulkanCreateInfo = ci;
    VkResult vr = VK_SUCCESS;
    XrResult r = h.create_vulkan_instance(h.instance, &xci, out, &vr);
    if (XR_FAILED(r)) {
        log("openxr: xrCreateVulkanInstanceKHR failed: %d (Vulkan %d)", (int)r, (int)vr);
        return vr != VK_SUCCESS ? vr : VK_ERROR_INITIALIZATION_FAILED;
    }
    return vr;
}

VkResult hook_physical_device(void *user, VkInstance instance, VkPhysicalDevice *out)
{
    Host &h = *static_cast<Host *>(user);
    XrVulkanGraphicsDeviceGetInfoKHR gi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = h.system;
    gi.vulkanInstance = instance;
    XrResult r = h.get_vulkan_device(h.instance, &gi, out);
    if (XR_FAILED(r)) {
        log("openxr: xrGetVulkanGraphicsDevice2KHR failed: %d", (int)r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

VkResult hook_device(void *user, VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, VkDevice *out)
{
    Host &h = *static_cast<Host *>(user);
    XrVulkanDeviceCreateInfoKHR xci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xci.systemId = h.system;
    xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xci.vulkanPhysicalDevice = phys;
    xci.vulkanCreateInfo = ci;
    VkResult vr = VK_SUCCESS;
    XrResult r = h.create_vulkan_device(h.instance, &xci, out, &vr);
    if (XR_FAILED(r)) {
        log("openxr: xrCreateVulkanDeviceKHR failed: %d (Vulkan %d)", (int)r, (int)vr);
        return vr != VK_SUCCESS ? vr : VK_ERROR_INITIALIZATION_FAILED;
    }
    return vr;
}

// ---- set-up ----

bool loader_init(Host &h)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (!env || !activity) {
        log("openxr: no JNI environment or activity from SDL");
        return false;
    }
    h.activity = env->NewGlobalRef(activity);
    env->DeleteLocalRef(activity);
    JavaVM *vm = nullptr;
    env->GetJavaVM(&vm);

    PFN_xrInitializeLoaderKHR init = nullptr;
    load(XR_NULL_HANDLE, "xrInitializeLoaderKHR", init);
    if (!init) {
        log("openxr: the loader has no xrInitializeLoaderKHR");
        return false;
    }
    XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = vm;
    li.applicationContext = h.activity;
    VRCORE_XR(init((const XrLoaderInitInfoBaseHeaderKHR *)&li), false);
    return true;
}

bool instance_create(Host &h)
{
    uint32_t n = 0;
    VRCORE_XR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr), false);
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    VRCORE_XR(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()), false);
    auto has = [&](const char *name) {
        for (const XrExtensionProperties &p : props)
            if (!std::strcmp(p.extensionName, name)) return true;
        return false;
    };
    // over several lines: logcat cuts one at ~1 KB
    log("openxr: %u extensions", n);
    std::string list;
    for (uint32_t i = 0; i < n; ++i) {
        list += ' ';
        list += props[i].extensionName;
        if (list.size() > 600 || i + 1 == n) {
            log("openxr: extensions:%s", list.c_str());
            list.clear();
        }
    }

    std::vector<const char *> exts;
    for (const char *req : {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME}) {
        if (!has(req)) {
            log("openxr: required extension %s missing", req);
            return false;
        }
        exts.push_back(req);
    }
    auto optional = [&](const char *name) {
        if (!has(name)) return false;
        exts.push_back(name);
        return true;
    };
    h.ext.refresh_rate = optional(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    bool update_state = optional(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
    bool fov = optional(XR_FB_FOVEATION_EXTENSION_NAME);
    bool fov_config = optional(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
    bool fov_vulkan = optional(XR_FB_FOVEATION_VULKAN_EXTENSION_NAME);
    h.ext.foveation = update_state && fov && fov_config && fov_vulkan;
    h.ext.perf_settings = optional(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    h.ext.thread_settings = optional(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    h.ext.convert_timespec = optional(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
    h.ext.touch_plus = optional(XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME);
    h.ext.frame_controller = optional("XR_VALVE_frame_controller_interaction");
    h.ext.layer_settings = optional(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    log("openxr: optional: refresh rate %d, foveation %d (update state %d, foveation %d, configuration %d, "
        "vulkan %d), performance settings %d, thread settings %d, timespec %d, touch plus %d, frame controller %d, "
        "layer settings %d",
        h.ext.refresh_rate, h.ext.foveation, update_state, fov, fov_config, fov_vulkan, h.ext.perf_settings,
        h.ext.thread_settings, h.ext.convert_timespec, h.ext.touch_plus, h.ext.frame_controller,
        h.ext.layer_settings);

    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    JavaVM *vm = nullptr;
    env->GetJavaVM(&vm);
    XrInstanceCreateInfoAndroidKHR android = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = vm;
    android.applicationActivity = h.activity;
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android;
    SDL_strlcpy(ci.applicationInfo.applicationName, h.info->name, XR_MAX_APPLICATION_NAME_SIZE);
    ci.applicationInfo.applicationVersion = 1;
    SDL_strlcpy(ci.applicationInfo.engineName, "vrcore", XR_MAX_ENGINE_NAME_SIZE);
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.enabledExtensionNames = exts.data();
    VRCORE_XR(xrCreateInstance(&ci, &h.instance), false);

    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(h.instance, &ip);
    log("openxr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    load(h.instance, "xrGetVulkanGraphicsRequirements2KHR", h.get_vulkan_requirements);
    load(h.instance, "xrCreateVulkanInstanceKHR", h.create_vulkan_instance);
    load(h.instance, "xrGetVulkanGraphicsDevice2KHR", h.get_vulkan_device);
    load(h.instance, "xrCreateVulkanDeviceKHR", h.create_vulkan_device);
    if (!h.get_vulkan_requirements || !h.create_vulkan_instance || !h.get_vulkan_device || !h.create_vulkan_device) {
        log("openxr: XR_KHR_vulkan_enable2 entry points missing");
        return false;
    }
    meta_load(h);
    if (h.ext.convert_timespec) load(h.instance, "xrConvertTimeToTimespecTimeKHR", h.time_to_timespec);
    return true;
}

bool system_get(Host &h, std::string &name)
{
    XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    VRCORE_XR(xrGetSystem(h.instance, &gi, &h.system), false);
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(h.instance, h.system, &sp);
    name = sp.systemName;

    uint32_t n = 0;
    VRCORE_XR(xrEnumerateViewConfigurationViews(h.instance, h.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n,
                                                h.config_views), false);
    const XrViewConfigurationView &v = h.config_views[0];
    log("openxr: system %s (vendor 0x%x); eye recommended %ux%u (max %ux%u), samples %u (max %u)", sp.systemName,
        sp.vendorId, v.recommendedImageRectWidth, v.recommendedImageRectHeight, v.maxImageRectWidth,
        v.maxImageRectHeight, v.recommendedSwapchainSampleCount, v.maxSwapchainSampleCount);

    XrGraphicsRequirementsVulkan2KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    VRCORE_XR(h.get_vulkan_requirements(h.instance, h.system, &req), false);
    log("openxr: Vulkan API %u.%u to %u.%u", XR_VERSION_MAJOR(req.minApiVersionSupported),
        XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_MAJOR(req.maxApiVersionSupported),
        XR_VERSION_MINOR(req.maxApiVersionSupported));
    if (XR_MAKE_VERSION(1, 1, 0) < XR_MAKE_VERSION(XR_VERSION_MAJOR(req.minApiVersionSupported),
                                                   XR_VERSION_MINOR(req.minApiVersionSupported), 0)) {
        log("openxr: the runtime wants a newer Vulkan than 1.1");
        return false;
    }
    return true;
}

bool session_create(Host &h)
{
    XrGraphicsBindingVulkan2KHR gb = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance = h.dev.instance;
    gb.physicalDevice = h.dev.phys;
    gb.device = h.dev.device;
    gb.queueFamilyIndex = h.dev.queue_family;
    gb.queueIndex = 0;
    XrSessionCreateInfo ci = {XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &gb;
    ci.systemId = h.system;
    VRCORE_XR(xrCreateSession(h.instance, &ci, &h.session), false);
    g_head_latched = false;

    XrReferenceSpaceCreateInfo si = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    si.poseInReferenceSpace.orientation.w = 1.f;
    VRCORE_XR(xrCreateReferenceSpace(h.session, &si, &h.local), false);
    return actions_attach(h);
}

// ---- the loop ----

void poll_events(Host &h)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        pads_event(&e);
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
            log("xr: SDL %s", e.type == SDL_EVENT_QUIT ? "quit" : "terminating");
            if (h.running) xrRequestExitSession(h.session);
            else h.quit = true;
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND: log("xr: SDL will enter background"); break;
        case SDL_EVENT_DID_ENTER_BACKGROUND: log("xr: SDL entered background"); break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND: log("xr: SDL will enter foreground"); break;
        case SDL_EVENT_DID_ENTER_FOREGROUND: log("xr: SDL entered foreground"); break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: log("xr: window focus gained"); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: log("xr: window focus lost"); break;
        default: break;
        }
    }

    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(h.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto &s = *reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
            h.state = s.state;
            log("xr: session %s", state_name(s.state));
            if (s.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(h.session, &bi);
                h.running = XR_SUCCEEDED(r);
                if (!h.running) log("openxr: xrBeginSession failed: %d", (int)r);
                else meta_session_start(h);
            } else if (s.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(h.session);
                h.running = false;
            } else if (s.state == XR_SESSION_STATE_EXITING || s.state == XR_SESSION_STATE_LOSS_PENDING) {
                h.quit = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            log("xr: instance loss pending");
            h.quit = true;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto &c = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending *>(&ev);
            if (c.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) ++h.recentres;
            log("xr: reference space %d change pending (recentre %u)", (int)c.referenceSpaceType, h.recentres);
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            actions_profile_changed(h);
            break;
        default:
            meta_event(h, ev);
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// app.h's request_graphics: what changed since the last ask, applied between frames; the eye resolution once no
// newer ask changed it for 0.5 s (a held row makes one swapchain). False (logged): the loop cannot go on.
bool apply_graphics(Host &h)
{
    Options &o = *h.opt;
    Graphics g;
    if (take_graphics(g)) {
        if (std::fabs(g.hz - o.hz) > 0.01f) {
            o.hz = g.hz;
            if (h.running) refresh_set(h);          // else at the session's start
        }
        int fov = g.foveation < 0 ? 0 : g.foveation > 3 ? 3 : g.foveation;
        if (fov != o.foveation) {
            o.foveation = fov;
            if (h.swapchain_fdm) foveation_set(h, fov);
        }
        int sharpen = g.sharpen < 0 ? 0 : g.sharpen > 2 ? 2 : g.sharpen;
        if (sharpen != o.sharpen) {
            o.sharpen = sharpen;
            log("xr: sharpening %s%s", sharpen == 0 ? "Off" : sharpen == 1 ? "Normal" : "Quality",
                h.ext.layer_settings ? "" : " (no layer settings extension: none)");
        }
        float eye = g.eye_scale < 1.f ? 1.f : g.eye_scale > 3.f ? 3.f : g.eye_scale;
        if (std::fabs(eye - (h.eye_pending > 0 ? h.eye_pending : o.eye_scale)) > 1e-4f) {
            h.eye_pending = eye;
            h.eye_at = now_s() + 0.5;
        }
    }
    if (h.eye_pending > 0 && now_s() >= h.eye_at) {
        bool changed = std::fabs(h.eye_pending - o.eye_scale) > 1e-4f;
        o.eye_scale = h.eye_pending;
        h.eye_pending = 0;
        if (changed && !eye_remake(h)) return false;
    }
    return true;
}

struct Stats {
    double start = 0, wait = 0, cpu = 0, gpu = 0, period = 0;
    long frames = 0, rendered = 0;
};

// Acquires a swapchain image, draws both eyes into it, submits and releases it. False (logged) when the
// image could not be had (the frame then goes out with no layer) or the GPU submit failed (`fatal`).
bool render(Host &h, Stats &st, const gpu::Eye eyes[2], bool &fatal)
{
    uint32_t image = 0;
    VRCORE_XR(xrAcquireSwapchainImage(h.swapchain, nullptr, &image), false);
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XrResult wr = xrWaitSwapchainImage(h.swapchain, &wi);
    if (XR_FAILED(wr) || wr == XR_TIMEOUT_EXPIRED) {
        log("openxr: xrWaitSwapchainImage: %d; frame dropped", (int)wr);
        xrReleaseSwapchainImage(h.swapchain, nullptr);
        return false;
    }
    double t0 = now_s();
    gpu::Frame &f = gpu::renderer_begin(h.r, eyes);
    if (h.info->prepare) h.info->prepare(f);
    gpu::renderer_begin_eye_pass(h.r, image, h.info->clear ? h.info->clear : gpu::kClear);
    h.info->draw(f);
    gpu::renderer_end_pass(h.r);
    bool ok = gpu::renderer_submit(h.r);
    layers_release(h);                      // the layers' images drawn this frame, after the submit
    st.cpu += now_s() - t0;
    // A failed submit leaves its slot's fence unsignalled: the next renderer_begin would wait for ever.
    fatal = !ok;
    // Released even when the submit failed, so the frame can still be ended.
    VRCORE_XR(xrReleaseSwapchainImage(h.swapchain, nullptr), false);
    return ok;
}

// --head-lock: the eyes held where the first located frame had them, level and facing the way the left eye faced,
// with the eye separation of that frame; the runtime's head pose is ignored from then on, so an unworn headset draws
// the same view on every run (--head-lock). The submitted views are these, as the drawn ones.
void head_lock(XrView views[2], bool position_valid)
{
    static float centre[3], yaw, half;
    static int waited = 0;
    if (!g_head_latched) {
        // wait for a located position (3DOF or tracking still starting gives none); 2 s on, latch what there is
        if (!position_valid && ++waited < 120) return;
        g_head_latched = true;
        waited = 0;
        const XrQuaternionf &q = views[0].pose.orientation;
        float fx = -2.f * (q.x * q.z + q.w * q.y), fz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
        yaw = std::atan2(-fx, -fz);
        const XrVector3f &a = views[0].pose.position, &b = views[1].pose.position;
        centre[0] = 0.5f * (a.x + b.x);
        centre[1] = 0.5f * (a.y + b.y);
        centre[2] = 0.5f * (a.z + b.z);
        half = 0.5f * std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
        log("xr: head lock at %.3f %.3f %.3f, yaw %.1f, eyes %.1f mm apart", centre[0], centre[1], centre[2],
            yaw * 57.2958f, half * 2000.f);
    }
    Quat q = quat_axis_angle(0, 1, 0, yaw);
    float rx = std::cos(yaw), rz = -std::sin(yaw);
    for (int i = 0; i < 2; ++i) {
        float side = i == 0 ? -half : half;
        views[i].pose.orientation = {q.x, q.y, q.z, q.w};
        views[i].pose.position = {centre[0] + rx * side, centre[1], centre[2] + rz * side};
    }
}

// One display frame; once begun it is always ended, with no layer when nothing was drawn. Returns false
// (logged) when the frame loop cannot go on.
bool frame(Host &h, Stats &st, bool &rendered)
{
    rendered = false;
    double t0 = now_s();
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    VRCORE_XR(xrWaitFrame(h.session, nullptr, &fs), false);
    double t1 = now_s();
    actions_sync(h, fs.predictedDisplayTime);
    input_begin_frame(t1);
    update(h, display_ns(h, fs.predictedDisplayTime, fs.predictedDisplayPeriod), fs.predictedDisplayPeriod,
           fs.shouldRender);
    VRCORE_XR(xrBeginFrame(h.session, nullptr), false);

    XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    static std::vector<XrCompositionLayerQuad> quads, under;
    static std::vector<const XrCompositionLayerBaseHeader *> layers;
    layers.clear();
    bool fatal = false;

    if (fs.shouldRender) {
        XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = h.local;
        XrViewState vs = {XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t n = 0;
        XrResult lr = xrLocateViews(h.session, &li, &vs, 2, &n, views);
        // Orientation is enough: without positional tracking the runtime keeps the last position (3DOF).
        bool located = XR_SUCCEEDED(lr) && n == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (located && h.opt->head_lock)
            head_lock(views, (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
        if (located) {
            static bool fov_logged = false;
            if (!fov_logged) {
                fov_logged = true;
                for (int i = 0; i < 2; ++i)
                    log("xr: eye %d fov left %.6f right %.6f up %.6f down %.6f, at %.4f %.4f %.4f", i,
                        views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp,
                        views[i].fov.angleDown, views[i].pose.position.x, views[i].pose.position.y,
                        views[i].pose.position.z);
            }
            gpu::Eye eyes[2];
            for (int i = 0; i < 2; ++i) {
                const XrPosef &p = views[i].pose;
                Quat q = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
                eyes[i].view = mat4_view_from_pose(q, p.position.x, p.position.y, p.position.z);
                eyes[i].proj = mat4_projection_reverse_z(views[i].fov.angleLeft, views[i].fov.angleRight,
                                                         views[i].fov.angleUp, views[i].fov.angleDown, gpu::kEyeNear,
                                                         gpu::kEyeFar);
                eyes[i].pos[0] = p.position.x;
                eyes[i].pos[1] = p.position.y;
                eyes[i].pos[2] = p.position.z;
            }

            rendered = render(h, st, eyes, fatal);
        }
        if (rendered) {
            // The views submitted are the ones the eyes were drawn with.
            for (int i = 0; i < 2; ++i) {
                pviews[i].pose = views[i].pose;
                pviews[i].fov = views[i].fov;
                pviews[i].subImage.swapchain = h.swapchain;
                pviews[i].subImage.imageRect.extent = {(int32_t)h.width, (int32_t)h.height};
                pviews[i].subImage.imageArrayIndex = (uint32_t)i;
            }
            layer.next = sharpen_settings(h);
            layer.space = h.local;
            layer.viewCount = 2;
            layer.views = pviews;
        }
        // the front's quad layers (app/layers.h): the underlays under the eyes' picture, which is then blended by
        // its alpha (premultiplied: layer_punch's holes are transparent black), the rest over it; on their own when
        // the eyes were not drawn (tracking lost, an eye image dropped): they keep their last released image
        layers_compose(h, under, quads);
        for (const XrCompositionLayerQuad &q : under)
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&q));
        if (rendered) {
            layer.layerFlags = under.empty() ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&layer));
        }
        for (const XrCompositionLayerQuad &q : quads)
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&q));
    }

    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = (uint32_t)layers.size();
    ei.layers = layers.data();
    VRCORE_XR(xrEndFrame(h.session, &ei), false);
    if (fatal) return false;

    st.wait += t1 - t0;
    st.period += fs.predictedDisplayPeriod * 1e-9;
    ++st.frames;
    if (rendered) ++st.rendered;
    return true;
}

int loop(Host &h)
{
    Stats st;
    st.start = now_s();
    double cycle_at = 0, first_render = 0;
    long total_rendered = 0;
    bool pick_started = false, exit_requested = false;

    while (!h.quit) {
        poll_events(h);
        if (!apply_graphics(h)) return 1;
        if (h.opt->pick_test) picker_test_poll();
        if (!h.running) {
            actions_sync(h, 0);             // the hands zero
            input_begin_frame(now_s());
            update(h, steady_ns(), 10000000, false);
            if (quit_requested()) h.quit = true;
            SDL_Delay(10);
            continue;
        }
        bool done = h.opt->frames >= 0 && total_rendered >= h.opt->frames;
        if ((done || quit_requested()) && !exit_requested) {
            exit_requested = true;
            xrRequestExitSession(h.session);
        }
        bool rendered = false;
        if (!frame(h, st, rendered)) return 1;
        double now = now_s();
        if (rendered) {
            if (!total_rendered++) {
                first_render = now;
                cycle_at = now + h.opt->foveation_cycle;
            }
        }
        if (h.opt->foveation_cycle > 0 && h.foveation >= 0 && total_rendered && now >= cycle_at) {
            cycle_at = now + h.opt->foveation_cycle;
            foveation_set(h, (h.foveation + 1) % 4);
        }
        if (h.opt->soundtrack_probe && total_rendered && now - first_render > 3.0) soundtrack_probe_poll(now);
        if (h.opt->pick_test && !pick_started && total_rendered && now - first_render > 3.0) {
            pick_started = true;
            picker_test_start();
        }
        double span = now - st.start;
        if (span >= 5.0) {
            log("xr: %ld frames in %.1f s (%.1f fps), %ld rendered; display %.1f Hz; wait %.2f ms, cpu %.2f ms, "
                "gpu %.2f ms; %s, foveation %d",
                st.frames, span, st.frames / span, st.rendered, st.frames ? st.frames / st.period : 0.0,
                st.frames ? st.wait / st.frames * 1e3 : 0.0, st.rendered ? st.cpu / st.rendered * 1e3 : 0.0,
                h.r.gpu_ms, state_name(h.state), h.foveation);
            st = Stats();
            st.start = now;
        }
    }
    int code = 0;
    quit_requested(&code);
    return code;
}

void teardown(Host &h)
{
    if (h.dev.device) vkDeviceWaitIdle(h.dev.device);
    if (h.front_up) h.info->shutdown(h.r.renderer);
    gpu::renderer_destroy(h.r);
    if (h.dev.device) gpu::eye_target_destroy(h.dev, h.target);
    swapchain_destroy(h);
    actions_destroy(h);
    if (h.local) xrDestroySpace(h.local);
    if (h.session) xrDestroySession(h.session);
    gpu::device_destroy(h.dev);
    if (h.instance) xrDestroyInstance(h.instance);
    if (h.activity) {
        JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
        if (env) env->DeleteGlobalRef(h.activity);
    }
}

int run_host(Host &h)
{
    std::string system_name;
    if (!loader_init(h) || !instance_create(h) || !system_get(h, system_name) || !actions_create(h)) return 1;

    gpu::DeviceHooks hooks;
    hooks.want_fdm = h.ext.foveation;
    hooks.user = &h;
    hooks.create_instance = hook_instance;
    hooks.pick_physical_device = hook_physical_device;
    hooks.create_device = hook_device;
    if (!gpu::device_create(h.dev, hooks, h.info->name)) return 1;
    if (!session_create(h)) return 1;

    // the headset known: the front's device profile (AppInfo::headset), before the swapchain is made at its size
    Headset hs;
    hs.system = system_name;
    hs.gpu = h.dev.props.deviceName;
    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.product.manufacturer", prop) > 0) hs.make = prop;
    if (__system_property_get("ro.product.model", prop) > 0) hs.model = prop;
    hs.rates = meta_rates(h);
    hs.foveation = h.ext.foveation && h.dev.fdm;
    hs.sharpen = h.ext.layer_settings;
    set_headset(*h.info, hs, *h.opt);

    if (!swapchain_create(h)) return 1;
    if (!eye_target_make(h) || !gpu::renderer_create(h.r, h.dev, h.target)) return 1;
    layers_bind(h);
    if (h.swapchain_fdm) foveation_set(h, h.opt->foveation);
    h.front_up = true;                      // shut down even after a failed init: it frees what init made
    if (!h.info->init(h.r.renderer, *h.opt)) {
        log("xr: %s init failed", h.info->name);
        return 1;
    }
    log("xr: thread %d runs the frame loop", (int)gettid());
    return loop(h);
}

}

int run(const AppInfo &info, const Options &launch)
{
    Options o = launch;                     // the graphics in force change while it runs
    // SDL's activity has its surface either way; a window (as M0 had) keeps its event plumbing as usual.
    SDL_Window *window = SDL_CreateWindow(info.name, 1280, 720, 0);
    if (!window) log("xr: SDL_CreateWindow failed: %s (going on)", SDL_GetError());
    pads_init();
    Host *h = new Host;
    h->info = &info;
    h->opt = &o;
    int rc = run_host(*h);
    pads_shutdown();
    teardown(*h);
    delete h;
    if (window) SDL_DestroyWindow(window);
    return rc;
}

}

namespace vrcore {

// app/shots.h: the sim's alone
bool request_view_shot(const gpu::Eye &, uint32_t, uint32_t, const char *png_path)
{
    log("xr: view shots are the sim's; %s not taken", png_path);
    return false;
}

}
