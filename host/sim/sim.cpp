/*
 * sim.cpp -- the desktop host: the same multiview eye pass as the headset, drawn into two-layer images at the
 * Quest 3 x1.3 eye size, with a fake head (mouse and keyboard) for the pose. Vulkan comes through MoltenVK in an
 * SDL3 window: the window shows the left eye (Tab: both side by side), blitted into the swapchain. Headless
 * (--headless, --shot) draws the same frames with no window and can write the left eye as a PPM.
 * The front's graphics asks (app.h's request_graphics): the eye resolution (once it has rested 0.5 s) makes the eye
 * images / target again as on the headset; the rate, foveation and sharpening are logged (no headset).
 * --graphics-test asks a fixed list of eye resolutions by display frame (x1.4 then x1.5 at once: one remake, x1.3)
 * and logs whether each was made.
 *
 * Keys: the keyboard is the front's (app/input.h: a game's keys win, with no fake
 * head); the fake head takes them only while Option is held: Option + the arrows look, Option + W A S D move in
 * the yaw plane, Q down, E up, Shift x4. Right mouse drag looks; R resets the head; Tab toggles side by side;
 * closing the window quits (Escape is the front's: its menu). No Touch controllers: the hands read inactive.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../app/app.h"
#include "../app/input_host.h"
#include "../app/log.h"
#include "../app/shots.h"
#include "../gpu/device.h"
#include "../gpu/eye_target.h"
#include "../gpu/vmath.h"
#include "../gpu/render.h"
#include "../gpu/render_host.h"
#include "../gpu/vk.h"
#include "sim_layers.h"

namespace vrcore::sim {

namespace {

// Quest 3 recommended eye size; Options::eye_scale multiplies it.
constexpr float kQuestEyeWidth = 1680.f, kQuestEyeHeight = 1760.f;

constexpr float kIpd = 0.063f;                // the headset logged 0.0648 at its own setting
constexpr float kMoveSpeed = 1.5f;       // m/s
constexpr float kLookRate = 1.5f;        // rad/s on the arrow keys
constexpr float kMouseSens = 0.0025f;    // rad per pixel

// The Quest 3's per-eye FOV (radians, as OpenXR's XrFovf), as its runtime reported it (v207, 2026-10-10).
struct Fov { float left, right, up, down; };
constexpr Fov kEyeFov[2] = {
    {-0.942478f, 0.698132f, 0.767945f, -0.959931f},     // left eye
    {-0.698132f, 0.942478f, 0.767945f, -0.959931f},     // right eye
};


struct Head {
    float pos[3] = {0, 0, 0};
    float yaw = 0, pitch = 0;
};

Quat head_orientation(const Head &h)
{
    return quat_mul(quat_axis_angle(0, 1, 0, h.yaw), quat_axis_angle(1, 0, 0, h.pitch));
}

void make_eyes(const Head &h, gpu::Eye eyes[2])
{
    Quat q = head_orientation(h);
    float rx = std::cos(h.yaw), rz = -std::sin(h.yaw);      // the head's local x (pitch leaves it in the yaw plane)
    for (int i = 0; i < 2; ++i) {
        float side = (i == 0 ? -0.5f : 0.5f) * kIpd;
        gpu::Eye &e = eyes[i];
        e.pos[0] = h.pos[0] + rx * side;
        e.pos[1] = h.pos[1];
        e.pos[2] = h.pos[2] + rz * side;
        e.view = mat4_view_from_pose(q, e.pos[0], e.pos[1], e.pos[2]);
        const Fov &f = kEyeFov[i];
        e.proj = mat4_projection_reverse_z(f.left, f.right, f.up, f.down, gpu::kEyeNear, gpu::kEyeFar);
    }
}

int64_t steady_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct EyeImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation mem = VK_NULL_HANDLE;
};

// app/shots.h's requests, taken after the frame they were made in.
struct ShotRequest {
    gpu::Eye eye;
    uint32_t width, height;
    std::string path;
};
std::vector<ShotRequest> g_shots;

// An RGBA picture (top row first, the bytes as they are: sRGB) as a PNG: zlib with stored (uncompressed)
// blocks, so no library.
bool write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    static uint32_t crc_table[256];
    if (!crc_table[1])
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
    auto be32 = [](std::vector<uint8_t> &v, uint32_t x) {
        for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
    };
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (w * 4 + 1));
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);                                   // filter: none
        raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t at = 0; at < raw.size() || at == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - at);
        bool last = at + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32(out, (uint32_t)data.size());
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        uint32_t c = 0xffffffffu;
        for (size_t i = start; i < out.size(); ++i) c = crc_table[(c ^ out[i]) & 0xff] ^ (c >> 8);
        be32(out, c ^ 0xffffffffu);
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});               // 8 bits, RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::FILE *fp = std::fopen(path, "wb");
    if (!fp) {
        log("sim: cannot write %s", path);
        return false;
    }
    bool ok = std::fwrite(out.data(), 1, out.size(), fp) == out.size();
    std::fclose(fp);
    if (!ok) log("sim: short write to %s", path);
    return ok;
}

struct Sim {
    const AppInfo *info = nullptr;
    Options *opt = nullptr;                          // the launch's, then the graphics in force
    bool headless = false;

    // loading
    bool sdl_vulkan_loaded = false;
    SDL_SharedObject *lib = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    const char *lib_path = nullptr;

    // window and swapchain
    SDL_Window *window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat sc_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR sc_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D sc_extent = {};
    std::vector<VkImage> sc_images;
    std::vector<VkSemaphore> done_sem;               // per swapchain image
    VkSemaphore acquire_sem[gpu::kFramesInFlight] = {};
    bool sc_dirty = true;

    // gpu
    gpu::Device dev;
    bool dev_ok = false;
    VkExtent2D eye_size = {};
    EyeImage eye_images[gpu::kFramesInFlight];
    gpu::EyeTarget target;
    gpu::RendererImpl r;
    bool app_ok = false;

    // view shots (app/shots.h)
    EyeImage shot_image;
    gpu::EyeTarget shot_target;
    VkExtent2D shot_size = {};

    // fake head
    Head head;
    bool looking = false;
    bool side_by_side = false;
    bool quit = false;
    int64_t period_ns = 0;                          // the display's (windowed) or --hz's (headless)
    uint32_t recentres = 0;                         // R presses: the headset's recentre
    bool left_pressed = false;                      // the left button went down since the last update (Input::mouse)

    // the graphics asks (request_graphics)
    float eye_pending = 0;                          // an eye resolution asked for, made at eye_at (s); 0: none
    double eye_at = 0;
    long updates = 0;                               // display frames updated (the --graphics-test schedule)
    int remakes = 0;

    int run(const AppInfo &i, Options &o);
    void teardown();

    bool load_vulkan();
    bool make_device();
    bool make_eye_images();
    void destroy_eye_images();
    bool make_eye_target();
    bool eye_remake();
    bool apply_graphics();
    void graphics_test_step();
    bool make_window_surface();
    bool make_swapchain();
    void destroy_swapchain();
    bool make_acquire_semaphores();

    void poll(float dt);
    void fill_mouse(Mouse &m);
    void update(int64_t display_ns);
    void record_blit(VkCommandBuffer cmd, uint32_t eye_slot, VkImage dst);
    bool frame_windowed(double &cpu_s);
    bool run_shots();
    void destroy_shot_target();
    int loop_headless();
    int loop_windowed();
};

// The env override wins over the path CMake baked in.
bool load_vulkan_path(const char *&out)
{
    const char *env = std::getenv("VRCORE_VULKAN_LIBRARY");
    if (env && *env) { out = env; return true; }
#ifdef VRCORE_MOLTENVK_PATH
    out = VRCORE_MOLTENVK_PATH;
    return true;
#else
    out = nullptr;
    return false;
#endif
}

bool Sim::load_vulkan()
{
    if (!load_vulkan_path(lib_path)) {
        log("sim: no Vulkan library: set VRCORE_VULKAN_LIBRARY (to libMoltenVK.dylib)");
        return false;
    }
    if (headless) {
        lib = SDL_LoadObject(lib_path);
        if (!lib) { log("sim: loading %s failed: %s", lib_path, SDL_GetError()); return false; }
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(lib, "vkGetInstanceProcAddr"));
        if (!gipa) { log("sim: %s has no vkGetInstanceProcAddr: %s", lib_path, SDL_GetError()); return false; }
    } else {
        if (!SDL_Vulkan_LoadLibrary(lib_path)) {
            log("sim: loading %s failed: %s", lib_path, SDL_GetError());
            return false;
        }
        sdl_vulkan_loaded = true;
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (!gipa) { log("sim: SDL has no vkGetInstanceProcAddr: %s", SDL_GetError()); return false; }
    }
    return true;
}

bool Sim::make_device()
{
    gpu::DeviceHooks hooks;
    hooks.get_instance_proc_addr = gipa;
    hooks.want_fdm = false;                      // MoltenVK has no fragment density map
    if (!headless) {
        Uint32 n = 0;
        const char *const *ext = SDL_Vulkan_GetInstanceExtensions(&n);
        if (!ext) { log("sim: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError()); return false; }
        for (Uint32 k = 0; k < n; ++k) hooks.instance_extensions.push_back(ext[k]);
        hooks.device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (!gpu::device_create(dev, hooks, info->name)) { log("sim: device_create failed"); return false; }
    dev_ok = true;
    return true;
}

bool Sim::make_eye_images()
{
    eye_size.width = (uint32_t)std::lround(kQuestEyeWidth * opt->eye_scale);
    eye_size.height = (uint32_t)std::lround(kQuestEyeHeight * opt->eye_scale);

    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_SRGB;
    ci.extent = {eye_size.width, eye_size.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 2;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    for (EyeImage &e : eye_images)
        VRCORE_VK(vmaCreateImage(dev.vma, &ci, &ai, &e.image, &e.mem, nullptr), false);
    return true;
}

void Sim::destroy_eye_images()
{
    for (EyeImage &e : eye_images) {
        if (e.image) vmaDestroyImage(dev.vma, e.image, e.mem);
        e = EyeImage();
    }
}

bool Sim::make_eye_target()
{
    gpu::EyeTargetDesc desc;
    desc.width = eye_size.width;
    desc.height = eye_size.height;
    desc.color_format = VK_FORMAT_R8G8B8A8_SRGB;
    for (const EyeImage &e : eye_images) desc.images.push_back(e.image);
    if (!gpu::eye_target_create(dev, target, desc)) { log("sim: eye_target_create failed"); return false; }
    return true;
}

// A new eye resolution (opt->eye_scale): the eye images and target made again, as the headset's swapchain.
bool Sim::eye_remake()
{
    auto t0 = std::chrono::steady_clock::now();
    vkDeviceWaitIdle(dev.device);
    gpu::eye_target_destroy(dev, target);
    destroy_eye_images();
    if (!make_eye_images() || !make_eye_target()) return false;
    ++remakes;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log("sim: eye remade: %ux%u (x%.2f) in %.1f ms", eye_size.width, eye_size.height, opt->eye_scale, ms);
    return true;
}

// As the headset's (xr.cpp apply_graphics). False (logged): the loop cannot go on.
bool Sim::apply_graphics()
{
    Options &o = *opt;
    Graphics g;
    if (take_graphics(g)) {
        if (std::fabs(g.hz - o.hz) > 0.01f || g.foveation != o.foveation || g.sharpen != o.sharpen)
            log("sim: rate %.0f Hz, foveation %d, sharpening %d asked (no headset: logged only)", g.hz, g.foveation,
                g.sharpen);
        o.hz = g.hz;
        o.foveation = g.foveation;
        o.sharpen = g.sharpen;
        float eye = std::clamp(g.eye_scale, 1.f, 3.f);
        if (std::fabs(eye - (eye_pending > 0 ? eye_pending : o.eye_scale)) > 1e-4f) {
            eye_pending = eye;
            eye_at = steady_ns() * 1e-9 + 0.5;
        }
    }
    if (eye_pending > 0 && steady_ns() * 1e-9 >= eye_at) {
        bool changed = std::fabs(eye_pending - o.eye_scale) > 1e-4f;
        o.eye_scale = eye_pending;
        eye_pending = 0;
        if (changed && !eye_remake()) return false;
    }
    return true;
}

// --graphics-test: the asks by display frame, then the check (the remakes made, the size in force)
void Sim::graphics_test_step()
{
    if (!opt->graphics_test) return;
    Graphics g;
    g.hz = opt->hz;
    g.eye_scale = opt->eye_scale;
    g.foveation = opt->foveation;
    g.sharpen = opt->sharpen;
    switch (updates) {
    case 100: g.eye_scale = 1.4f; break;
    case 110: g.eye_scale = 1.5f; break;           // within 0.5 s of the last: one remake, at x1.5
    case 250: g.eye_scale = 1.3f; break;
    case 350: {
        bool ok = remakes == 2 && std::fabs(opt->eye_scale - 1.3f) < 1e-4f &&
                  eye_size.width == (uint32_t)std::lround(kQuestEyeWidth * 1.3f);
        log("graphics-test: %s: %d remakes (2 expected), eye %ux%u x%.2f", ok ? "PASS" : "FAIL", remakes,
            eye_size.width, eye_size.height, opt->eye_scale);
        if (!ok) request_quit(1);
        return;
    }
    default: return;
    }
    log("graphics-test: frame %ld asks eye x%.2f", updates, g.eye_scale);
    request_graphics(g);
}

bool Sim::make_window_surface()
{
    window = SDL_CreateWindow(info->name, 1280, 720,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) { log("sim: SDL_CreateWindow failed: %s", SDL_GetError()); return false; }
    return true;
}

bool Sim::make_acquire_semaphores()
{
    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (VkSemaphore &s : acquire_sem) VRCORE_VK(vkCreateSemaphore(dev.device, &sci, nullptr, &s), false);
    return true;
}

void Sim::destroy_swapchain()
{
    for (VkSemaphore s : done_sem) vkDestroySemaphore(dev.device, s, nullptr);
    done_sem.clear();
    sc_images.clear();
    if (swapchain) vkDestroySwapchainKHR(dev.device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

// (Re)creates the window swapchain. Returns false on a failure; a zero-sized window is not one (sc_extent stays
// 0 and the caller skips frames).
bool Sim::make_swapchain()
{
    vkDeviceWaitIdle(dev.device);

    VkSurfaceCapabilitiesKHR caps;
    VRCORE_VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.phys, surface, &caps), false);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        log("sim: the surface does not allow TRANSFER_DST swapchain images");
        return false;
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        ext.width = std::clamp((uint32_t)std::max(w, 0), caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp((uint32_t)std::max(h, 0), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (ext.width == 0 || ext.height == 0) {
        destroy_swapchain();
        sc_extent = {};
        sc_dirty = false;
        return true;
    }

    if (sc_format == VK_FORMAT_UNDEFINED) {
        uint32_t n = 0;
        VRCORE_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(dev.phys, surface, &n, nullptr), false);
        std::vector<VkSurfaceFormatKHR> formats(n);
        VRCORE_VK(vkGetPhysicalDeviceSurfaceFormatsKHR(dev.phys, surface, &n, formats.data()), false);
        if (formats.empty()) { log("sim: the surface offers no formats"); return false; }
        VkSurfaceFormatKHR pick = formats[0];
        bool found = false;
        for (VkFormat want : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
            for (const VkSurfaceFormatKHR &f : formats)
                if (!found && f.format == want) { pick = f; found = true; }
        }
        if (!found) log("sim: no sRGB window format offered; colours will look off (using %d)", (int)pick.format);
        sc_format = pick.format;
        sc_color_space = pick.colorSpace;
    }

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (VkCompositeAlphaFlagBitsKHR a :
             {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) { alpha = a; break; }
    }

    VkSwapchainCreateInfoKHR sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface;
    sci.minImageCount = count;
    sci.imageFormat = sc_format;
    sci.imageColorSpace = sc_color_space;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = alpha;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = swapchain;

    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VkResult res = vkCreateSwapchainKHR(dev.device, &sci, nullptr, &fresh);
    for (VkSemaphore s : done_sem) vkDestroySemaphore(dev.device, s, nullptr);
    done_sem.clear();
    if (swapchain) vkDestroySwapchainKHR(dev.device, swapchain, nullptr);
    swapchain = fresh;
    if (res != VK_SUCCESS) {
        log("vulkan: vkCreateSwapchainKHR failed: %d", (int)res);
        return false;
    }
    sc_extent = ext;

    uint32_t n = 0;
    VRCORE_VK(vkGetSwapchainImagesKHR(dev.device, swapchain, &n, nullptr), false);
    sc_images.resize(n);
    VRCORE_VK(vkGetSwapchainImagesKHR(dev.device, swapchain, &n, sc_images.data()), false);

    VkSemaphoreCreateInfo semi = {};
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    done_sem.resize(n, VK_NULL_HANDLE);
    for (VkSemaphore &s : done_sem) VRCORE_VK(vkCreateSemaphore(dev.device, &semi, nullptr, &s), false);

    sc_dirty = false;
    log("sim: window swapchain format %d, %ux%u, %u images", (int)sc_format, ext.width, ext.height, n);
    return true;
}

void Sim::poll(float dt)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        pads_event(&ev);
        switch (ev.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            quit = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
            sc_dirty = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (ev.key.repeat) break;
            if (ev.key.key == SDLK_TAB) side_by_side = !side_by_side;   // Esc is the front's (the menu): close the window to quit
            else if (ev.key.key == SDLK_R) {
                head = Head();
                ++recentres;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) left_pressed = true;
            if (ev.button.button == SDL_BUTTON_RIGHT) {
                looking = true;
                SDL_SetWindowRelativeMouseMode(window, true);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (ev.button.button == SDL_BUTTON_RIGHT) {
                looking = false;
                SDL_SetWindowRelativeMouseMode(window, false);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (looking) {
                head.yaw -= ev.motion.xrel * kMouseSens;
                head.pitch -= ev.motion.yrel * kMouseSens;
            }
            break;
        default:
            break;
        }
    }

    const bool *k = SDL_GetKeyboardState(nullptr);
    head.pitch = std::clamp(head.pitch, -1.55f, 1.55f); // the mouse's look too
    if (!(SDL_GetModState() & SDL_KMOD_ALT)) return;    // the keys are the front's
    if (k[SDL_SCANCODE_LEFT]) head.yaw += kLookRate * dt;
    if (k[SDL_SCANCODE_RIGHT]) head.yaw -= kLookRate * dt;
    if (k[SDL_SCANCODE_UP]) head.pitch += kLookRate * dt;
    if (k[SDL_SCANCODE_DOWN]) head.pitch -= kLookRate * dt;
    head.pitch = std::clamp(head.pitch, -1.55f, 1.55f);

    float fwd = (k[SDL_SCANCODE_W] ? 1.f : 0.f) - (k[SDL_SCANCODE_S] ? 1.f : 0.f);
    float strafe = (k[SDL_SCANCODE_D] ? 1.f : 0.f) - (k[SDL_SCANCODE_A] ? 1.f : 0.f);
    float rise = (k[SDL_SCANCODE_E] ? 1.f : 0.f) - (k[SDL_SCANCODE_Q] ? 1.f : 0.f);
    float speed = kMoveSpeed * ((k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) ? 4.f : 1.f) * dt;
    float s = std::sin(head.yaw), c = std::cos(head.yaw);
    head.pos[0] += (-s * fwd + c * strafe) * speed;
    head.pos[2] += (-c * fwd - s * strafe) * speed;
    head.pos[1] += rise * speed;
}

// Input::mouse: the pointer in the window's pixels and the eye picture under it (the blits' layout, record_blit).
void Sim::fill_mouse(Mouse &m)
{
    m = Mouse();
    if (!window) return;                                // headless
    m.present = true;
    m.left_pressed = left_pressed;
    left_pressed = false;
    float lx = 0, ly = 0;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&lx, &ly);
    m.left = buttons & SDL_BUTTON_LMASK;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    float w = (float)sc_extent.width, h = (float)sc_extent.height;
    if (ww <= 0 || wh <= 0 || w <= 0 || h <= 0) return;
    m.x = lx * w / (float)ww;
    m.y = ly * h / (float)wh;
    m.nx = m.x / w;
    m.ny = m.y / h;
    m.inside = m.nx >= 0 && m.nx < 1 && m.ny >= 0 && m.ny < 1;
    if (!m.inside) return;
    // the cells fit_blit aspect-fits an eye into
    int cells = side_by_side ? 2 : 1;
    for (int e = 0; e < cells; ++e) {
        float cx = e * (w / 2), cw = side_by_side ? (e == 0 ? (float)((int)w / 2) : w - (float)((int)w / 2)) : w;
        float scale = std::min(cw / (float)eye_size.width, h / (float)eye_size.height);
        float dw = std::max(1.f, std::round(eye_size.width * scale)), dh = std::max(1.f, std::round(eye_size.height * scale));
        float x0 = cx + std::floor((cw - dw) / 2), y0 = std::floor((h - dh) / 2);
        if (m.x >= x0 && m.x < x0 + dw && m.y >= y0 && m.y < y0 + dh) {
            m.eye = e;
            m.eye_u = (m.x - x0) / dw;
            m.eye_v = (m.y - y0) / dh;
            return;
        }
    }
}

// The front's update for a frame shown at `display_ns`; a quit it asks for ends the loop.
void Sim::update(int64_t display_ns)
{
    if (opt->head_lock) head = Head();                  // --head-lock: the fake head stays at the seat
    Input &in = input_fill();
    in.hands[0] = Hand();
    in.hands[1] = Hand();
    in.head_tracked = true;
    in.head.rot = head_orientation(head);
    for (int i = 0; i < 3; ++i) in.head.pos[i] = head.pos[i];
    fill_mouse(in.mouse);
    input_begin_frame(steady_ns() * 1e-9);
    if (in.alt) {                                       // Option held: the keys are the fake head's, not the front's
        std::fill(std::begin(in.keys), std::end(in.keys), false);
        std::fill(std::begin(in.pressed), std::end(in.pressed), false);
    }
    ++updates;
    graphics_test_step();
    if (info->update) {
        Tick t;
        t.display_ns = display_ns;
        t.period_ns = period_ns;
        t.shown = true;
        t.recentres = recentres;
        info->update(t);
    }
    if (quit_requested()) quit = true;
    else if (take_restart()) {          // app.h's request_restart: the front again from its start, same window
        vkDeviceWaitIdle(dev.device);
        log("sim: restart: %s shut down and started again", info->name);
        info->shutdown(r.renderer);
        if (!info->init(r.renderer, *opt)) {
            log("sim: %s init failed on the restart", info->name);
            quit = true;
        }
    }
}

void image_barrier(VkCommandBuffer cmd, VkImage image, uint32_t layers, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                   VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Aspect-fits `src` (w x h) into the cell (cx, cy, cw, ch) of the window.
VkImageBlit fit_blit(uint32_t layer, VkExtent2D src, int cx, int cy, int cw, int ch)
{
    float scale = std::min((float)cw / (float)src.width, (float)ch / (float)src.height);
    int dw = std::max(1, (int)std::lround(src.width * scale));
    int dh = std::max(1, (int)std::lround(src.height * scale));
    int x0 = cx + (cw - dw) / 2, y0 = cy + (ch - dh) / 2;

    VkImageBlit b = {};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
    b.srcOffsets[1] = {(int32_t)src.width, (int32_t)src.height, 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[0] = {x0, y0, 0};
    b.dstOffsets[1] = {x0 + dw, y0 + dh, 1};
    return b;
}

// After the eye pass: eye image (both layers) to TRANSFER_SRC, the window image cleared and the eye(s) blitted in.
void Sim::record_blit(VkCommandBuffer cmd, uint32_t eye_slot, VkImage dst)
{
    VkImage src = eye_images[eye_slot].image;
    image_barrier(cmd, src, 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    image_barrier(cmd, dst, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkClearColorValue black = {};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

    VkMemoryBarrier waw = {};
    waw.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    waw.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    waw.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &waw, 0, nullptr,
                         0, nullptr);

    int w = (int)sc_extent.width, h = (int)sc_extent.height;
    VkImageBlit blits[2];
    uint32_t n = 1;
    if (side_by_side) {
        blits[0] = fit_blit(0, eye_size, 0, 0, w / 2, h);
        blits[1] = fit_blit(1, eye_size, w / 2, 0, w - w / 2, h);
        n = 2;
    } else {
        blits[0] = fit_blit(0, eye_size, 0, 0, w, h);
    }
    for (uint32_t i = 0; i < n; ++i)
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blits[i], VK_FILTER_LINEAR);

    image_barrier(cmd, dst, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
}

void Sim::destroy_shot_target()
{
    if (!dev_ok) return;
    vkDeviceWaitIdle(dev.device);
    gpu::eye_target_destroy(dev, shot_target);
    if (shot_image.image) vmaDestroyImage(dev.vma, shot_image.image, shot_image.mem);
    shot_image = EyeImage();
    shot_size = {};
}

// The view shots asked for in the frame just drawn: a frame each into a shot-sized eye target, its first view
// read back and written as a PNG. False when the GPU work failed.
bool Sim::run_shots()
{
    std::vector<ShotRequest> list;
    list.swap(g_shots);
    for (const ShotRequest &s : list) {
        if (shot_size.width != s.width || shot_size.height != s.height) {
            destroy_shot_target();
            VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = VK_FORMAT_R8G8B8A8_SRGB;
            ci.extent = {s.width, s.height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 2;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo ai = {};
            ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            VRCORE_VK(vmaCreateImage(dev.vma, &ci, &ai, &shot_image.image, &shot_image.mem, nullptr), false);
            gpu::EyeTargetDesc desc;
            desc.width = s.width;
            desc.height = s.height;
            desc.color_format = VK_FORMAT_R8G8B8A8_SRGB;
            desc.images.push_back(shot_image.image);
            if (!gpu::eye_target_create(dev, shot_target, desc)) return false;
            shot_size = {s.width, s.height};
        }
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readback_mem = VK_NULL_HANDLE;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(s.width) * s.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out = {};
        VRCORE_VK(vmaCreateBuffer(dev.vma, &bci, &aci, &readback, &readback_mem, &out), false);

        gpu::Eye eyes[2] = {s.eye, s.eye};
        gpu::Frame &f = gpu::renderer_begin(r, eyes);
        if (info->prepare) info->prepare(f);
        gpu::renderer_begin_eye_pass(r, 0, info->clear ? info->clear : gpu::kClear, &shot_target);
        info->draw(f);
        layers_draw(f);
        gpu::renderer_end_pass(r);
        VkCommandBuffer cmd = gpu::renderer_cmd(r);
        image_barrier(cmd, shot_image.image, 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {s.width, s.height, 1};
        vkCmdCopyImageToBuffer(cmd, shot_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = readback;
        bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0,
                             nullptr);
        bool ok = gpu::renderer_submit(r) && vkQueueWaitIdle(dev.queue) == VK_SUCCESS;
        if (ok) {
            vmaInvalidateAllocation(dev.vma, readback_mem, 0, VK_WHOLE_SIZE);
            if (write_png(s.path.c_str(), static_cast<const uint8_t *>(out.pMappedData), s.width, s.height))
                log("sim: view shot %s (%ux%u)", s.path.c_str(), s.width, s.height);
        }
        vmaDestroyBuffer(dev.vma, readback, readback_mem);
        if (!ok) return false;
    }
    return true;
}

// One windowed frame. `cpu_s` gets the time spent recording and submitting (not waiting on the GPU or the display).
bool Sim::frame_windowed(double &cpu_s)
{
    using clock = std::chrono::steady_clock;
    auto secs = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };

    gpu::Eye eyes[2];
    make_eyes(head, eyes);

    uint32_t slot = gpu::renderer_next_slot(r);
    gpu::Frame &f = gpu::renderer_begin(r, eyes);                       // waits the slot's fence
    auto t0 = clock::now();
    if (info->prepare) info->prepare(f);
    gpu::renderer_begin_eye_pass(r, slot, info->clear ? info->clear : gpu::kClear);
    info->draw(f);
    layers_draw(f);
    gpu::renderer_end_pass(r);
    auto t1 = clock::now();

    // The slot's last submit is done, so its acquire semaphore is free again.
    uint32_t image = 0;
    VkResult ar = vkAcquireNextImageKHR(dev.device, swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &image);
    auto t2 = clock::now();
    bool have_image = ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR;
    if (ar == VK_SUBOPTIMAL_KHR || ar == VK_ERROR_OUT_OF_DATE_KHR) sc_dirty = true;
    if (!have_image && ar != VK_ERROR_OUT_OF_DATE_KHR) {
        log("vulkan: vkAcquireNextImageKHR failed: %d", (int)ar);
        gpu::renderer_submit(r);
        return false;
    }

    if (!have_image) {
        gpu::renderer_submit(r);                    // the pass was begun; finish it, show nothing
        cpu_s = secs(t0, t1) + secs(t2, clock::now());
        return true;
    }

    record_blit(gpu::renderer_cmd(r), slot, sc_images[image]);
    if (!gpu::renderer_submit(r, acquire_sem[slot], VK_PIPELINE_STAGE_TRANSFER_BIT, done_sem[image])) {
        log("sim: renderer_submit failed");
        return false;
    }
    auto t3 = clock::now();

    VkPresentInfoKHR pi = {};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done_sem[image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &image;
    VkResult pr = vkQueuePresentKHR(dev.queue, &pi);
    if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR) sc_dirty = true;
    else if (pr != VK_SUCCESS) {
        log("vulkan: vkQueuePresentKHR failed: %d", (int)pr);
        return false;
    }
    cpu_s = secs(t0, t1) + secs(t2, t3);
    return true;
}

int Sim::loop_windowed()
{
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    auto window_start = last;
    long frames = 0, shown = 0;
    double cpu_sum = 0;

    while (!quit && (opt->frames < 0 || shown < opt->frames)) {
        auto now = clock::now();
        float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
        last = now;
        poll(dt);
        if (quit) break;
        // FIFO: what is drawn now is shown about a refresh from now.
        SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
        float hz = mode && mode->refresh_rate > 0 ? mode->refresh_rate : 60.f;
        period_ns = (int64_t)(1e9 / hz);
        update(steady_ns() + period_ns);
        if (quit) break;
        if (!apply_graphics()) return 1;

        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        if (pw <= 0 || ph <= 0) {                   // minimised
            SDL_Delay(50);
            sc_dirty = true;
            continue;
        }
        if (sc_dirty && !make_swapchain()) return 1;
        if (!swapchain) { SDL_Delay(50); continue; }

        double cpu_s = 0;
        if (!frame_windowed(cpu_s) || !run_shots()) return 1;
        cpu_sum += cpu_s;
        ++frames;
        ++shown;

        double span = std::chrono::duration<double>(clock::now() - window_start).count();
        if (span >= 5.0) {
            log("sim: %ld frames in %.1f s (%.1f fps), cpu %.2f ms, gpu %.2f ms", frames, span, frames / span,
                cpu_sum / frames * 1000.0, r.gpu_ms);
            frames = 0;
            cpu_sum = 0;
            window_start = clock::now();
        }
    }
    int code = 0;
    quit_requested(&code);
    return code;
}

// The left eye of the last frame as a binary PPM. The bytes are already sRGB-encoded.
bool write_ppm(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    std::FILE *fp = std::fopen(path, "wb");
    if (!fp) { log("sim: cannot write %s", path); return false; }
    std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *p = rgba + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 3 + 0] = p[x * 4 + 0];
            row[x * 3 + 1] = p[x * 4 + 1];
            row[x * 3 + 2] = p[x * 4 + 2];
        }
        if (std::fwrite(row.data(), 1, row.size(), fp) != row.size()) {
            log("sim: short write to %s", path);
            std::fclose(fp);
            return false;
        }
    }
    std::fclose(fp);
    return true;
}

int Sim::loop_headless()
{
    // --frames N, else 3 for a --shot or a front without an update (the test scene), else until the front quits
    // (or Ctrl-C); paced at --hz.
    long frames = opt->frames >= 0 ? opt->frames : opt->shot || !info->update ? 3 : -1;
    if (frames == 0) frames = 1;
    period_ns = (int64_t)(1e9 / std::max(opt->hz, 1.f));

    gpu::Eye eyes[2];
    make_eyes(head, eyes);

    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readback_mem = VK_NULL_HANDLE;
    uint8_t *mapped = nullptr;
    VkDeviceSize bytes = VkDeviceSize(eye_size.width) * eye_size.height * 4;
    if (opt->shot) {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo out = {};
        VRCORE_VK(vmaCreateBuffer(dev.vma, &bci, &aci, &readback, &readback_mem, &out), 1);
        mapped = static_cast<uint8_t *>(out.pMappedData);
    }

    int rc = 0;
    bool shot_recorded = false;
    int64_t next = steady_ns();
    long n = 0;
    for (; (frames < 0 || n < frames) && rc == 0; ++n) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(next - steady_ns()));
        next += period_ns;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            pads_event(&ev);
            if (ev.type == SDL_EVENT_QUIT) quit = true;
        }
        if (!quit) update(next);
        if (quit) break;
        if (!apply_graphics()) { rc = 1; break; }
        uint32_t slot = gpu::renderer_next_slot(r);
        gpu::Frame &f = gpu::renderer_begin(r, eyes);
        if (info->prepare) info->prepare(f);
        gpu::renderer_begin_eye_pass(r, slot, info->clear ? info->clear : gpu::kClear);
        info->draw(f);
        layers_draw(f);
        gpu::renderer_end_pass(r);

        bool last = frames > 0 && n == frames - 1;
        if (last && opt->shot && VkDeviceSize(eye_size.width) * eye_size.height * 4 != bytes) {
            log("sim: the eye resolution changed during the run; --shot not taken");
            rc = 1;
        } else if (last && opt->shot) {
            shot_recorded = true;
            VkCommandBuffer cmd = gpu::renderer_cmd(r);
            image_barrier(cmd, eye_images[slot].image, 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy copy = {};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {eye_size.width, eye_size.height, 1};
            vkCmdCopyImageToBuffer(cmd, eye_images[slot].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                                   &copy);
            VkBufferMemoryBarrier bb = {};
            bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = readback;
            bb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &bb, 0, nullptr);
        }
        if (!gpu::renderer_submit(r)) { log("sim: renderer_submit failed"); rc = 1; break; }
        if (!run_shots()) { log("sim: a view shot failed"); rc = 1; break; }
    }

    if (rc == 0 && shot_recorded) {
        if (vkQueueWaitIdle(dev.queue) != VK_SUCCESS) {
            log("sim: vkQueueWaitIdle failed");
            rc = 1;
        } else {
            vmaInvalidateAllocation(dev.vma, readback_mem, 0, VK_WHOLE_SIZE);
            if (!mapped || !write_ppm(opt->shot, mapped, eye_size.width, eye_size.height)) rc = 1;
            else log("sim: wrote %s (%ux%u)", opt->shot, eye_size.width, eye_size.height);
        }
    }
    if (readback) {
        vkDeviceWaitIdle(dev.device);
        vmaDestroyBuffer(dev.vma, readback, readback_mem);
    }
    log("sim: headless, %ld frames", n);
    int code = 0;
    if (rc == 0 && quit_requested(&code)) rc = code;
    return rc;
}

int Sim::run(const AppInfo &i, Options &o)
{
    info = &i;
    opt = &o;
    headless = o.headless || o.shot;

    if (!load_vulkan()) return 1;
    if (!headless && !make_window_surface()) return 1;
    if (!make_device()) return 1;

    if (!headless) {
        if (!SDL_Vulkan_CreateSurface(window, dev.instance, nullptr, &surface)) {
            log("sim: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            return 1;
        }
        VkBool32 present = VK_FALSE;
        VRCORE_VK(vkGetPhysicalDeviceSurfaceSupportKHR(dev.phys, dev.queue_family, surface, &present), 1);
        if (!present) {
            log("sim: queue family %u cannot present to the window", dev.queue_family);
            return 1;
        }
        if (!make_acquire_semaphores()) return 1;
    }

    // the headset: none (the front's device profile matches nothing: the Quest 3's)
    Headset hs;
    hs.system = "sim";
    hs.gpu = dev.props.deviceName;
    set_headset(*info, hs, o);

    if (!make_eye_images()) return 1;
    log("sim: eye %ux%u x2 layers (x%.2f), vulkan %s", eye_size.width, eye_size.height, o.eye_scale, lib_path);
    if (!make_eye_target()) return 1;
    if (!gpu::renderer_create(r, dev, target)) { log("sim: renderer_create failed"); return 1; }
    if (!layers_init(r.renderer)) { log("sim: the layers' pipelines failed"); return 1; }

    app_ok = true;                                  // shut down even after a failed init: it frees what init made
    if (!info->init(r.renderer, o)) { log("sim: %s init failed", info->name); return 1; }

    return headless ? loop_headless() : loop_windowed();
}

void Sim::teardown()
{
    if (dev_ok) vkDeviceWaitIdle(dev.device);
    destroy_shot_target();
    if (app_ok) info->shutdown(r.renderer);
    if (r.dev) layers_shutdown(r.renderer);
    gpu::renderer_destroy(r);                       // also a half-made one (it checks)
    if (dev_ok) {
        gpu::eye_target_destroy(dev, target);
        destroy_eye_images();
        destroy_swapchain();
        for (VkSemaphore &s : acquire_sem)
            if (s) vkDestroySemaphore(dev.device, s, nullptr);
        if (surface) vkDestroySurfaceKHR(dev.instance, surface, nullptr);
        gpu::device_destroy(dev);
    }
    if (window) SDL_DestroyWindow(window);
    if (sdl_vulkan_loaded) SDL_Vulkan_UnloadLibrary();
    if (lib) SDL_UnloadObject(lib);
}

}

int run(const AppInfo &info, const Options &launch)
{
    Options o = launch;                             // the graphics in force change while it runs
    pads_init();
    Sim s;
    int rc = s.run(info, o);
    pads_shutdown();
    s.teardown();
    return rc;
}

}

namespace vrcore {

bool request_view_shot(const gpu::Eye &eye, uint32_t width, uint32_t height, const char *png_path)
{
    if (!width || !height || !png_path) return false;
    sim::g_shots.push_back({eye, width, height, png_path});
    return true;
}

}
