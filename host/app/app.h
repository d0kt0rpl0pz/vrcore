/*
 * app.h -- the host's entry: a game's front names itself and its callbacks (front_info), and the
 * host runs it: on Android through OpenXR (xr/), on the desktop in the sim (sim/).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrcore {

namespace gpu {
class Renderer;
class Frame;
}

// Launch options. Desktop: the command line. Android: the intent extra "args"
// (am start ... --es args "--foveation 3 --fdm-view").
struct Options {
    long frames = -1;           // --frames N: quit after N display frames (smoke tests; headless sim without
                                //   it: 3 frames with --shot, else until the front quits)
    float hz = 120.f;           // --hz: the refresh rate asked for (the listed rate nearest it)
    float eye_scale = 1.3f;     // --eye: x the runtime's recommended eye size
    int foveation = 2;          // --foveation 0-3: Off, Low, Medium, High
    int sharpen = 0;            // --sharpen 0-2: Off, Normal, Quality (the compositor's, where the headset has it)
    float foveation_cycle = 0;  // --foveation-cycle S: (test only) step the level every S seconds (0: off)
    bool fdm_view = false;      // --fdm-view: the front colours fragments by their density size
    int cpu_level = -1;         // --cpu-level / --gpu-level 0-3: powersave, sustained_low,
    int gpu_level = -1;         //   sustained_high, boost (-1: not set, the runtime's own)
    const char *shot = nullptr; // --shot FILE: (sim) write the left eye as a PPM at the last frame
    bool headless = false;      // --headless: (sim) no window; implied by --shot
    bool pick_test = false;     // --pick-test: open SDL's file dialog and log what it gives
    const char *rig_test = nullptr; // --rig-test DIR: (Mac) run the rig's device-free checks into DIR, then quit:
                                //   exit 0 = all passed
    const char *audio_test = nullptr;   // --audio-test DIR: (Mac) the sound's device-free checks, the same way
    const char *settings_test = nullptr;    // --settings-test DIR: (Mac) the settings file's device-free checks
    bool graphics_test = false; // --graphics-test: (sim) asks the eye resolution by display frame (sim.cpp)
    bool spatial = true;        // --spatial 0|1: the cabinet speakers through Steam Audio, or the plain stereo
    bool head_lock = false;     // --head-lock: the eyes held where the first frame found them, level and facing
                                //   forward; the head's pose is ignored (unworn test runs draw the same view)
};

// The graphics the host applies while it runs (the menu's Graphics rows; the launch's are Options').
struct Graphics {
    float hz = 120.f;           // the listed rate nearest it
    float eye_scale = 1.3f;     // x the runtime's recommended eye size, 1.0-3.0
    int foveation = 2;          // 0-3: Off, Low, Medium, High
    int sharpen = 0;            // 0-2: Off, Normal, Quality
};

// The headset the host runs on, known before its eye swapchain is made.
struct Headset {
    std::string system;         // XrSystemProperties.systemName ("Meta Quest 3"); the sim: "sim"
    std::string gpu;            // Vulkan's device name ("Adreno (TM) 740"; the sim: the Mac's GPU)
    std::string make, model;    // Android's ro.product.manufacturer / model ("Oculus", "Quest 3"); the sim: empty
    std::vector<float> rates;   // the runtime's refresh rates, as listed (empty: none known; the sim)
    bool foveation = false;     // fixed foveation offered
    bool sharpen = false;       // compositor sharpening offered (XR_FB_composition_layer_settings)
};

// One display frame's timing, given to the front before it is drawn.
struct Tick {
    int64_t display_ns = 0;     // when the frame is shown, on steady_clock (CLOCK_MONOTONIC on Android: the
                                //   clock a plugin's vr_now_ns reads)
    int64_t period_ns = 0;      // the display's frame period
    bool shown = false;         // a frame is drawn for it (false: the session is not visible yet / any more)
    uint32_t recentres = 0;     // the headset's own recentres so far (LOCAL space changes; the sim's R key)
    bool focused = true;        // the app has the input focus (OpenXR's FOCUSED; false under the system menu, with
                                //   the headset off, while the activity is paused; the sim: always)
};

struct AppInfo {
    const char *name;                                       // window title and log name
    bool (*init)(gpu::Renderer &r, const Options &o);       // pipelines, buffers
    void (*draw)(gpu::Frame &f);                            // every display frame, in the eye pass
    void (*shutdown)(gpu::Renderer &r);                     // also after a failed init (free what it made)
    // Optional (null: none).
    // A launch option the host does not know, at argv[i]: the words the front took (0: not its own; -1: bad).
    int (*option)(int argc, char **argv, int i);
    // Every display frame before it is drawn, and every ~10 ms while the session is not running.
    void (*update)(const Tick &t);
    // Every drawn frame, before the eye pass: uploads, dynamic buffers, uniforms, offscreen passes (render.h).
    void (*prepare)(gpu::Frame &f);
    // The eye pass's clear colour, linear RGBA (null: gpu::kClear, vrcore's dark grey).
    const float *clear;
    // Before the launch options are parsed: the front opens its settings (settings::open) and puts the saved choices
    // into `o` and its own arguments; flags given at launch then override them for this run (never saved).
    void (*load_settings)(Options &o);
    // Once the headset is known, before its eye swapchain is made: the front may put a device profile's graphics
    // into `o` (hz, eye_scale, foveation, sharpen); a value a launch flag gave is kept (the host puts it back).
    void (*headset)(const Headset &h, Options &o);
};

// Defined by each front (host/test/scene_front.cpp is the test scene).
const AppInfo &front_info();

// Runs the app until it quits.
int run(const AppInfo &info, int argc, char **argv);

// The front asks the host to quit (the session ends, then the app); run() returns `code`.
void request_quit(int code);
bool quit_requested(int *code = nullptr);

// The front asks for the game again from power-on (the menu's Restart game): after this update the host shuts the
// front down (the game stops, its saves are written) and starts it again, in the same process, window and XR
// session; the plugin is opened as a fresh copy (plugin.h). Not a relaunch of the activity
// on the Quest: a process started by the app itself got no head tracking from Horizon until the Meta button
// (2026-10-10: the eyes black, the game heard).
void request_restart();
// The host's side: true once, after a request.
bool take_restart();

// The headset this run is on (filled before AppInfo::headset is called; empty before).
const Headset &headset();

// The front asks for these graphics (the menu's rows). The host applies what changed between frames: the refresh
// rate, foveation and sharpening at once; the eye resolution 0.5 s after the last ask that changed it (a held row
// makes one swapchain, not one a step). The eye pass is always 4x MSAA. The sim takes the eye resolution, logs the rest.
void request_graphics(const Graphics &g);
// The host's side: the newest ask since the last take (false: none).
bool take_graphics(Graphics &g);
// The host's side: the headset found, then AppInfo::headset with the launch flags' values kept.
void set_headset(const AppInfo &info, const Headset &h, Options &o);

// The two hosts (app.cpp picks one).
namespace xr { int run(const AppInfo &info, const Options &o); }
namespace sim { int run(const AppInfo &info, const Options &o); }

}
