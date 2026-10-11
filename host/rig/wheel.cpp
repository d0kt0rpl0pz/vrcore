#include "wheel.h"

#include <cmath>

namespace vrcore {
namespace rig {

namespace {

// the float maths, defined here
const float kRad2Deg = 57.29578f, kDeg2Rad = 0.0174532924f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
int sign(float v) { return v > 0.f ? 1 : v < 0.f ? -1 : 0; }
// Mathf.RoundToInt: (int)Math.Round, halves to even (the default rounding mode)
int round_int(float v) { return (int)std::nearbyint((double)v); }
float cosf_(float v) { return (float)std::cos((double)v); }
float sinf_(float v) { return (float)std::sin((double)v); }

}

int Wheel::step(State &s, Vec3 pL, Vec3 pR, bool tracked, Vec3 forward, float dt, int lockDeg, int deadDeg,
                int pitchDeg, int fullLock, Vec3 *vout)
{
    Vec3 v;
    if (vout) *vout = v;
    if (!tracked) {                                 // tracking lost: held, then eased to centre
        s.have = false;
        s.crossed = false;
        s.lost += dt;
        float k = s.lost <= HoldLost ? 1.f : clamp01(1.f - (s.lost - HoldLost) / EaseLost);
        int eased = round_int((float)s.steer * k);
        if (k == 0.f) s.steer = 0;
        return eased;
    }
    s.lost = 0;
    Vec3 right, up, axis;
    plane(forward, (float)pitchDeg, right, up, axis);
    v = pR - pL;
    v -= axis * Vec3::dot(v, axis);                 // the wheel plane: along its axis does not steer
    if (vout) *vout = v;
    if (v.magnitude() < MinSpan) return s.steer;    // hands together: the angle is noise
    float lat = Vec3::dot(v, right);
    float raw = (float)std::atan2((double)Vec3::dot(v, up), (double)lat) * kRad2Deg;
    if (!s.have) {
        s.theta = raw;
        s.have = true;
    } else {                                        // the turn nearest the last, within +-270
        float t = raw;
        while (t - s.theta > 180.f) t -= 360.f;
        while (t - s.theta < -180.f) t += 360.f;
        if (t > 270.f) t -= 360.f;
        if (t < -270.f) t += 360.f;
        s.theta = t;
    }
    if (!s.crossed && lat < 0.f) s.crossed = true;
    else if (s.crossed && lat > Uncross) s.crossed = false;
    float lk = clampf((float)lockDeg, (float)MinLock, (float)MaxLock);
    float dz = clampf((float)deadDeg, 0.f, lk - 1.f);
    float a = std::fabs(s.theta);
    float x = (s.crossed && lk <= 90.f) || a >= lk ? 1.f : clamp01((a - dz) / (lk - dz));
    s.steer = -sign(s.theta) * round_int(x * (float)fullLock);      // right hand up = left
    return s.steer;
}

void Wheel::plane(Vec3 forward, float pitchDeg, Vec3 &right, Vec3 &up, Vec3 &axis)
{
    Vec3 f = forward;
    f.y = 0;
    f = f.sqr_magnitude() > 1e-6f ? f.normalized() : Vec3::forward();
    right = Vec3::cross(Vec3::up(), f);
    float p = clampf(pitchDeg, (float)MinPitch, (float)MaxPitch) * kDeg2Rad;
    up = Vec3::up() * cosf_(p) + f * sinf_(p);
    axis = f * cosf_(p) - Vec3::up() * sinf_(p);
}

// the room's LOCAL space (right-handed, -z forward) to the wheel's (left-handed, +z forward)
static Vec3 to_lh(const float p[3]) { return {p[0], p[1], -p[2]}; }

bool Wheel::read(const Input &in, float seat_yaw, int &steer, int full)
{
    const Hand &lh = in.hands[Input::kLeft], &rh = in.hands[Input::kRight];
    tracked_ = lh.active && rh.active && lh.tracked && rh.tracked;
    read_ = true;
    // the seat's forward: the room's (-sin yaw, 0, -cos yaw), z turned for the wheel's space
    fwd_ = {-std::sin(seat_yaw), 0.f, std::cos(seat_yaw)};
    Vec3 pL, pR;
    if (tracked_) {
        pL = to_lh(lh.grip.pos);
        pR = to_lh(rh.grip.pos);
        mid_ = (pL + pR) * 0.5f;
    }
    Vec3 v;
    steer = step(st_, pL, pR, tracked_, fwd_, in.dt, lockDeg, deadDeg, pitchDeg, full, &v);
    if (tracked_) span_ = v;
    return tracked_ || steer != 0;
}

}
}
