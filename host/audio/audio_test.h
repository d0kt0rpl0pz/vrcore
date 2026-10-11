/*
 * audio_test.h -- the sound's device-free checks (--audio-test <dir>, Mac): a made-up signal through the
 * callback's block path and Steam Audio at set head poses. No device, no plugin, no sound out.
 */
#pragma once

namespace vrcore::audio {

// Runs every check; writes <dir>/audio_test.txt (one line per check: PASS / FAIL, the name and values) and the
// renders as WAVs, and logs a summary. 0 = all passed, 1 otherwise.
int audio_test(const char *dir);

}
