/*
 * wheel.h -- steering with both hands: a wheel held in the air (the device-free part; the
 * device reading is wheel.cpp's, the model and its drawing wheel_model's).
 *
 * Hold both controllers as if gripping a wheel: the roll of the line between them is the wheel's angle.
 *
 * THE ANGLE. The vector from the left controller to the right, laid into the wheel's plane (its part along the
 * wheel's axis taken out: pushing one hand forward does not steer), and measured there against the wheel's own up:
 * level = straight ahead, right hand higher = left. The wheel's plane leans back by the Wheel pitch (default 25
 * degrees, as a car's: the top edge away from you, the face turned up towards you), so turning the hands round the
 * wheel you see is exactly its angle. "Forward" is the recentred seat's, never the head's: looking into a corner
 * does not steer. Only the line between the hands counts, so no body origin is needed. The angle is kept
 * continuous from frame to frame (a twist through 180 degrees does not jump to the other lock), within +-270.
 *
 * CROSSOVER. At a lock of 90 degrees or less: once the right hand is left of the left hand (in the wheel plane:
 * past 90 degrees, or one hand stacked over the other), it is full lock towards the side the hands went, until
 * they are back past each other by Uncross (2 cm), so a hand resting right over the other does not flicker. Past
 * 90 (up to 135, the games' cabinets' own lock) the arms cross as part of the travel: no latch, the continuous
 * angle carries on to full lock.
 *
 * THE GAME'S SIDE. The angle maps linearly to 0..fullLock of the steering's -32767..32767, past the deadzone:
 * fullLock is the game's setting, the value where its own steering curve goes flat (more would be wasted travel),
 * and its curve's own deadzone near centre is the "Default" deadzone here (deadDeg 0: nothing added). Hands holding
 * nothing get a lighter lock than a cabinet's wheel: a setting of 30-135 degrees (default 75).
 *
 * Tracking lost on either hand: the last value for 0.1 s, then back to centre over 0.2 s, never a snap.
 *
 * THE WHEEL'S SPACE. step() and plane() take and give LEFT-HANDED vectors: x right, y up, z
 * forward (cross is the same formula as ever, so right = cross(up, forward) is +x for a
 * forward of +z; the angle it measures has "right hand up = positive theta"). The host's LOCAL space is
 * right-handed (y up, -z forward): the caller converts the poses and the seat's forward (z negated) before the
 * call and uses the result as it is. Nothing in here depends on the host's spaces.
 */
#pragma once

#include <cmath>

#include "../app/input.h"

namespace vrcore {
namespace rig {

// A 3-vector, with the operations the wheel uses (magnitude through double).
struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float f) const { return {x * f, y * f, z * f}; }
    Vec3 operator/(float f) const { return {x / f, y / f, z / f}; }
    Vec3 &operator+=(const Vec3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3 &operator-=(const Vec3 &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }

    float sqr_magnitude() const { return x * x + y * y + z * z; }
    float magnitude() const { return (float)std::sqrt((double)sqr_magnitude()); }
    // a vector under 1e-5 long gives zero
    Vec3 normalized() const
    {
        float m = magnitude();
        return m > 1e-5f ? *this / m : Vec3();
    }

    static float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static Vec3 cross(const Vec3 &a, const Vec3 &b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    static Vec3 up() { return {0.f, 1.f, 0.f}; }
    static Vec3 forward() { return {0.f, 0.f, 1.f}; }
};

class Wheel {
public:
    static constexpr int DefaultLock = 75, MinLock = 30, MaxLock = 135;     // degrees of hand roll for full lock
    static constexpr int DefaultPitch = 25, MinPitch = 0, MaxPitch = 45;    // degrees the wheel leans back
    static constexpr int DeadSteps[6] = {0, 1, 2, 3, 5, 8};                 // degrees of extra deadzone
    static constexpr float ShowLock = 135.f;        // degrees: the model turns no further, each way
    static constexpr float MinSpan = 0.10f;         // m: hands closer than this hold the last value
    static constexpr float Uncross = 0.02f;         // m: back past each other by this to uncross
    static constexpr float HoldLost = 0.1f, EaseLost = 0.2f;    // s: tracking lost

    struct State {
        float theta = 0.f;                          // the continuous angle, degrees, within +-270
        bool have = false, crossed = false;
        int steer = 0;                              // the last value
        float lost = 0.f;                           // seconds without tracking
    };

    // The steering maths, one frame (device-free): the steering, -fullLock..fullLock (+ = right). `v` gets the
    // hands' line in the wheel plane (zero when untracked). Left-handed vectors (see the top).
    static int step(State &s, Vec3 pL, Vec3 pR, bool tracked, Vec3 forward, float dt, int lockDeg, int deadDeg,
                    int pitchDeg, int fullLock, Vec3 *v = nullptr);

    // The wheel's frame: right (level), up (leaning back by pitch: the top edge forward) and its axis (forward,
    // tipped down by the same), from the seat's forward. Left-handed.
    static void plane(Vec3 forward, float pitchDeg, Vec3 &right, Vec3 &up, Vec3 &axis);

    // ---- the device side ----
    bool hands = true;                              // the menu's Steering: Hands (wheel) | Thumbstick
    int lockDeg = DefaultLock, deadDeg = 0, pitchDeg = DefaultPitch;
    bool show = true;
    int fullLock = 32767;

    // Once a frame while the hands steer: the steering, -fullLock..fullLock (+ = right), from both Touch
    // controllers' grip poses (the room's LOCAL space) and the seat's yaw (its forward: looking into a corner does
    // not steer). False when there is nothing to give (the hands untracked and back at centre).
    bool read(const Input &in, float seat_yaw, int &steer) { return read(in, seat_yaw, steer, fullLock); }
    // The same with this frame's full lock in place of fullLock (the selects' full travel).
    bool read(const Input &in, float seat_yaw, int &steer, int fullLock);
    bool tracked() const { return tracked_; }
    const State &state() const { return st_; }
    // for the model (left-handed): between the hands, their line in the wheel plane, the seat's forward
    const Vec3 &mid() const { return mid_; }
    const Vec3 &span() const { return span_; }
    const Vec3 &forward() const { return fwd_; }
    // read since the last call (the model shows only while the hands steer)
    bool take_read()
    {
        bool r = read_;
        read_ = false;
        return r;
    }

private:
    State st_;
    bool tracked_ = false, read_ = false;
    Vec3 mid_, span_, fwd_;                         // left-handed: between the hands, their line, the seat's forward
};

}
}
