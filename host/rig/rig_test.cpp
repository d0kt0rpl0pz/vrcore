/*
 * rig_test.cpp -- the rig's device-free checks, run by `--rig-test <dir>` before any host starts.
 *
 * THE WHEEL's are at a wheel's full lock of 27306. The stick's, the shifter's and the rumble's test the
 * maths (Stick::shape / damp / to_game / steer, Shifter::lever / corner / read over a made-up snapshot, Rumble through
 * its output seam, so no controller is touched). One line a check in <dir>/rig_test.txt, then a summary in the log.
 */
#include "rig_test.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <SDL3/SDL_scancode.h>

#include "../app/input.h"
#include "../app/log.h"
#include "rig.h"
#include "rumble.h"
#include "shifter.h"
#include "stick.h"
#include "wheel.h"

namespace vrcore {
namespace rig {

namespace {

constexpr int kFull = 27306;                // a game's steering where its curve goes flat (WheelFullLock)
const float kDeg2Rad = 0.0174532924f;

struct Report {
    std::FILE *f = nullptr;
    int pass = 0, fail = 0;

    void note(const char *fmt, ...)
    {
        if (!f) return;
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
    }

    void check(bool ok, const char *fmt, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        ++(ok ? pass : fail);
        if (f) std::fprintf(f, "%s %s\n", ok ? "PASS" : "FAIL", buf);
    }
};

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// ---------------------------------------------------------------------------------------------------------
// The wheel: hands 40 cm apart, rolled by theta (+ = right hand up), in front of a seat facing +Z (left-handed).

Vec3 hand(float deg, float r = 0.2f)
{
    return Vec3(std::cos(deg * kDeg2Rad), std::sin(deg * kDeg2Rad), 0.f) * r;
}

int at(Wheel::State &s, float deg, int lk, int dz, float r = 0.2f, float push = 0.f)
{
    return Wheel::step(s, -hand(deg, r) + Vec3(0, 1.2f, 0.4f), hand(deg, r) + Vec3(0, 1.2f, 0.4f + push), true,
                       Vec3::forward(), 1.f / 72.f, lk, dz, 0, kFull);
}

void wheel_checks(Report &r)
{
    const int full = kFull;
    // a sweep -180..+180 at lock 60, no deadzone: monotonic, 0 at level, half at 30, full past 60
    Wheel::State s;
    int last = 0x7fffffff;
    bool mono = true;
    for (float t = -179.f; t <= 179.f; t += 1.f) {
        int v = at(s, t, 60, 0);
        if (v > last) mono = false;
        last = v;
        r.note("  theta %6.1f  steer %7d", t, v);
    }
    r.check(mono, "wheel sweep: steer never rises as the right hand rises");
    s = Wheel::State(); r.check(at(s, 0, 60, 0) == 0, "wheel level: 0");
    s = Wheel::State(); r.check(std::abs(at(s, 30, 60, 0) + full / 2) <= 1, "wheel 30 of 60 degrees, right hand up: half lock left");
    s = Wheel::State(); r.check(at(s, -60, 60, 0) == full, "wheel -60 of 60: full lock right");
    s = Wheel::State(); r.check(at(s, -75, 60, 0) == full, "wheel past the lock: full lock");
    // deadzone 5 at lock 60: nothing up to 5, then rescaled so 60 is still full
    s = Wheel::State(); r.check(at(s, 4.5f, 60, 5) == 0, "wheel deadzone 5: 4.5 degrees gives 0");
    s = Wheel::State(); r.check(std::abs(at(s, 32.5f, 60, 5) + full / 2) <= 1, "wheel deadzone 5: 32.5 degrees gives half");
    s = Wheel::State(); r.check(at(s, 60, 60, 5) == -full, "wheel deadzone 5: 60 degrees still full");
    // a panic twist through 180 and on to 250, and back: full left all the way, no flip
    s = Wheel::State();
    bool held = true;
    for (float t = 0; t <= 250.f; t += 5.f) { int v = at(s, t, 60, 0); if (t >= 60.f && v != -full) held = false; }
    for (float t = 250.f; t >= 60.f; t -= 5.f) { int v = at(s, t, 60, 0); if (v != -full) held = false; }
    r.check(held, "wheel twist to 250 degrees and back: full left throughout, no jump to the right");
    // crossover hysteresis at lock 90: 88 degrees fresh is below full; after crossing it stays full until the
    // hands are 2 cm back past each other (40 cm apart: at ~87.1)
    s = Wheel::State();
    int fresh = at(s, 88, 90, 0);
    r.check(fresh > -full && fresh < 0, "wheel lock 90: 88 degrees fresh is %d, below full", fresh);
    s = Wheel::State(); at(s, 80, 90, 0); at(s, 95, 90, 0);
    r.check(at(s, 88, 90, 0) == -full, "wheel lock 90: crossed, then back to 88: still full (latched)");
    r.check(at(s, 85, 90, 0) > -full, "wheel lock 90: back to 85 (3.5 cm uncrossed): released");
    // lock 135: the arms cross as travel, no latch; 90 is two thirds, 135 full
    s = Wheel::State();
    int c90 = 0, c120 = 0, c135 = 0;
    for (float t = 0; t <= 135.f; t += 5.f) {
        int v = at(s, t, 135, 0);
        if (t == 90.f) c90 = v;
        if (t == 120.f) c120 = v;
        if (t == 135.f) c135 = v;
    }
    r.check(std::abs(c90 + full * 2 / 3) <= 1, "wheel lock 135: 90 degrees gives %d, two thirds left", c90);
    r.check(c120 > -full && c120 < c90, "wheel lock 135: 120 degrees (crossed) gives %d, below full", c120);
    r.check(c135 == -full, "wheel lock 135: 135 degrees full");
    r.check(at(s, 100, 135, 0) > -full, "wheel lock 135: back to 100 (still crossed): released, no latch");
    // hands together hold; pushing one hand forward does not steer
    s = Wheel::State();
    int before = at(s, 20, 60, 0);
    r.check(at(s, -40, 60, 0, 0.03f) == before && at(s, 50, 60, 0, 0.03f) == before,
            "wheel hands 6 cm apart: the last value held");
    s = Wheel::State();
    before = at(s, 20, 60, 0);
    r.check(at(s, 20, 60, 0, 0.2f, 0.3f) == before, "wheel right hand 30 cm further forward: same steer");
    // tracking lost: held 0.1 s, centre by 0.3 s, no snap
    s = Wheel::State();
    at(s, -60, 60, 0);
    int prev = full, at005 = 0, at03 = -1;
    bool smooth = true;
    for (int i = 1; i <= 30; i++) {
        int v = Wheel::step(s, Vec3(), Vec3(), false, Vec3::forward(), 0.01f, 60, 0, 0, full);
        if (prev - v > full / 15) smooth = false;
        if (i == 5) at005 = v;
        if (i == 30) at03 = v;
        prev = v;
    }
    r.check(at005 == full && at03 == 0 && smooth, "wheel tracking lost: %d at 0.05 s, %d at 0.3 s, eased", at005, at03);

    // pitch 15: hands turned round the leaning wheel read as the same angles as upright; turned in the upright
    // plane instead, 30 degrees reads a little less (not more)
    bool tilted = true;
    const float angles[] = {-80.f, -45.f, -10.f, 0.f, 20.f, 60.f};
    for (float t : angles) {
        Vec3 rt, upw, axis;
        Wheel::plane(Vec3::forward(), 15.f, rt, upw, axis);
        Vec3 h = (rt * std::cos(t * kDeg2Rad) + upw * std::sin(t * kDeg2Rad)) * 0.2f;
        Wheel::State a, b;
        int tv = Wheel::step(a, -h + Vec3::up(), h + Vec3::up(), true, Vec3::forward(), 1.f / 72.f, 90, 0, 15, full);
        if (std::abs(tv - at(b, t, 90, 0)) > 1) tilted = false;
    }
    r.check(tilted, "wheel pitch 15: turning in the leaning plane reads the same as upright at pitch 0");
    {
        Wheel::State a, b;
        int upright = Wheel::step(a, -hand(30) + Vec3::up(), hand(30) + Vec3::up(), true, Vec3::forward(), 1.f / 72.f,
                                  90, 0, 15, full);
        int plain = at(b, 30, 90, 0);
        r.check(upright < 0 && upright >= plain, "wheel pitch 15, hands turned in the upright plane: %d vs %d (a little less)",
                upright, plain);
    }
    // the plane itself: pitch 0 faces +z with right +x; pitch 25 tips up/axis by 25 degrees; pitch clamps to 45
    {
        Vec3 rt, upw, axis;
        Wheel::plane(Vec3::forward(), 0.f, rt, upw, axis);
        r.check(near(rt.x, 1.f) && near(upw.y, 1.f) && near(axis.z, 1.f), "wheel plane at pitch 0: right +x, up +y, axis +z");
        Wheel::plane(Vec3(0, 5, 2), 25.f, rt, upw, axis);   // the seat's forward is taken flat
        r.check(near(upw.y, std::cos(25 * kDeg2Rad)) && near(upw.z, std::sin(25 * kDeg2Rad)) && near(axis.z, std::cos(25 * kDeg2Rad)) &&
                    near(axis.y, -std::sin(25 * kDeg2Rad)) && near(rt.x, 1.f),
                "wheel plane at pitch 25, forward flattened: up leans forward, axis tips down");
        Vec3 r2, u2, a2;
        Wheel::plane(Vec3::forward(), 90.f, r2, u2, a2);
        Wheel::plane(Vec3::forward(), 45.f, rt, upw, axis);
        r.check(near(u2.y, upw.y) && near(a2.z, axis.z), "wheel plane: pitch clamps to 45");
    }
    // the fly step (Rig::fly): flying on, the world ready, one second of keys from a head at the seat facing -z
    {
        Rig rig(2.f);
        Input in;
        in.dt = 1.f;
        in.head_tracked = true;
        rig.fly(in, true);
        r.check(rig.seat()[2] == 0.f, "fly: not flying, W does nothing");
        rig.set_flying(true);
        in.keys[SDL_SCANCODE_W] = true;
        rig.fly(in, true);
        r.check(near(rig.seat()[2], 8.f) && near(rig.seat()[0], 0.f), "fly: W 1 s at x1: the seat 8 m back (z %.3f)",
                rig.seat()[2]);
        in.keys[SDL_SCANCODE_W] = false;
        in.keys[SDL_SCANCODE_E] = true;
        rig.fly(in, true);
        r.check(near(rig.seat()[1], -8.f), "fly: E 1 s: the seat 8 m down (y %.3f)", rig.seat()[1]);
        in.keys[SDL_SCANCODE_E] = false;
        rig.fly(in, false);
        r.check(near(rig.seat()[1], -8.f), "fly: not ready: nothing moves");
        in.keys[SDL_SCANCODE_RIGHT] = true;
        in.dt = 0.5f;
        rig.fly(in, true);
        r.check(near(rig.yaw(), 0.25f * 3.14159265f), "fly: right arrow 0.5 s: turned 45 degrees right (yaw %.3f)",
                rig.yaw());
        r.check(near(rig.seat()[0], 8.f * std::sin(0.25f * 3.14159265f)) &&
                    near(rig.seat()[2], 8.f * std::cos(0.25f * 3.14159265f)),
                "fly: the turn is about the head: the seat's offset turned with it (%.3f, %.3f)", rig.seat()[0],
                rig.seat()[2]);
        in.keys[SDL_SCANCODE_RIGHT] = false;
        in.keys[SDL_SCANCODE_LSHIFT] = true;
        in.keys[SDL_SCANCODE_W] = true;
        in.dt = 0.f;
        rig.fly(in, true);                          // the boost starts (5x), no time passed
        float z0 = rig.seat()[2];
        in.dt = 1.f;
        rig.fly(in, true);                          // held 1 s: 10x
        r.check(near(rig.seat()[2] - z0, 80.f, 1e-3f), "fly: Shift held 1 s: 10x (%.3f m)", rig.seat()[2] - z0);
        rig.set_flying(false);
        r.check(rig.seat()[0] == 0.f && rig.seat()[1] == 0.f && rig.seat()[2] == 0.f && rig.yaw() == 0.f,
                "fly: off: the seat back home");
        rig.look_behind(in, true);
        r.check(!rig.looking_back(), "look behind: not held");
        in.hands[Input::kRight].stick_click = true;
        rig.look_behind(in, true);
        r.check(rig.looking_back() && near(rig.world_yaw(), 3.14159265f), "look behind: the right stick's click");
        rig.look_behind(in, false);
        r.check(!rig.looking_back(), "look behind: not when the game says no");
    }
}

// ---------------------------------------------------------------------------------------------------------
// The stick

void stick_checks(Report &r)
{
    // the deadzone: at or under it 0, past it rescaled so the output still starts from 0 and reaches 1
    r.check(Stick::shape(0.10f, 0.1f, 1.f, 1.f) == 0.f && Stick::shape(0.05f, 0.1f, 1.f, 1.f) == 0.f,
            "stick deadzone 10%%: at or under it is 0");
    r.check(near(Stick::shape(0.55f, 0.1f, 1.f, 1.f), 0.5f) && near(Stick::shape(-0.55f, 0.1f, 1.f, 1.f), -0.5f),
            "stick deadzone 10%%: 0.55 rescales to 0.5, the sign kept");
    r.check(near(Stick::shape(1.f, 0.1f, 1.f, 1.f), 1.f) && near(Stick::shape(2.f, 0.1f, 1.f, 1.f), 1.f),
            "stick deadzone 10%%: full push is 1, past it still 1");
    // the response: |v|^gamma, full lock still reached
    float strong = Stick::shape(0.55f, 0.1f, 2.2f, 1.f);
    r.check(near(strong, std::pow(0.5f, 2.2f)) && near(strong, 0.2176f, 5e-4f), "stick Strong (2.2): 0.5 gives %.4f", strong);
    r.check(near(Stick::shape(1.f, 0.1f, 3.f, 1.f), 1.f), "stick Very Strong: full push still 1");
    r.check(near(Stick::gamma(0), 1.f) && near(Stick::gamma(1), 1.3f) && near(Stick::gamma(2), 1.7f) &&
                near(Stick::gamma(3), 2.2f) && near(Stick::gamma(4), 3.f),
            "stick gammas: Linear 1.0, Slight 1.3, Medium 1.7, Strong 2.2, Very Strong 3.0");
    // the range: the full push gives that share
    r.check(near(Stick::shape(1.f, 0.f, 1.f, 0.8f), 0.8f) && near(Stick::shape(0.5f, 0.f, 1.f, 0.8f), 0.4f),
            "stick range 80%%: full push 0.8, half push 0.4");
    {   // the defaults: Medium, 2%, 80%
        Stick k;
        r.check(k.response == 2 && k.deadzone == 2 && k.range == 80 && k.turn == 150 && k.snapback == 150 &&
                    near(Stick::shape(1.f, k.deadzone / 100.f, Stick::gamma(k.response), k.range / 100.f), 0.8f),
                "stick defaults: Medium 2%% 80%% 150/150 ms, full push gives 0.8");
    }
    // damping through centre at the default 150 / 150 ms: 6.667 lock per second each way
    {
        Stick k;
        for (int i = 0; i < 6; ++i) k.steer(1.f, 0.01f);
        r.check(near(k.value(), 0.4f), "stick damping: 6 steps of 10 ms out reach 0.4 (%.4f)", k.value());
        for (int i = 0; i < 7; ++i) k.steer(1.f, 0.01f);
        r.check(k.value() == 0.8f, "stick damping: 0.12 s to the range's 0.8 (%.4f), and no further", k.value());
        k.steer(-1.f, 0.06f);       // back at the snapback rate: 0.8 -> 0.4
        r.check(near(k.value(), 0.4f), "stick damping: swing to full left, 60 ms back to %.4f", k.value());
        k.steer(-1.f, 0.09f);       // 60 ms more to centre, the other 30 ms out at the turn rate: -0.2
        r.check(near(k.value(), -0.2f), "stick damping: through centre, 30 ms out the other side to %.4f", k.value());
        k.reset();
        r.check(k.value() == 0.f, "stick reset: back to centre at once");
        k.steer(0.f, 0.01f);
        r.check(k.value() == 0.f, "stick at rest after a reset stays 0");
    }
    {   // the hold-out: back to rest at the snapback rate
        Stick k;
        for (int i = 0; i < 20; ++i) k.steer(1.f, 0.01f);
        for (int i = 0; i < 6; ++i) k.steer(0.f, 0.01f);
        r.check(near(k.value(), 0.4f), "stick snapback: 60 ms of rest from 0.8 gives %.4f", k.value());
        for (int i = 0; i < 10; ++i) k.steer(0.f, 0.01f);
        r.check(k.value() == 0.f, "stick snapback: back to 0 and stays");
    }
    {   // instant: no limit either way
        Stick k;
        k.turn = 0; k.snapback = 0;
        k.steer(1.f, 0.001f);
        float up = k.value();
        k.steer(-1.f, 0.001f);
        r.check(up == 0.8f && k.value() == -0.8f, "stick damping Instant: %.2f then %.2f at once", up, k.value());
    }
    {   // the selects: the whole travel to the ADC's end whatever the Range, the settings left as they were
        Stick k;
        k.turn = 0; k.snapback = 0;
        k.range = 50; k.fullLock = kFull; k.gameDead = 1877;
        int sel = k.steer(1.f, 0.01f, 100, 32767);
        int race = k.steer(1.f, 0.01f);
        r.check(sel == 32767 && k.range == 50 && k.fullLock == kFull && race == Stick::to_game(0.5f, 1877, kFull),
                "stick selects: range 50%% still reaches 32767 (%d), the race's next frame %d", sel, race);
    }
    // ToGame: a game's dead band 1877 and flat at 27306
    r.check(Stick::to_game(0.f, 1877, kFull) == 0, "stick to_game: 0 stays 0");
    r.check(Stick::to_game(1e-9f, 1877, kFull) == 1878 && Stick::to_game(-1e-9f, 1877, kFull) == -1878,
            "stick to_game: anything past 0 starts at dead + 1 (1878)");
    r.check(Stick::to_game(1.f, 1877, kFull) == kFull && Stick::to_game(-1.f, 1877, kFull) == -kFull &&
                Stick::to_game(1.5f, 1877, kFull) == kFull,
            "stick to_game: 1 is full lock 27306, past it clamped");
    r.check(Stick::to_game(0.5f, 1877, kFull) == 14592 && Stick::to_game(-0.5f, 1877, kFull) == -14592,
            "stick to_game: half is 14592");
    r.check(Stick::to_game(1.f, 0, 32767) == 32767 && Stick::to_game(1e-9f, 0, 32767) == 1,
            "stick to_game: no dead band, 32767: 1 .. 32767");
    {   // the whole chain with the game's curve
        Stick k;
        k.gameDead = 1877; k.fullLock = kFull;
        int out = 0;
        for (int i = 0; i < 20; ++i) out = k.steer(1.f, 0.01f);
        r.check(out == 22220, "stick steer, defaults on Daytona's curve: full push gives %d (22220)", out);
        k.range = 100;
        for (int i = 0; i < 20; ++i) out = k.steer(1.f, 0.01f);
        r.check(out == kFull, "stick steer, range 100: full push gives %d (27306)", out);
        for (int i = 0; i < 40; ++i) out = k.steer(0.f, 0.01f);
        r.check(out == 0, "stick steer: at rest, back to 0");
        k.reset();
        for (int i = 0; i < 30; ++i) out = k.steer(0.02f, 0.01f);
        r.check(out == 0, "stick steer: the default 2%% deadzone, 0.02 gives 0");
        for (int i = 0; i < 30; ++i) out = k.steer(0.03f, 0.01f);
        r.check(out > 1878 && out < 1900, "stick steer: 0.03 is just past the game's dead band (%d)", out);
        for (int i = 0; i < 30; ++i) out = k.steer(-1.f, 0.01f);
        r.check(out == -kFull, "stick steer: full left gives %d", out);
    }
    {   // the menu's steps
        Stick k;
        k.step_response(-3);
        bool a = k.response == 4;       // 2 - 3 wraps to 4
        k.step_response(1);
        a = a && k.response == 0;
        r.check(a, "stick step_response wraps (%s)", k.response_name());
        k.step_deadzone(1);
        bool b = k.deadzone == 5;
        k.deadzone = 0; k.step_deadzone(-1);
        b = b && k.deadzone == 20;
        k.range = 100; k.step_range(-1);
        b = b && k.range == 60;
        k.turn = 150; k.step_turn(1);
        b = b && k.turn == 200;
        k.snapback = 0; k.step_snapback(-1);
        b = b && k.snapback == 300;
        k.deadzone = 3; k.step_deadzone(1);      // not in the list: counts as the first
        b = b && k.deadzone == 2;
        r.check(b, "stick steps through the lists wrap, a stray value counts as the first");
        Stick d;
        bool names = !std::strcmp(d.deadzone_name().c_str(), "2%") && !std::strcmp(d.range_name().c_str(), "80%") &&
                     !std::strcmp(d.turn_name().c_str(), "150 ms") && !std::strcmp(d.response_name(), "Medium");
        d.deadzone = 0; d.range = 100; d.turn = 0;
        names = names && !std::strcmp(d.deadzone_name().c_str(), "Off") &&
                !std::strcmp(d.range_name().c_str(), "100% (full lock)") && !std::strcmp(d.turn_name().c_str(), "Instant");
        r.check(names, "stick row names");
    }
}

// ---------------------------------------------------------------------------------------------------------
// The shifter

void corner_vec(float len, float deg, bool left, bool up, float &x, float &y)
{
    x = len * std::cos(deg * kDeg2Rad) * (left ? -1.f : 1.f);
    y = len * std::sin(deg * kDeg2Rad) * (up ? 1.f : -1.f);
}

void shifter_checks(Report &r)
{
    using S = Shifter;
    // Sequential: in past 0.6 with the vertical axis stronger, out under 0.4
    r.check(S::lever(0, 0.59f, 0) == 0 && S::lever(0, 0.6f, 0) == 1 && S::lever(0, -0.6f, 0) == -1 && S::lever(0, -0.59f, 0) == 0,
            "shifter lever: in at 0.6 up or down, not under");
    r.check(S::lever(0.5f, 0.6f, 0) == 1 && S::lever(0.7f, 0.6f, 0) == 0 && S::lever(0.6f, 0.6f, 0) == 0,
            "shifter lever: up needs the vertical axis stronger");
    r.check(S::lever(0, 0.41f, 1) == 1 && S::lever(0, 0.4f, 1) == 0 && S::lever(0, -0.41f, -1) == -1 && S::lever(0, -0.4f, -1) == 0,
            "shifter lever hysteresis: held down to 0.4, out at 0.4");
    r.check(S::lever(0, 0.41f, 0) == 0 && S::lever(0, 0.5f, 0) == 0, "shifter lever: between 0.4 and 0.6 does not engage");
    r.check(S::lever(0.9f, 0.5f, 1) == 1, "shifter lever: held, the horizontal axis does not let it go");
    r.check(S::lever(0, -0.9f, 1) == -1 && S::lever(0, -0.5f, 1) == 0, "shifter lever: up to down needs the way in again");

    // HGate4: the corners
    r.check(S::corner(-0.6f, 0.6f, 0) == 1 && S::corner(-0.6f, -0.6f, 0) == 2 && S::corner(0.6f, 0.6f, 0) == 3 &&
                S::corner(0.6f, -0.6f, 0) == 4,
            "shifter corner: 1 up-left, 2 down-left, 3 up-right, 4 down-right");
    r.check(S::corner(0, 1, 0) == 0 && S::corner(0, -1, 0) == 0 && S::corner(1, 0, 0) == 0 && S::corner(-1, 0, 0) == 0 &&
                S::corner(0, 0, 0) == 0,
            "shifter corner: the edges and the centre are no gear");
    float x, y;
    corner_vec(0.79f, 45.f, true, true, x, y);
    bool lenIn = S::corner(x, y, 0) == 0;
    corner_vec(0.81f, 45.f, true, true, x, y);
    lenIn = lenIn && S::corner(x, y, 0) == 1;
    r.check(lenIn, "shifter corner: in at a length of 0.8 (0.79 no, 0.81 yes)");
    corner_vec(0.51f, 45.f, false, false, x, y);
    bool lenOut = S::corner(x, y, 4) == 4 && S::corner(x, y, 0) == 0;
    corner_vec(0.49f, 45.f, false, false, x, y);
    lenOut = lenOut && S::corner(x, y, 4) == 0;
    r.check(lenOut, "shifter corner hysteresis: held down to a length of 0.5, not engaged there from outside");
    corner_vec(1.f, 69.f, true, true, x, y);        // 24 degrees off the diagonal
    bool angIn = S::corner(x, y, 0) == 1;
    corner_vec(1.f, 71.f, true, true, x, y);        // 26
    angIn = angIn && S::corner(x, y, 0) == 0;
    corner_vec(1.f, 21.f, true, true, x, y);        // 24 the other way
    angIn = angIn && S::corner(x, y, 0) == 1;
    corner_vec(1.f, 19.f, true, true, x, y);        // 26
    angIn = angIn && S::corner(x, y, 0) == 0;
    r.check(angIn, "shifter corner: in within 25 degrees of the diagonal (24 yes, 26 no, both ways)");
    corner_vec(1.f, 79.f, true, true, x, y);        // 34 degrees off
    bool angOut = S::corner(x, y, 1) == 1;
    corner_vec(1.f, 81.f, true, true, x, y);        // 36
    angOut = angOut && S::corner(x, y, 1) == 0;
    corner_vec(1.f, 11.f, true, true, x, y);        // 34 the other way
    angOut = angOut && S::corner(x, y, 1) == 1;
    corner_vec(1.f, 9.f, true, true, x, y);         // 36
    angOut = angOut && S::corner(x, y, 1) == 0;
    r.check(angOut, "shifter corner hysteresis: held out to 35 degrees (34 yes, 36 no, both ways)");
    corner_vec(1.f, 79.f, true, true, x, y);
    r.check(S::corner(x, y, 0) == 0 && S::corner(x, y, 3) == 0, "shifter corner: 34 degrees off is not an entry, nor held by another gear");

    // read: over a made-up snapshot
    static Input in;
    in = Input();
    Shifter h(Shifter::Mode::HGate4);
    in.hands[Input::kRight].active = true;
    in.hands[Input::kRight].stick[0] = -0.7f;
    in.hands[Input::kRight].stick[1] = 0.7f;
    r.check(h.read(in, true) == 0 && !h.engaged, "shifter read: off, the stick does nothing");
    h.on = true;
    r.check(h.read(in, false) == 0 && !h.engaged, "shifter read: not driving, nothing");
    int g = h.read(in, true);
    r.check(g == 1 && h.gate == 1 && h.engaged && h.from == 0, "shifter read: right hand into gear 1, engaged, from the Touch controller (0)");
    h.read(in, true);
    r.check(h.gate == 1 && !h.engaged, "shifter read: held, no new tick");
    in.hands[Input::kRight].active = false;
    h.read(in, true);
    r.check(h.gate == 0 && !h.engaged, "shifter read: the hand inactive reads as rest");
    in.hands[Input::kRight].active = true;
    in.pad_count = 2;
    in.pads[0].id = 4;
    in.pads[1].id = 7;
    in.pads[0].right[0] = 0.5f;  in.pads[0].right[1] = 0.5f;        // weaker than the hand's 0.99
    in.pads[1].right[0] = 0.8f;  in.pads[1].right[1] = 0.8f;        // stronger: the pad wins
    g = h.read(in, true);
    r.check(g == 3 && h.engaged && h.from == 7, "shifter read: the strongest stick wins (pad 7 into gear 3, from 7)");
    in.pads[1].right[0] = 0.f;   in.pads[1].right[1] = 0.f;
    h.read(in, true);
    r.check(h.gate == 1 && h.engaged && h.from == 0, "shifter read: the pad let go, the hand's gear 1 engages, from 0");
    in.hands[Input::kRight].stick[0] = 0.f;
    in.hands[Input::kRight].stick[1] = 0.f;
    in.pads[0].right[0] = 0.f;   in.pads[0].right[1] = 0.f;
    h.read(in, true);
    r.check(h.gate == 0 && !h.engaged, "shifter read: all at rest, gate 0 (the game's gearbox latches the gear)");
    Shifter q(Shifter::Mode::Sequential);
    q.on = true;
    in.hands[Input::kRight].stick[1] = 0.7f;
    r.check(q.read(in, true) == 1 && q.engaged, "shifter read, sequential: up is +1, engaged");
    in.hands[Input::kRight].stick[1] = 0.45f;
    r.check(q.read(in, true) == 1 && !q.engaged, "shifter read, sequential: held to 0.4");
    in.hands[Input::kRight].stick[1] = -0.7f;
    r.check(q.read(in, true) == -1 && q.engaged, "shifter read, sequential: down is -1");
    r.check(!std::strcmp(q.name(), "Right stick") && !std::strcmp(h.legend_action(), "Gears 1-4 (H pattern)") &&
                !std::strcmp(q.legend_key(), "Right Stick, up / down") && Shifter::TickStrength == 0.6f && Shifter::TickSeconds == 0.04f,
            "shifter names and the tick (0.6 for 40 ms)");
}

// ---------------------------------------------------------------------------------------------------------
// The rumble, through its output seam

struct Op {
    enum Kind { Pad, Touch, StopTouch } kind;
    uint32_t id;            // the pad, or the hand
    float a, b;             // low / high, or amplitude / seconds
};
Op g_ops[64];
int g_nops = 0;
bool g_present[16];

void rec(Op::Kind k, uint32_t id, float a, float b)
{
    if (g_nops < 64) g_ops[g_nops++] = Op{k, id, a, b};
}
bool t_there(uint32_t id) { return id < 16 && g_present[id]; }
void t_pad(uint32_t id, float low, float high) { rec(Op::Pad, id, low, high); }
void t_touch(int hand, float amp, float s) { rec(Op::Touch, (uint32_t)hand, amp, s); }
void t_stop() { rec(Op::StopTouch, 0, 0, 0); }

int count(Op::Kind k)
{
    int n = 0;
    for (int i = 0; i < g_nops; ++i) n += g_ops[i].kind == k;
    return n;
}

// the last Touch impulse on a hand (-1: none)
float last_touch(int hand)
{
    for (int i = g_nops - 1; i >= 0; --i)
        if (g_ops[i].kind == Op::Touch && (int)g_ops[i].id == hand) return g_ops[i].a;
    return -1.f;
}

void rumble_checks(Report &r)
{
    Rumble::Out out = {t_there, t_pad, t_touch, t_stop};
    for (bool &p : g_present) p = false;
    const double base = 100.0;

    // the spring
    {
        Rumble m;
        r.check(m.spring_strength(1.f, 1.f) == 0.f, "rumble spring off: 0");
        m.spring = true;
        const float mx = 27.f / 63.f;
        r.check(m.spring_strength(0.f, 1.f) == 0.f, "rumble spring: straight ahead 0");
        r.check(near(m.spring_strength(1.f, 1.f), mx) && near(m.spring_strength(-1.f, 1.f), mx) && near(m.spring_strength(3.f, 1.f), mx),
                "rumble spring: full lock, full speed: %.4f (27/63), either way, clamped", m.spring_strength(1.f, 1.f));
        r.check(near(m.spring_strength(0.5f, 1.f), 0.75f * mx), "rumble spring: half lock gives 0.75 of it (%.4f)", m.spring_strength(0.5f, 1.f));
        r.check(near(m.spring_strength(1.f, 0.f), 0.25f * mx), "rumble spring: standstill keeps the 0.25 floor (%.4f)", m.spring_strength(1.f, 0.f));
        r.check(near(m.spring_strength(0.5f, 0.5f), 0.75f * (0.25f + 0.75f * 0.5f) * mx),
                "rumble spring: half lock, half speed (%.4f)", m.spring_strength(0.5f, 0.5f));
        r.check(near(m.spring_strength(1.f, -1.f), mx) && near(m.spring_strength(1.f, 2.f), mx),
                "rumble spring: negative speed is full, past 1 clamped");
    }
    // the side split on the Touch controllers
    {
        Rumble m;
        m.set_out(out);
        uint32_t used = 0;
        Rumble::Inputs in;
        in.strength = 0.8f;
        g_nops = 0;
        in.side = 0.f;
        m.update(in, base, used);
        bool a = near(last_touch(Input::kLeft), 0.8f) && near(last_touch(Input::kRight), 0.8f) && count(Op::Touch) == 2 &&
                 near(g_ops[0].b, 0.1f);
        in.side = -1.f; g_nops = 0; m.update(in, base, used);
        bool b = near(last_touch(Input::kLeft), 0.8f) && near(last_touch(Input::kRight), 0.f);
        in.side = 1.f; g_nops = 0; m.update(in, base, used);
        bool c = near(last_touch(Input::kLeft), 0.f) && near(last_touch(Input::kRight), 0.8f);
        in.side = 0.5f; g_nops = 0; m.update(in, base, used);
        bool d = near(last_touch(Input::kLeft), 0.4f) && near(last_touch(Input::kRight), 0.8f);
        in.side = 7.f; g_nops = 0; m.update(in, base, used);
        bool e = near(last_touch(Input::kLeft), 0.f) && near(last_touch(Input::kRight), 0.8f);
        r.check(a, "rumble side 0: both hands 0.8, 0.1 s impulses, renewed every frame");
        r.check(b && c, "rumble side -1: left alone; side 1: right alone");
        r.check(d && e, "rumble side 0.5: left 0.4, right 0.8; a side past 1 clamps");
        // renewed each frame while on, the same strength
        g_nops = 0;
        in.side = 0.f;
        m.update(in, base, used); m.update(in, base, used);
        r.check(count(Op::Touch) == 4 && count(Op::Pad) == 0, "rumble Touch: the impulse is sent every frame (2 frames, 4 calls)");
        m.stop();
        g_nops = 0;
        m.stop();
        r.check(g_nops == 0, "rumble stop with nothing running: no calls");
    }
    // layering: the spring and the strength, each hand the stronger
    {
        Rumble m;
        m.set_out(out);
        m.spring = true;
        uint32_t used = 0;
        Rumble::Inputs in;
        in.strength = 0.1f; in.side = -1.f; in.steer = 1.f; in.speed = 1.f;
        g_nops = 0;
        m.update(in, base, used);
        const float mx = 27.f / 63.f;
        r.check(near(last_touch(Input::kLeft), mx) && near(last_touch(Input::kRight), mx),
                "rumble layering: a weak strength under the spring: both hands feel the spring (%.4f)", last_touch(Input::kLeft));
        in.strength = 0.9f; in.side = 1.f; g_nops = 0;
        m.update(in, base, used);
        r.check(near(last_touch(Input::kLeft), mx) && near(last_touch(Input::kRight), 0.9f),
                "rumble layering: a contact on the right stands out above the spring (L %.4f, R %.4f)",
                last_touch(Input::kLeft), last_touch(Input::kRight));
        in.driving = false; g_nops = 0;
        m.update(in, base, used);
        bool zero = near(last_touch(Input::kLeft), 0.f) && near(last_touch(Input::kRight), 0.f);
        g_nops = 0;
        m.update(in, base, used);
        r.check(zero && g_nops == 0, "rumble not driving: one frame of zeros, then silence (the early return)");
    }
    // the pulse
    {
        Rumble m;
        m.set_out(out);
        uint32_t used = 0;
        Rumble::Inputs in;
        m.pulse(0.6f, 0.04f, 0.0, 1.f);          // the shifter's tick in the right hand
        double t0 = 0.0;
        g_nops = 0;
        m.update(in, t0 + 0.02, used);
        r.check(near(last_touch(Input::kLeft), 0.f) && near(last_touch(Input::kRight), 0.6f),
                "rumble pulse on the right: right 0.6, left 0 (L %.2f, R %.2f)", last_touch(Input::kLeft), last_touch(Input::kRight));
        g_nops = 0;
        m.update(in, t0 + 0.05, used);
        r.check(near(last_touch(Input::kLeft), 0.f) && near(last_touch(Input::kRight), 0.f) && count(Op::Touch) == 2,
                "rumble pulse: over after 40 ms (zeros once)");
        g_nops = 0;
        m.update(in, t0 + 0.06, used);
        r.check(g_nops == 0, "rumble pulse: then silent");
        m.pulse(0.6f, 0.04f, 1.f, 1.f);
        in.strength = 0.2f; in.side = -1.f; g_nops = 0;
        m.update(in, 1.01, used);
        r.check(near(last_touch(Input::kLeft), 0.2f) && near(last_touch(Input::kRight), 0.6f),
                "rumble pulse on the right over a left strength: L 0.2, R 0.6 (the stronger on each hand)");
        in.strength = 0.9f; in.side = 0.f; g_nops = 0;
        m.update(in, 1.01, used);
        r.check(near(last_touch(Input::kLeft), 0.9f) && near(last_touch(Input::kRight), 0.9f), "rumble pulse under a strong contact: it stands out (0.9)");
        m.pulse(2.f, 0.04f, 1.f, -3.f);                 // strength and side clamp; a new one replaces the last
        in.strength = 0.f; g_nops = 0;
        m.update(in, 1.02, used);
        r.check(near(last_touch(Input::kLeft), 1.f) && near(last_touch(Input::kRight), 0.f), "rumble pulse: strength and side clamp, a new one replaces the last");
        in.driving = false; g_nops = 0;
        m.pulse(0.6f, 0.04f, 2.f, 0.f);
        m.update(in, 2.01, used);
        bool quiet = near(last_touch(Input::kLeft), 0.f);     // one frame of zeros after the 1.0 from before
        g_nops = 0;
        m.update(in, 2.015, used);
        r.check(quiet && g_nops == 0, "rumble pulse: not felt while not driving");
    }
    // a pad: set when the strength changes; gone is dropped
    {
        Rumble m;
        m.set_out(out);
        g_present[5] = true;
        uint32_t used = 5;
        Rumble::Inputs in;
        in.strength = 0.5f;
        g_nops = 0;
        m.update(in, base, used);
        r.check(count(Op::Pad) == 1 && g_ops[0].id == 5 && near(g_ops[0].a, 0.5f) && g_ops[0].b == 0.f && count(Op::Touch) == 0,
                "rumble pad: low motor 0.5, the other 0, nothing to the Touch controllers");
        g_nops = 0;
        m.update(in, base, used);
        r.check(g_nops == 0, "rumble pad: the same strength is not sent again");
        in.strength = 0.7f; g_nops = 0;
        m.update(in, base, used);
        r.check(count(Op::Pad) == 1 && near(g_ops[0].a, 0.7f), "rumble pad: a changed strength is sent");
        in.side = -1.f; in.strength = 0.7f; g_nops = 0;
        m.update(in, base, used);
        r.check(g_nops == 0, "rumble pad: the side is ignored (one strength for both)");
        g_present[5] = false; g_nops = 0;
        m.update(in, base, used);
        r.check(used == 0 && count(Op::Pad) == 0 && count(Op::Touch) == 2,
                "rumble pad gone: no write to it, the Touch controllers take over (L %.2f)", last_touch(Input::kLeft));
    }
    // switching
    {
        Rumble m;
        m.set_out(out);
        g_present[3] = true;
        uint32_t used = 0;
        Rumble::Inputs in;
        in.strength = 0.5f;
        g_nops = 0;
        m.update(in, base, used);
        used = 3; g_nops = 0;
        m.update(in, base, used);
        r.check(count(Op::StopTouch) == 1 && count(Op::Pad) == 1 && near(g_ops[1].a, 0.5f),
                "rumble switch Touch -> pad: the Touch controllers go quiet first, then the pad's 0.5");
        used = 0; g_nops = 0;
        m.update(in, base, used);
        r.check(count(Op::Pad) == 1 && g_ops[0].id == 3 && g_ops[0].a == 0.f && count(Op::Touch) == 2,
                "rumble switch pad -> Touch: the pad's motors off first, then the impulses");
        // stop with a pad running
        used = 3; g_nops = 0;
        m.update(in, base, used);
        g_nops = 0;
        m.stop();
        r.check(count(Op::Pad) == 1 && g_ops[0].id == 3 && g_ops[0].a == 0.f && m.level() == 0.f, "rumble stop: the pad's motors off");
        // stop with the pad gone writes nothing to it
        g_nops = 0;
        used = 3;
        m.update(in, base, used);
        g_present[3] = false;
        g_nops = 0;
        m.stop();
        r.check(g_nops == 0, "rumble stop, pad gone: nothing written");
        // stop with touch running
        used = 0; g_nops = 0;
        m.update(in, base, used);
        g_nops = 0;
        m.stop();
        r.check(count(Op::StopTouch) == 1, "rumble stop: the Touch controllers stopped");
    }
}

}

int rig_test(const char *dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string path = std::string(dir) + "/rig_test.txt";
    Report r;
    r.f = std::fopen(path.c_str(), "w");
    if (!r.f) log("rig-test: cannot write %s", path.c_str());

    wheel_checks(r);
    stick_checks(r);
    shifter_checks(r);
    rumble_checks(r);

    int total = r.pass + r.fail;
    char head[96];
    std::snprintf(head, sizeof head, r.fail == 0 ? "rig test: all %d passed" : "rig test: %d of %d FAILED", r.fail == 0 ? total : r.fail, total);
    if (r.f) {
        std::fprintf(r.f, "%s\n", head);
        std::fclose(r.f);
    }
    log("%s (wheel full lock %d) -> %s", head, kFull, path.c_str());
    return r.fail == 0 ? 0 : 1;
}

}
}
