#include "app.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#endif

#include "log.h"
#include "../audio/audio.h"
#include "../audio/audio_test.h"
#include "../rig/rig_test.h"
#include "settings.h"
#ifndef __ANDROID__
#include "settings_test.h"
#endif

namespace vrcore {

namespace {

constexpr int kNoQuit = -0x7fffffff;
std::atomic<bool> g_quit{false};
std::atomic<int> g_quit_code{kNoQuit};
std::atomic<bool> g_restart{false};
Headset g_headset;
Graphics g_graphics_ask;                // the front's newest ask (one thread: the front asks from update)
bool g_graphics_asked = false;

// the graphics a launch flag gave (set_headset keeps them over a device profile)
enum : unsigned { kFlagHz = 1, kFlagEye = 2, kFlagFoveation = 4, kFlagSharpen = 8 };
unsigned g_flagged = 0;

bool parse(const AppInfo &info, Options &o, int argc, char **argv)
{
    static const char *const kValued[] = {"--frames", "--hz", "--eye", "--foveation", "--foveation-cycle",
                                          "--cpu-level", "--gpu-level", "--shot", "--rig-test",
                                          "--audio-test", "--spatial", "--settings", "--settings-test",
                                          "--sharpen"};
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!std::strcmp(a, "--fdm-view")) { o.fdm_view = true; continue; }
        if (!std::strcmp(a, "--headless")) { o.headless = true; continue; }
        if (!std::strcmp(a, "--pick-test")) { o.pick_test = true; continue; }
        if (!std::strcmp(a, "--graphics-test")) { o.graphics_test = true; continue; }
        if (!std::strcmp(a, "--head-lock")) { o.head_lock = true; continue; }
        bool known = false;
        for (const char *k : kValued) known |= !std::strcmp(a, k);
        if (!known) {
            int took = info.option ? info.option(argc, argv, i) : 0;
            if (took < 0) return false;                 // the front logged why
            if (took == 0) { log("app: unknown option %s", a); return false; }
            i += took - 1;
            continue;
        }
        if (i + 1 >= argc) { log("app: %s needs a value", a); return false; }
        const char *v = argv[++i];
        if (!std::strcmp(a, "--frames")) o.frames = std::atol(v);
        else if (!std::strcmp(a, "--hz")) o.hz = (float)std::atof(v), g_flagged |= kFlagHz;
        else if (!std::strcmp(a, "--eye")) o.eye_scale = (float)std::atof(v), g_flagged |= kFlagEye;
        else if (!std::strcmp(a, "--foveation")) o.foveation = std::atoi(v), g_flagged |= kFlagFoveation;
        else if (!std::strcmp(a, "--sharpen")) o.sharpen = std::atoi(v), g_flagged |= kFlagSharpen;
        else if (!std::strcmp(a, "--foveation-cycle")) o.foveation_cycle = (float)std::atof(v);
        else if (!std::strcmp(a, "--cpu-level")) o.cpu_level = std::atoi(v);
        else if (!std::strcmp(a, "--gpu-level")) o.gpu_level = std::atoi(v);
        else if (!std::strcmp(a, "--rig-test")) o.rig_test = v;
        else if (!std::strcmp(a, "--audio-test")) o.audio_test = v;
        else if (!std::strcmp(a, "--settings-test")) o.settings_test = v;
        else if (!std::strcmp(a, "--settings")) continue;       // run() took it before the front's settings
        else if (!std::strcmp(a, "--spatial")) {
            if (std::strcmp(v, "0") && std::strcmp(v, "1")) {
                log("app: --spatial takes 0 or 1, not %s", v);
                return false;
            }
            o.spatial = v[0] == '1';
        }
        else o.shot = v;
    }
#ifdef __ANDROID__
    if (o.shot || o.headless) log("app: --shot / --headless are for the sim; ignored on the headset");
#else
    if (o.pick_test || o.foveation_cycle > 0 || o.cpu_level >= 0 || o.gpu_level >= 0)
        log("app: --pick-test / --foveation-cycle / --cpu-level / --gpu-level are for the headset; "
            "ignored in the sim");
#endif
    if (o.foveation < 0 || o.foveation > 3) o.foveation = 2;
    if (o.sharpen < 0 || o.sharpen > 2) o.sharpen = 0;
    if (o.eye_scale < 0.5f || o.eye_scale > 3.f) o.eye_scale = 1.3f;   // the menu offers x1.0-x3.0
    if (o.cpu_level > 3) o.cpu_level = 3;
    if (o.gpu_level > 3) o.gpu_level = 3;
    if (o.shot) o.headless = true;
    return true;
}

#ifdef __ANDROID__
// stdout and stderr into logcat (tag vrcore, "stdio: " before each line): a game plugin reports there
// (Android sends both to /dev/null).
void *stdio_reader(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        size_t start = 0;
        for (size_t k = 0; k < n; ++k)
            if (buf[k] == '\n') {
                buf[k] = 0;
                __android_log_print(ANDROID_LOG_INFO, "vrcore", "stdio: %s", buf + start);
                start = k + 1;
            }
        if (start == 0 && n == sizeof buf - 1) {        // a line longer than the buffer: out in pieces
            buf[n] = 0;
            __android_log_print(ANDROID_LOG_INFO, "vrcore", "stdio: %s", buf);
            n = 0;
        } else {
            std::memmove(buf, buf + start, n - start);
            n -= start;
        }
    }
    return nullptr;
}

