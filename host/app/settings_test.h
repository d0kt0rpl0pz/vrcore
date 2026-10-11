/*
 * settings_test.h -- the settings file's device-free checks (--settings-test <dir>, Mac): load, the typed
 * round trips, kept comments and unknown keys, skipped bad lines, the atomic write, erase and upgrade. No window,
 * no plugin; the files are <dir>/settings.txt and its neighbours.
 */
#pragma once

namespace vrcore::settings {

// Runs every check; writes <dir>/settings_test.txt (one line per check: PASS / FAIL, the name) and logs the same
// with a total. 0 = all passed, 1 otherwise.
int settings_test(const char *dir);

}
