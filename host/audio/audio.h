/*
 * audio.h -- the host's sound (and the plan's virtual speakers): one SDL
 * playback stream at the device's own rate, fed on SDL's audio thread by a game's pull (a plugin's vr_audio),
 * then spatialised: the pulled stream's channels (Layout) are speakers fixed in the SEAT's frame, rendered
 * binaurally by Steam Audio for the head's pose in that frame (spatial.h), and DIRECT channels, straight to the
 * ears (locked to the head: a game's music). Off (set_spatial(false)): the stream is pulled 2 wide, the game's
 * own stereo mix, and goes out as it is, bit for bit. Either way the player's own music (soundtrack.h) is added
 * last, locked to the head, while it plays; silent, it leaves the block alone.
 *
 * THE ORDER: open (the device paused, its rate known: vr_start's audio_hz) -> the game started -> start (the pull
 * and the speakers given, the device resumed) -> listener every update -> close (the device closed: the callback
 * can no longer run) before the game is stopped.
 *
 * THE AUDIO THREAD works in fixed blocks of kBlock frames (Steam Audio's frame size), however many SDL asks for;
 * a block more than it asked for waits in the stream for the next callback. It never waits on another thread:
 * the listener comes through a seqlock (the newest wins; a write caught half-done is retried, then the last good
 * one is kept), and the stats are counters. No allocation, lock or log call in it.
 *
 * Seat space: metres, x right, y up, -z ahead (the room's convention, rig.h's seat frame).
 */
#pragma once

#include <cstdint>

#include "../app/input.h"

namespace vrcore::audio {

constexpr int kBlock = 256;         // frames per block (Choice D; the device buffer is 1024, audio.cpp)
constexpr int kChannels = 2;        // the output's: stereo (and the plain pull's)
constexpr int kMaxIn = 8;           // the widest pulled stream

// A virtual speaker: where it is in seat space, and which channel of the pulled stream it plays.
struct Speaker {
    float pos[3] = {0, 0, 0};
    int channel = 0;
};

// The game's sound: `frames` frames of interleaved float, `channels` wide; returns the frames written. 2 wide it
// is the game's whole stereo mix.
using Pull = int (*)(float *buf, int frames, int channels);

// The pulled stream when spatial: how wide, and which channels go straight to the left and right ear (-1: none).
// The speakers name the rest.
struct Layout {
    int channels = kChannels;
    int direct[2] = {-1, -1};
};

// SDL's audio and the default playback device, opened paused; *hz its rate. False (logged): no sound.
bool open(int *hz);
// The pull and up to kMaxSpeakers speakers (copied); the device resumed. Spatial needs Steam Audio to start
// (logged if it cannot: the plain stereo then).
constexpr int kMaxSpeakers = 8;
bool start(Pull pull, const Layout &layout, const Speaker *speakers, int n);
// The head in seat space (any thread; one writer at a time). Never called: the head at the seat, looking ahead.
void listener(const Pose &head_in_seat);
// Spatial on (the default) or the plain stereo; before start, or any time after.
void set_spatial(bool on);
bool spatial();

struct Stats {
    int hz = 0;                     // the device's rate
    int device_frames = 0;          // the device's buffer, as SDL reports it
    uint64_t callbacks = 0;         // SDL's callbacks so far
    uint64_t blocks = 0;            // kBlock-frame blocks made
    uint64_t short_pulls = 0;       // pulls that gave fewer frames than asked (the rest silence)
    uint64_t spatial_blocks = 0;    // blocks spatialised since the last stats(true)
    double spatial_mean_us = 0;     // their time each, mean and max (us)
    double spatial_max_us = 0;
    bool spatial = false;           // spatialising now (on, and Steam Audio started)
};
// The counters; reset_window: the spatial mean / max start again after this read.
Stats stats(bool reset_window);

// (tests, --audio-test) Steam Audio and the pull with no device; test_block then makes one block as the callback
// does (spatial or not by set_spatial), for the head given. close() after.
bool test_start(int hz, Pull pull, const Layout &layout, const Speaker *speakers, int n);
const float *test_block(const Pose &head_in_seat);

// The device closed and Steam Audio released (idempotent).
void close();

}
