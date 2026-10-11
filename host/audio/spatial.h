/*
 * spatial.h -- the virtual speakers through Steam Audio (the plan's Audio): one context, the built-in HRTF
 * (RMS-normalised, so the binaural output keeps the input's loudness), one binaural effect per speaker
 * (bilinear interpolation, fully spatial). Per block: each speaker's direction from the head, its channel through
 * its effect, the ears' outputs summed (not clamped: the caller adds its direct channels, then clamps). No distance
 * attenuation, no reflections, no occlusion.
 *
 * init / shutdown on any thread but not during process; process on one thread (the audio thread): it does not
 * allocate, lock or log.
 */
#pragma once

#include "audio.h"

namespace vrcore::audio {

class Spatial {
public:
    Spatial() = default;
    Spatial(const Spatial &) = delete;
    Spatial &operator=(const Spatial &) = delete;
    ~Spatial() { shutdown(); }

    // Steam Audio at `hz`, kBlock frames, an input `channels` wide, the speakers (up to kMaxSpeakers; each its
    // channel of the input). False (logged): not ready.
    bool init(int hz, int channels, const Speaker *speakers, int n);
    bool ready() const { return ready_; }
    // kBlock frames in (init's channels wide), kBlock frames of stereo out (may not alias), for the head's pose
    // in seat space.
    void process(const float *in, float *out, const Pose &head);
    // The effects' history cleared (after a gap, or when switched back on).
    void reset();
    void shutdown();

private:
    struct Impl;
    Impl *impl_ = nullptr;
    bool ready_ = false;
};

}