void stdio_to_logcat()
{
    int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    pthread_t t;
    if (pthread_create(&t, nullptr, stdio_reader, (void *)(intptr_t)fds[0]) == 0) pthread_detach(t);
}
#endif

}

void request_quit(int code)
{
    // The first request's code, stored before the flag is seen (a reader on another thread gets the code too).
    int none = kNoQuit;
    if (g_quit_code.compare_exchange_strong(none, code)) g_quit = true;
}

void request_restart()
{
    g_restart = true;
}

bool take_restart()
{
    return g_restart.exchange(false);
}


const Headset &headset()
{
    return g_headset;
}

void set_headset(const AppInfo &info, const Headset &h, Options &o)
{
    g_headset = h;
    std::string rates;
    for (float r : h.rates) rates += " " + std::to_string((int)(r + 0.5f));
    log("app: headset %s, gpu %s, %s %s; rates%s; foveation %d, sharpening %d", h.system.c_str(), h.gpu.c_str(),
        h.make.empty() ? "-" : h.make.c_str(), h.model.empty() ? "-" : h.model.c_str(),
        rates.empty() ? " none listed" : rates.c_str(), h.foveation, h.sharpen);
    if (!info.headset) return;
    Options flags = o;
    info.headset(h, o);
    if (g_flagged & kFlagHz) o.hz = flags.hz;
    if (g_flagged & kFlagEye) o.eye_scale = flags.eye_scale;
    if (g_flagged & kFlagFoveation) o.foveation = flags.foveation;
    if (g_flagged & kFlagSharpen) o.sharpen = flags.sharpen;
    if (o.foveation < 0 || o.foveation > 3) o.foveation = 2;
    if (o.sharpen < 0 || o.sharpen > 2) o.sharpen = 0;
    if (o.eye_scale < 0.5f || o.eye_scale > 3.f) o.eye_scale = 1.3f;
    log("app: graphics for this run: hz %.0f, eye x%.2f, foveation %d, sharpen %d", o.hz, o.eye_scale, o.foveation,
        o.sharpen);
}

void request_graphics(const Graphics &g)
{
    g_graphics_ask = g;
    g_graphics_asked = true;
}

bool take_graphics(Graphics &g)
{
    if (!g_graphics_asked) return false;
    g_graphics_asked = false;
    g = g_graphics_ask;
    return true;
}

bool quit_requested(int *code)
{
    bool q = g_quit.load();
    if (q && code) *code = g_quit_code.load();
    return q;
}

int run(const AppInfo &info, int argc, char **argv)
{
#ifdef __ANDROID__
    stdio_to_logcat();
#endif
    // Android may call SDL_main again in the same process: a quit from the run before must not carry over.
    g_quit = false;
    g_quit_code = kNoQuit;
    g_restart = false;
    g_flagged = 0;
    g_graphics_asked = false;
    g_headset = Headset();
    Options o;
    // --settings FILE (the tests' own file) is needed before the front opens its settings
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--settings")) settings::set_path(argv[i + 1]);
    if (info.load_settings) info.load_settings(o);          // the saved choices; the flags below override them
    if (!parse(info, o, argc, argv)) return 1;
#ifndef __ANDROID__
    if (o.rig_test) return rig::rig_test(o.rig_test);       // device-free: no window, no plugin
    if (o.audio_test) return audio::audio_test(o.audio_test);
    if (o.settings_test) return settings::settings_test(o.settings_test);
#endif
    audio::set_spatial(o.spatial);
    log("%s: hz %.0f, eye x%.2f, foveation %d, sharpen %d, spatial sound %s%s%s%s", info.name, o.hz,
        o.eye_scale, o.foveation, o.sharpen, o.spatial ? "on" : "off",
        o.foveation_cycle > 0 ? " (cycling)" : "", o.fdm_view ? ", density view" : "",
        o.pick_test ? ", pick test" : "");

#ifdef __ANDROID__
    // The XR session's state drives the loop. SDL would otherwise block SDL_main inside its event pump
    // while the activity is paused, and the session's STOPPING would never be seen.
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log("%s: SDL_Init failed: %s", info.name, SDL_GetError());
        return 1;
    }
    int rc = xr::run(info, o);
#else
    // Headless: the events alone (Ctrl-C quits).
    if (!SDL_Init(o.headless ? SDL_INIT_EVENTS : SDL_INIT_VIDEO)) {
        log("%s: SDL_Init failed: %s", info.name, SDL_GetError());
        return 1;
    }
    int rc = sim::run(info, o);
#endif
    audio::close();             // the front closes it before its game stops; this only if it did not
    SDL_Quit();
    log("%s: done (%d)", info.name, rc);
    return rc;
}

}
