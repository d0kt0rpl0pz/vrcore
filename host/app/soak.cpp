/*
 * soak.cpp -- THE SOAK TEST (--soak [seed]; a game's tools run it on the headset): a player nobody wears the headset
 * for, that plays the game again and again, driven by the game's state (Hooks::state), not by frame numbers, so it
 * never ends. The game's front plays (Hooks::drive) and picks (Hooks::enter) with the seeded rng, so a seed repeats
 * its picks.
 *
 * Logged ("<tag> soak ..."): the seed, every 60 s the totals (the front's, then the game frame, the state, two of the
 * counters, the front's extra) and the native heap, and "stuck" when the state has not changed for the front's limit.
 * --soak-crash MINUTES: a native crash after that long, to test a tool's capture of one. A soak keeps its saves in
 * a folder of its own (nvram_name).
 */
#include "soak.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __ANDROID__
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif

#include "log.h"

namespace vrcore {
namespace soak {

namespace {

struct Soak {
    bool on = false;
    long seed_arg = -1;                 // --soak's seed (-1: from the clock)
    float crash_s = 0;                  // --soak-crash, seconds (0 off)
    std::mt19937 rng;
    double started = 0, mode_at = 0, log_at = 0;
    int mode = -1;
    bool stuck_said = false;
} s;

double now_s()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The native heap in use (MB): bionic's mallinfo on the headset, the default malloc zone on the Mac.
long heap_mb()
{
#ifdef __ANDROID__
    struct mallinfo mi = mallinfo();
    return (long)(((size_t)mi.uordblks + (size_t)mi.hblkhd) >> 20);
#elif defined(__APPLE__)
    malloc_statistics_t st = {};
    malloc_zone_statistics(nullptr, &st);
    return (long)(st.size_in_use >> 20);
#else
    return -1;
#endif
}

bool is_number(const char *v)
{
    if (!v || !*v) return false;
    for (const char *p = v; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    return true;
}

}

int option(const Hooks &h, int argc, char **argv, int i)
{
    const char *o = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
    if (!std::strcmp(o, "--soak")) {
        s.on = true;
        if (is_number(v)) {
            s.seed_arg = std::atol(v);
            return 2;
        }
        return 1;
    }
    if (!std::strcmp(o, "--soak-crash")) {
        char *end = nullptr;
        float m = v ? std::strtof(v, &end) : 0.f;
        if (!v || end == v || *end || m <= 0.f) {
            log("%s --soak-crash takes the minutes (above 0)%s%s", h.tag, v ? ", not " : "", v ? v : "");
            return -1;
        }
        s.on = true;
        s.crash_s = m * 60.f;
        return 2;
    }
    return 0;
}

bool on() { return s.on; }

const char *nvram_name(const char *normal) { return s.on ? "soak-nvram" : normal; }

void start(const Hooks &h)
{
    long seed = s.seed_arg >= 0 ? s.seed_arg : (long)(std::chrono::steady_clock::now().time_since_epoch().count() & 0x7fffffff);
    // the options stay; the run's state starts again (a restart in the sim runs this twice)
    Soak fresh;
    fresh.on = s.on;
    fresh.seed_arg = s.seed_arg;
    fresh.crash_s = s.crash_s;
    s = fresh;
    s.rng.seed((unsigned)seed);
    s.started = now_s();
    char crash[48] = "";
    if (s.crash_s > 0) std::snprintf(crash, sizeof crash, ", crash at %.0f s", s.crash_s);
    log("%s soak: seed %ld%s", h.tag, seed, crash);
    if (!h.state || !h.stuck_s) log("%s soak: the front gives no state or stuck_s hook: nobody plays", h.tag);
}

void drive(const Hooks &h, Plugin &p, float dt, Controls &c)
{
    if (!h.state || !h.stuck_s) return;     // both required (start logged it)
    double now = now_s(), t = now - s.started;
    uint32_t f = 0, ended = 0, resyncs = 0, late = 0;
    p.stats(&f, &ended, &resyncs, &late);
    int m = h.state(p);
    if (m != s.mode) {
        if (h.enter) h.enter(p, s.mode, m, f, now, s.rng);
        s.mode = m;
        s.mode_at = now;
        s.stuck_said = false;
    }
    if (!s.stuck_said && now - s.mode_at > h.stuck_s(m)) {
        s.stuck_said = true;
        log("%s soak stuck mode %d for %.0f s, frame %u", h.tag, m, now - s.mode_at, f);
    }
    if (now - s.log_at >= 60.0) {
        s.log_at = now;
        std::string totals = h.totals ? h.totals() + ", " : "";
        std::string extra = h.extra ? " " + h.extra(p) : "";
        log("%s soak %.0f s: %sframe %u, mode %d, %s %u %s %u%s, heap %ld MB", h.tag, t, totals.c_str(), f, m,
            h.counters[2], late, h.counters[1], resyncs, extra.c_str(), heap_mb());
    }
    if (s.crash_s > 0 && t > s.crash_s) {
        log("%s soak: the crash test (--soak-crash)", h.tag);
        volatile int *nowhere = nullptr;
        *nowhere = 1;                                       // a native access violation: the capture's test
    }

    if (h.drive) h.drive(p, s.rng, m, now, now - s.mode_at, dt, c);
}

}
}
