/*
 * rig.cpp -- the seat, look behind, flying and Explore (rig.h).
 */
#include "rig.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>

#include "../app/log.h"

namespace vrcore::rig {

namespace {

float length(const float v[2]) { return std::sqrt(v[0] * v[0] + v[1] * v[1]); }

}

void Rig::track(const Tick &t)
{
    if (t.recentres != recentres_) {
        recentres_ = t.recentres;
        recentre_wait_ = 1;
    }
}

void Rig::place(const gpu::Frame &f)
{
    if (!centred_) {
        recentre_now(f);
        centred_ = true;
    } else if (recentre_wait_ == 0) {
        recentre_now(f);
        recentre_wait_ = -1;
    } else if (recentre_wait_ > 0) {
        --recentre_wait_;
    }
}

// The seat goes where the head is, facing where it faces (level).
void Rig::recentre_now(const gpu::Frame &f)
{
    for (int i = 0; i < 3; ++i) seat_[i] = 0.5f * (f.eyes[0].pos[i] + f.eyes[1].pos[i]);
    const float *v = f.eyes[0].view.m;
    float fx = -v[2], fz = -v[10];              // the eye's forward (its -z) in the room, levelled
    if (fx * fx + fz * fz > 1e-8f) yaw_ = std::atan2(-fx, -fz);
    log("rig: recentred at %.3f %.3f %.3f, yaw %.1f", seat_[0], seat_[1], seat_[2], yaw_ * 57.2958f);
}

Mat4 Rig::world_matrix(const Mat4 &inner) const
{
    float s = metres_per_unit_;
    return mat4_mul(mat4_translate(seat_[0], seat_[1], seat_[2]),
                    mat4_mul(mat4_rotate(quat_axis_angle(0, 1, 0, world_yaw())),
                             mat4_mul(mat4_scale(s, s, -s), inner)));
}

Mat4 Rig::world_inverse(const Mat4 &inner_inv) const
{
    float s = metres_per_unit_;
    return mat4_mul(inner_inv,
                    mat4_mul(mat4_scale(1 / s, 1 / s, -1 / s),
                             mat4_mul(mat4_transpose3(mat4_rotate(quat_axis_angle(0, 1, 0, world_yaw()))),
                                      mat4_translate(-seat_[0], -seat_[1], -seat_[2]))));
}

Pose Rig::to_seat(const Pose &room) const
{
    Pose p;
    float x = room.pos[0] - seat_[0], z = room.pos[2] - seat_[2];
    float s = std::sin(yaw_), c = std::cos(yaw_);
    p.pos[0] = c * x - s * z;                   // Ry(-yaw)
    p.pos[1] = room.pos[1] - seat_[1];
    p.pos[2] = s * x + c * z;
    p.rot = quat_mul(quat_axis_angle(0, 1, 0, -yaw_), room.rot);
    return p;
}

void Rig::look_behind(const Input &in, bool can)
{
    bool held = in.hands[Input::kRight].stick_click;
    for (int i = 0; i < in.pad_count; ++i) held |= in.pads[i].right_stick;
    looking_back_ = can && !flying_ && held;
}

void Rig::set_flying(bool on)
{
    if (on == flying_) return;
    flying_ = on;
    if (on) {
        for (int i = 0; i < 3; ++i) home_[i] = seat_[i];
        home_yaw_ = yaw_;
        looking_back_ = false;
    } else {
        for (int i = 0; i < 3; ++i) seat_[i] = home_[i];
        yaw_ = home_yaw_;
    }
}

bool Rig::set_explore(bool on, Plugin &p)
{
    if (on == flying_ || !p.set_option) return false;
    set_flying(on);
    p.set_option(VR_OPT_EXPLORE, on ? 1 : 0);
    log("rig: explore %s", on ? "on" : "off");
    return true;
}

// You turn by `rad` about your head: the world (the seat) turns the other way about it.
void Rig::turn(const Input &in, float rad)
{
    const float *pivot = in.head_tracked ? in.head.pos : seat_;
    float px = pivot[0], pz = pivot[2];
    float c = std::cos(rad), s = std::sin(rad);
    float x = seat_[0] - px, z = seat_[2] - pz;
    seat_[0] = px + c * x + s * z;              // Ry(rad): the seat's offset from the head turned with it
    seat_[2] = pz - s * x + c * z;
    yaw_ += rad;
}

void Rig::fly(const Input &in, bool ready)
{
    if (!flying_ || !ready) return;
    const float dead = 0.15f;
    const Hand &lh = in.hands[Input::kLeft], &rh = in.hands[Input::kRight];
    float move[2] = {lh.stick[0], lh.stick[1]}, look[2] = {rh.stick[0], rh.stick[1]};
    bool fast = lh.squeezed || rh.squeezed;
    float smooth = 0;
    for (int i = 0; i < in.pad_count; ++i) {
        const Pad &p = in.pads[i];
        if (p.left[0] * p.left[0] + p.left[1] * p.left[1] > move[0] * move[0] + move[1] * move[1]) {
            move[0] = p.left[0];
            move[1] = p.left[1];
        }
        if (p.right[0] * p.right[0] + p.right[1] * p.right[1] > look[0] * look[0] + look[1] * look[1]) {
            look[0] = p.right[0];
            look[1] = p.right[1];
        }
        fast |= p.left_shoulder || p.right_shoulder;
    }
    float kx = (in.keys[SDL_SCANCODE_D] ? 1.f : 0.f) - (in.keys[SDL_SCANCODE_A] ? 1.f : 0.f);
    float ky = (in.keys[SDL_SCANCODE_W] ? 1.f : 0.f) - (in.keys[SDL_SCANCODE_S] ? 1.f : 0.f);
    if (kx != 0 || ky != 0) {
        float n = std::sqrt(kx * kx + ky * ky);
        move[0] = kx / n;
        move[1] = ky / n;
    }
    if (in.keys[SDL_SCANCODE_E]) look[1] = 1;
    if (in.keys[SDL_SCANCODE_Q]) look[1] = -1;
    if (in.keys[SDL_SCANCODE_RIGHT]) smooth = 1;
    if (in.keys[SDL_SCANCODE_LEFT]) smooth = -1;
    fast |= in.keys[SDL_SCANCODE_LSHIFT] || in.keys[SDL_SCANCODE_RSHIFT];
    if (length(move) < dead) move[0] = move[1] = 0;
    float dt = in.dt;

    // turning: in the headset a cut of 30 degrees (a turn you watch is what makes people sick), past 0.6 and
    // let go below 0.4; on a flat screen (the desktop sim), smoothly
#ifdef __ANDROID__
    if (!snap_held_ && std::fabs(look[0]) > 0.6f) {
        turn(in, (look[0] > 0 ? 1.f : -1.f) * 30.f * kPi / 180.f);
        snap_held_ = true;
    } else if (std::fabs(look[0]) < 0.4f) {
        snap_held_ = false;
    }
#else
    if (std::fabs(look[0]) > dead) smooth += look[0];
#endif
    if (smooth != 0) turn(in, std::clamp(smooth, -1.f, 1.f) * 90.f * kPi / 180.f * dt);

    // where you look, rising and sinking on the right stick; the speed goes with the world scale. The boost: 5x
    // at once, then doubling every kFlyRampSeconds while held
    boost_held_ = fast ? boost_held_ + dt : 0;
    float boost = fast ? std::min(5.f * std::pow(2.f, boost_held_ / std::max(kFlyRampSeconds, 0.05f)),
                                  std::max(kFlyMaxBoost, 5.f))
                       : 1.f;
    float speed = kFlySpeed * boost * scale() * dt;
    float lift = std::fabs(look[1]) > dead ? look[1] : 0;
    Quat q = in.head_tracked ? in.head.rot : quat_axis_angle(0, 1, 0, yaw_);
    static const float kForward[3] = {0, 0, -1}, kRight[3] = {1, 0, 0};
    float fwd[3], right[3];
    quat_rotate(q, kForward, fwd);
    quat_rotate(q, kRight, right);
    for (int i = 0; i < 3; ++i) {
        float d = (fwd[i] * move[1] + right[i] * move[0]) * speed + (i == 1 ? lift * speed : 0.f);
        seat_[i] -= d;
    }
}

}
