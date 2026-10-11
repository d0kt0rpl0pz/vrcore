#include "shifter.h"

#include <cmath>

namespace vrcore {
namespace rig {

namespace {
int sign(float v) { return v > 0.f ? 1 : v < 0.f ? -1 : 0; }
const float kRad2Deg = 57.29578f;               // Mathf.Rad2Deg
}

int Shifter::read(const Input &in, bool driving)
{
    int was = gate;
    float sx = 0.f, sy = 0.f;
    uint32_t src = 0;
    if (on && driving) {
        const Hand &h = in.hands[Input::kRight];
        if (h.active) { sx = h.stick[0]; sy = h.stick[1]; }
        for (int i = 0; i < in.pad_count; ++i) {
            const Pad &p = in.pads[i];
            if (p.right[0] * p.right[0] + p.right[1] * p.right[1] > sx * sx + sy * sy) {
                sx = p.right[0];
                sy = p.right[1];
                src = p.id;
            }
        }
    }
    gate = mode == Mode::HGate4 ? corner(sx, sy, was) : lever(sx, sy, was);
    engaged = gate != 0 && gate != was;
    if (gate != 0) from = src;
    return gate;
}

int Shifter::lever(float x, float y, int was)
{
    int dir = sign(y);
    if (was != 0 && dir == was && std::fabs(y) > SeqOut) return was;
    return std::fabs(y) >= SeqIn && std::fabs(y) > std::fabs(x) ? dir : 0;
}

int Shifter::corner(float x, float y, int was)
{
    if (x == 0.f || y == 0.f) return 0;
    int c = x < 0.f ? (y > 0.f ? 1 : 2) : (y > 0.f ? 3 : 4);
    float len = (float)std::sqrt((double)(x * x + y * y));
    float off = std::fabs((float)std::atan2((double)std::fabs(y), (double)std::fabs(x)) * kRad2Deg - 45.f);
    if (c == was) return len >= GateOut && off <= GateOutDeg ? c : 0;
    return len >= GateIn && off <= GateInDeg ? c : 0;
}

}
}
