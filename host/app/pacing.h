/*
 * pacing.h -- which game frame a front draws on each display frame, and how far between it and the one before
 * (alpha). Any board's front uses it.
 *
 * BETWEEN GAME FRAMES. A board runs at its own rate (59.906 Hz or 57.524 Hz, say) and a headset
 * draws at 72-120 Hz. Every 3D vertex carries where it was one game frame earlier (abi/vrcore.h's prev), and
 * the board's shader draws lerp(prev, pos, alpha). This picks the frame and the alpha for each display frame:
 * a RENDER CLOCK runs on the plugin's own clock (vr_now_ns, the one the frames are stamped with) a game frame
 * and a margin in the past -- advanced by whole display periods, so alpha steps evenly, and pulled gently
 * towards now - delay -- and the frame drawn is the one whose slot contains it: alpha = (clock - its previous
 * slot) / frame. The margin is the game thread's own lateness (a frame's publish time after its slot began),
 * the most over the last two seconds plus 1 ms. A frame that is not there in time holds at alpha 1 and counts
 * as late; nothing is extrapolated. Off (interpolate false): the newest frame at alpha 1.
 *
 * THE STEPS:
 *   - the display period is the Tick's (xrWaitFrame's predictedDisplayPeriod), every frame;
 *   - the clock's step is the difference of consecutive display times rounded to whole periods.
 * `now` is the plugin's clock as the display frame begins. NOT the predicted display time (the M2
 * proposal's plan): that lies ~2 display frames ahead, so the clock aimed past the newest published frame and
 * nearly every display frame held at alpha 1 (Quest 3, 2026-10-10: late 107-122 of 120 a second, step sd 0.5;
 * the 1/32 pull settles onto the target, it does not take the offset out). A frame that is not made yet cannot
 * be drawn without extrapolating, which the pacing never does.
 *
 * Each display frame the front calls want (the frame to ask vr_frame for; 0 = the newest), took for a frame it
 * copied, then end. The stats line keeps its fields, so old logs compare.
 */
#pragma once

#include <cstdint>
#include <string>

#include "app.h"
#include "plugin.h"

namespace vrcore {

class Pacing {
public:
    explicit Pacing(const Plugin &p) : plugin_(p) {}

    bool interpolate = true;            // draw between game frames (off: the newest, as the game made it)
    float alpha() const { return alpha_; }  // this display frame: 0 = the frame before, 1 = shown()
    uint32_t shown() const { return have_; }  // the game frame drawn (0 = none yet)

    // A display frame begins (t: when it is shown): the game frame to ask for (0 = the newest).
    uint32_t want(const Tick &t);
    // The front copied game frame f (vr_frame's frame number, t_ns and ready_ns).
    void took(uint32_t f, int64_t t_ns, int64_t ready_ns);
    // The display frame's alpha and counters. false: no frame yet (alpha unchanged).
    bool end();
    // Once a second or so: what the pacing did since the last call (the stats line; "" before any frame).
    std::string stats();
    // A new run (a restart): no frame shown, the clock and counters as at the start; `interpolate` kept.
    void reset();

private:
    void advance_clock(const Tick &t);

    const Plugin &plugin_;
    uint32_t have_ = 0, was_ = 0;
    float alpha_ = 1.f;
    int64_t frame_ns_ = 0, shown_t_ = 0, clock_ = 0, margin_ns_ = 4000000;
    bool clock_set_ = false;
    int64_t last_display_ns_ = 0;
    int64_t lateness_[120] = {};
    int lateness_at_ = 0;
    double display_hz_ = 60;
    int display_frames_ = 0, late_frames_ = 0, skipped_frames_ = 0, resyncs_ = 0;
    double step_sum_ = 0, step_sq_ = 0, last_pos_ = -1;
    int steps_ = 0;
    int advanced_[4] = {};              // game frames taken per display frame: 0, 1, 2, 3+
};

}
