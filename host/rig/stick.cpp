#include "stick.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "ui/text.h"
#include "ui/textstrings.h"

namespace vrcore {
namespace rig {

const char *const Stick::ResponseNames[kResponses] = {txt::rig_response_linear, txt::rig_response_slight, txt::rig_response_medium,
                                                      txt::rig_response_strong, txt::rig_response_very_strong};
const int Stick::DeadzoneSteps[kDeadzoneSteps] = {0, 2, 5, 8, 10, 15, 20};
const int Stick::RangeSteps[kRangeSteps] = {100, 90, 80, 70, 60};
const int Stick::TimeSteps[kTimeSteps] = {0, 50, 100, 150, 200, 300};

namespace {

const float kGammas[Stick::kResponses] = {1.f, 1.3f, 1.7f, 2.2f, 3.f};

int sign(float v) { return v > 0.f ? 1 : v < 0.f ? -1 : 0; }

// the step after (d > 0) or before `v` in the list, wrapping; a value not in the list counts as the first
template <int N> int next(const int (&steps)[N], int v, int d)
{
    int i = 0;
    for (int k = 0; k < N; ++k)
        if (steps[k] == v) { i = k; break; }
    return steps[(i + d + N) % N];
}

// full lock per second from a step's milliseconds; 0: instant
float rate(int ms) { return ms <= 0 ? 0.f : 1000.f / (float)ms; }

// a menu row's short text, cut to the Label
Label label(const std::string &text)
{
    Label l;
    std::snprintf(l.s, sizeof l.s, "%s", text.c_str());
    return l;
}

}

void Stick::step_response(int d) { response = (response + d + kResponses) % kResponses; }
void Stick::step_deadzone(int d) { deadzone = next(DeadzoneSteps, deadzone, d); }
void Stick::step_range(int d) { range = next(RangeSteps, range, d); }
void Stick::step_turn(int d) { turn = next(TimeSteps, turn, d); }
void Stick::step_snapback(int d) { snapback = next(TimeSteps, snapback, d); }

Label Stick::deadzone_name() const
{
    return deadzone == 0 ? label(txt::rig_deadzone_off)
                         : label(ui::fmt(txt::rig_percent, {{"v", std::to_string(deadzone)}}));
}

Label Stick::range_name() const
{
    return range == 100 ? label(txt::rig_range_full) : label(ui::fmt(txt::rig_percent, {{"v", std::to_string(range)}}));
}

Label Stick::time_name(int ms)
{
    return ms == 0 ? label(txt::rig_time_instant) : label(ui::fmt(txt::rig_time_ms, {{"ms", std::to_string(ms)}}));
}

float Stick::gamma(int response) { return kGammas[response]; }

int Stick::steer(float x, float dt, int range, int full)
{
    now_ = damp(now_, shape(x, (float)deadzone / 100.f, kGammas[response], (float)range / 100.f), dt, rate(turn),
                rate(snapback));
    return to_game(now_, gameDead, full);
}

int Stick::to_game(float v, int dead, int full)
{
    if (v == 0.f) return 0;
    float m = std::min(std::fabs(v), 1.f);
    int lo = std::min(dead + 1, full);
    // Math.Round: halves to even (the default rounding mode)
    return sign(v) * std::min((int)std::nearbyint((double)((float)lo + m * (float)(full - lo))), full);
}

float Stick::shape(float x, float dz, float gamma, float range)
{
    float m = std::min(std::fabs(x), 1.f);
    if (m <= dz) return 0.f;
    float a = (m - dz) / (1.f - dz);
    if (gamma != 1.f) a = (float)std::pow((double)a, (double)gamma);
    return (float)sign(x) * std::min(a * range, 1.f);
}

float Stick::damp(float s, float target, float dt, float turn, float back)
{
    if (s != 0.f && (target * s <= 0.f || std::fabs(target) < std::fabs(s))) {      // heading in
        float stop = target * s > 0.f ? target : 0.f;
        if (back > 0.f) {
            float need = std::fabs(s - stop) / back;
            if (need > dt) return s + (float)sign(stop - s) * back * dt;
            dt -= need;
        }
        s = stop;
    }
    if (s == target) return s;
    if (turn <= 0.f) return target;                                                 // heading out
    float step = turn * dt;
    return std::fabs(target - s) <= step ? target : s + (float)sign(target - s) * step;
}

}
}
