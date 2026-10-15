/*
 * soundtrack_test.h -- the soundtrack's device-free checks (--soundtrack-test <dir>, Mac): the folder scan and its
 * playlists, the decoders, the ring under a slow reader, the shuffle order, and the game's music volume going to 0
 * and back. No window, no plugin, no sound device: a thread of the test plays the audio callback's part.
 */
#pragma once

namespace vrcore::ui {

// Makes its own songs under <dir>/Soundtracks (tones, encoded with afconvert), runs every check, writes
// <dir>/soundtrack_test.txt (one line a check: PASS / FAIL) and logs the same with a total. 0 = all passed.
int soundtrack_test(const char *dir);

}
