/*
 * rig.h -- where a game's world hangs in the room: the seat, recentring, looking
 * behind, flying round a frozen world, and Explore.
 *
 * THE SEAT: the game camera's place in the room. It goes where the head is, facing where it faces, level: once
 * on the first drawn frame, and one drawn frame after each of the headset's own recentres (a LOCAL space change:
 * the space's origin moved to the head, Tick::recentres), from the frame's eyes (place). The game's view space
 * (x right, y up, z forward, units) hangs from it scaled by metres_per_unit; the room is right-handed (z back),
 * so the game's z goes to the room's -z: world = T(seat) Ry(yaw) diag(s, s, -s) [inner] (world_matrix).
 *
 * LOOK BEHIND: the right thumbstick's click (Touch) or any pad's right stick click, held: the world drawn turned
 * 180 degrees about the seat while held, a cut each way. Not while flying, nor when the game says no.
 *
 * FLYING (a free camera round a frozen frame): set_flying(true) keeps the seat's pose, false puts it back. While
 * the game says its world is ready (fly's argument) the seat moves and turns under you. Touch: the left stick
 * flies where you look, the right stick's y rises and sinks, its x snap-turns 30 degrees (in past 0.6, out under
 * 0.4; smoothly on the desktop sim, which is a flat screen); a grip held goes 5x, doubling each second held, up
 * to 250x. A pad: the same on its sticks, a shoulder the boost. Keys: W A S D, E / Q up / down, the left / right
 * arrows turn (90 degrees a second), Shift the boost. The speed goes with the world scale.
 *
 * EXPLORE (VR_OPT_EXPLORE): flying on and the plugin told; the world is flown round once the plugin reads 2
 * (explore_frozen). Off: the plugin told, the seat back home.
 *
 * The game calls track, look_behind and fly once an update, in that order, and place once a prepare.
 */
#pragma once

#include <cstdint>

#include "../app/app.h"
#include "../app/input.h"
#include "../app/plugin.h"
#include "../gpu/render.h"
#include "../gpu/vmath.h"

namespace vrcore::rig {

class Rig {
public:
    static constexpr float kFlySpeed = 8.f;         // m/s at world scale x1
    static constexpr float kFlyRampSeconds = 1.f;   // the boost doubles each (from 5x)
    static constexpr float kFlyMaxBoost = 250.f;

    explicit Rig(float metres_per_unit) : default_scale_(metres_per_unit), metres_per_unit_(metres_per_unit) {}

    // ---- update ----
    void track(const Tick &t);                      // the headset's recentres: the seat follows a drawn frame on
    void look_behind(const Input &in, bool can);    // `can`: the game's own reasons (and no menu)
    void fly(const Input &in, bool ready);          // ready: the world can be flown round
    void set_flying(bool on);
    void recentre() { recentre_wait_ = 0; }         // on the next drawn frame
    // Explore on or off (false: already so). The plugin's VR_OPT_EXPLORE set; flying with it.
    bool set_explore(bool on, Plugin &p);
    bool explore_frozen(const Plugin &p) const { return flying_ && p.get_option(VR_OPT_EXPLORE) == 2; }

    // ---- prepare ----
    void place(const gpu::Frame &f);                // the first / a pending recentre, from the frame's eyes

    // ---- the seat ----
    const float *seat() const { return seat_; }
    float yaw() const { return yaw_; }                                      // the seat's
    float world_yaw() const { return looking_back_ ? yaw_ + kPi : yaw_; }   // the world's (looking behind)
    float metres_per_unit() const { return metres_per_unit_; }
    float scale() const { return metres_per_unit_ / default_scale_; }       // x the game's default
    // The world scale: x the game's default, not under 0.1. The seat stays.
    void set_scale(float x) { metres_per_unit_ = default_scale_ * (x < 0.1f ? 0.1f : x); }
    bool looking_back() const { return looking_back_; }
    bool flying() const { return flying_; }
    // A room (LOCAL) pose in the seat's frame: T(seat) Ry(yaw) undone. Not look behind's turn, not the world scale
    // (the cabinet's sound: its speakers are fixed to the seat, in metres).
    Pose to_seat(const Pose &room) const;
    // The game's view space (then `inner`) to the room; its inverse given `inner`'s.
    Mat4 world_matrix(const Mat4 &inner) const;
    Mat4 world_inverse(const Mat4 &inner_inv) const;

private:
    static constexpr float kPi = 3.14159265f;
    void recentre_now(const gpu::Frame &f);
    void turn(const Input &in, float rad);          // you turn by rad (+ right) about your head

    float default_scale_, metres_per_unit_;
    bool centred_ = false;
    float seat_[3] = {0, 0, 0}, yaw_ = 0;
    uint32_t recentres_ = 0;
    int recentre_wait_ = -1;                        // drawn frames to the pending recentre (-1: none)
    bool looking_back_ = false;
    bool flying_ = false;
    [[maybe_unused]] bool snap_held_ = false;      // the headset's snap turn (Android)
    float boost_held_ = 0;
    float home_[3] = {0, 0, 0}, home_yaw_ = 0;
};

}
