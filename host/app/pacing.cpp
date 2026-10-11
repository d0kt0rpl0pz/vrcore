#include "pacing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace vrcore {

// THE RENDER CLOCK, once per display frame: a game frame and the margin behind now. Advanced by
// whole display periods, so alpha steps evenly; the error to now - delay is taken out a 32nd a frame (the
// clocks' drift, the margin's changes); past three game frames off it is reset (a stall, a pause).
void Pacing::advance_clock(const Tick &t)
{
    int64_t period = t.period_ns > 0 ? t.period_ns : 16666667;
    display_hz_ = 1e9 / (double)period;
    int64_t target = plugin_.now_ns() - frame_ns_ - margin_ns_;
    int64_t last = last_display_ns_;
    last_display_ns_ = t.display_ns;
    if (!clock_set_) {
        clock_ = target;
        clock_set_ = true;
        return;
    }
    double periods = std::max(1.0, std::round((double)(t.display_ns - last) / (double)period));
    clock_ += (int64_t)(periods * (double)period);
    int64_t err = target - clock_;
    if (std::llabs(err) > 3 * frame_ns_) {
        clock_ = target;
        ++resyncs_;
    } else {
        clock_ += err / 32;
    }
}

uint32_t Pacing::want(const Tick &t)
{
    if (frame_ns_ == 0) frame_ns_ = plugin_.frame_ns();
    advance_clock(t);
    was_ = have_;
    uint32_t w = 0;
    if (interpolate && have_ != 0) {
        w = have_;
        if (clock_ >= shown_t_) w = have_ + 1 + (uint32_t)((clock_ - shown_t_) / frame_ns_);
    }
    return w;
}

void Pacing::took(uint32_t f, int64_t t_ns, int64_t ready_ns)
{
    shown_t_ = t_ns;
    lateness_[lateness_at_++ % 120] = ready_ns - t_ns;
    int64_t worst = 0;
    for (int64_t l : lateness_) worst = std::max(worst, l);
    margin_ns_ = std::min(worst + 1000000, frame_ns_);
    have_ = f;
}

bool Pacing::end()
{
    if (have_ == 0) return false;
    ++display_frames_;
    int adv = (int)(have_ - was_);
    ++advanced_[std::min(std::max(adv, 0), 3)];
    if (have_ > was_ + 1 && was_ != 0) skipped_frames_ += (int)(have_ - was_ - 1);
    double a = interpolate ? (double)(clock_ - (shown_t_ - frame_ns_)) / (double)frame_ns_ : 1.0;
    if (a > 1.0005) ++late_frames_;
    alpha_ = (float)std::min(std::max(a, 0.0), 1.0);
    double pos = (have_ - 1) + (double)alpha_;          // the game time shown, in frames
    if (last_pos_ >= 0) {
        double st = pos - last_pos_;
        step_sum_ += st;
        step_sq_ += st * st;
        ++steps_;
    }
    last_pos_ = pos;
    return true;
}

void Pacing::reset()
{
    have_ = was_ = 0;
    alpha_ = 1.f;
    frame_ns_ = shown_t_ = clock_ = 0;
    margin_ns_ = 4000000;
    clock_set_ = false;
    last_display_ns_ = 0;
    std::fill(std::begin(lateness_), std::end(lateness_), int64_t(0));
    lateness_at_ = 0;
    display_hz_ = 60;
    display_frames_ = late_frames_ = skipped_frames_ = resyncs_ = 0;
    step_sum_ = step_sq_ = 0;
    last_pos_ = -1;
    steps_ = 0;
    std::fill(std::begin(advanced_), std::end(advanced_), 0);
}

std::string Pacing::stats()
{
    double is[9] = {};
    plugin_.interp_stats(is);
    int n = display_frames_;
    if (n == 0) return "";
    double mean = step_sum_ / std::max(1, steps_);
    double sd = std::sqrt(std::max(0.0, step_sq_ / std::max(1, steps_) - mean * mean));
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%.0f Hz %s: step %.3f sd %.3f (ideal %.3f), game frames/display 0:%d 1:%d 2:%d 3+:%d, late %d, "
                  "skipped %d, resync %d, margin %.1f ms, matched %.1f%% world, snaps %.0f",
                  display_hz_, interpolate ? "smooth" : "game rate", mean, sd, 1e9 / (double)frame_ns_ / display_hz_,
                  advanced_[0], advanced_[1], advanced_[2], advanced_[3], late_frames_, skipped_frames_, resyncs_,
                  margin_ns_ / 1e6, is[3] > 0 ? 100 * is[4] / is[3] : 0.0, is[5] + is[6] + is[7] + is[8]);
    display_frames_ = late_frames_ = skipped_frames_ = resyncs_ = steps_ = 0;
    step_sum_ = step_sq_ = 0;
    std::fill(std::begin(advanced_), std::end(advanced_), 0);
    return buf;
}

}
