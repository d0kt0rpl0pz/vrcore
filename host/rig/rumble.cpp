#include "rumble.h"

#include <algorithm>
#include <cmath>

#include "../app/input.h"

namespace vrcore {
namespace rig {

namespace {

float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

bool real_pad_there(uint32_t id) { return input().pad(id) != nullptr; }
void real_pad(uint32_t id, float low, float high) { pad_rumble(id, low, high); }
void real_touch(int hand, float amplitude, float seconds) { haptic(hand, amplitude, seconds); }
void real_stop_touch() { stop_haptics(); }

}

Rumble::Rumble() : out_{real_pad_there, real_pad, real_touch, real_stop_touch} {}

float Rumble::spring_strength(float steer, float speed) const
{
    if (!spring) return 0.f;
    float x = 1.f - clamp(std::fabs(steer), 0.f, 1.f);
    float v = speed < 0.f ? 1.f : clamp(speed, 0.f, 1.f);
    return SpringMax * (1.f - x * x) * (SpringFloor + (1.f - SpringFloor) * v);
}

void Rumble::pulse(float strength, float seconds, double now, float side)
{
    pulse_ = clamp(strength, 0.f, 1.f);
    pulse_side_ = clamp(side, -1.f, 1.f);
    pulse_until_ = now + (double)seconds;
}

void Rumble::update(const Inputs &in, double now, uint32_t &used)
{
    bool on = in.driving;
    float a = !on ? 0.f : in.strength;
    float s = on ? spring_strength(in.steer, in.speed) : 0.f;
    float p = on && now < pulse_until_ ? pulse_ : 0.f;
    if (used != 0 && !out_.pad_there(used)) used = 0;               // unplugged, or Bluetooth gone
    if (pad_ != used || (used != 0 && touch_)) stop();              // switched: the old one goes quiet
    if (a == 0.f && s == 0.f && p == 0.f && level_ == 0.f) return;
    float m = std::max(std::max(a, s), p);
    if (used != 0) {
        if (m != level_) out_.pad(used, m, 0.f);
        pad_ = used;
    } else {
        float side = clamp(in.side, -1.f, 1.f);
        float l = std::max(a * std::min(1.f, 1.f - side), s), r = std::max(a * std::min(1.f, 1.f + side), s);
        l = std::max(l, p * std::min(1.f, 1.f - pulse_side_));
        r = std::max(r, p * std::min(1.f, 1.f + pulse_side_));
        out_.touch(Input::kLeft, l, 0.1f);
        out_.touch(Input::kRight, r, 0.1f);
        touch_ = m != 0.f;
    }
    level_ = m;
}

void Rumble::stop()
{
    if (pad_ != 0 && out_.pad_there(pad_)) out_.pad(pad_, 0.f, 0.f);
    if (touch_) out_.stop_touch();
    pad_ = 0;
    touch_ = false;
    level_ = 0.f;
}

}
}
