/*
 * soundtrack.h -- the player's own music, played by the host in place of a game's: a list of files, decoded on
 * a streaming thread of its own (decode.h) and resampled to the device's rate into a lock-free ring, which the
 * audio thread reads and adds to the game's sound after the pull (audio.cpp), as stereo locked to the head, in
 * both the spatial and the plain stereo mix. Off (nothing playing, the last fade done) the game's sound goes out
 * untouched.
 *
 * THREADS. The calls below are the main thread's (one caller), except mix, the audio thread's. The streaming
 * thread starts with the first play and stops at shutdown. The audio thread never waits on it: the ring is single
 * producer, single consumer, on atomic indices; the commands and the volume reach it as atomics. mix allocates,
 * locks and logs nothing.
 *
 * SKIPS. A play, next or stop fades what is playing out over kFadeMs (no click), then the ring's old samples are
 * dropped and the new track fades in. A track that ends moves on to the next with no fade, the list wrapping
 * round. A file that cannot be decoded is skipped (failure() says why, and the log); when none of the list can be,
 * playing stops.
 *
 * This is the engine only: where the list comes from, the order, the menu and what is saved are its callers'.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrcore::audio::soundtrack {

constexpr int kFadeMs = 20;

// The device's rate (audio.cpp: open, test_start); the streaming thread restarts for a new one.
void set_rate(int hz);

// The tracks (paths); playing stops. Failures are forgotten. Returns the list's number (Status::list).
uint32_t set_tracks(std::vector<std::string> paths);
// The tracks replaced with nothing stopped (the folder scanned again): the file playing goes on, renumbered by its
// place in `paths`; one no longer listed plays to its end, then the track after the one heard before it. Failures
// are forgotten. Returns the list's number.
uint32_t retarget(std::vector<std::string> paths);
// Track i from its start; false when i is not in the list or there is no device rate.
bool play(int i);
// The track after the current one (the list wraps round); nothing when stopped.
void next();
void stop();
// The soundtrack's own volume and the master over it (VR_OPT_VOLUME's value), 0..100 each.
void set_volume(int volume, int master);

struct Status {
    int track = -1;                 // the track heard now (-1: none, or stopped), in list `list`
    uint32_t list = 0;              // the set_tracks / retarget list the streaming thread has taken (0: none yet)
    bool playing = false;
    int tracks = 0;
    int failed = 0;                 // tracks skipped as undecodable
    uint64_t frames = 0;            // frames mixed (the audio thread's)
    uint64_t dry_frames = 0;        // frames mixed while playing with the ring empty (silence)
    uint64_t decoded = 0;           // frames into the ring (the streaming thread's)
};
Status status();
// Why track i was skipped ("": it was not).
std::string failure(int i);

// (audio thread) Adds `frames` stereo frames of the soundtrack to `out`, clamped to +-1; false (out untouched)
// when it is silent: stopped and faded out.
bool mix(float *out, int frames);

// The streaming thread stopped and the ring emptied. Only once the audio thread can no longer call mix
// (audio::close).
void shutdown();

}
