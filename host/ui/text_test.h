/*
 * text_test.h -- the wording's device-free checks (--text-test <dir>, Mac): fmt()'s slots, literal braces, missing
 * and unused slots (shown and logged once), lone braces, and a constant from the generated ui/textstrings.h. No
 * window, no plugin.
 */
#pragma once

namespace vrcore::ui {

// Runs every check; writes <dir>/text_test.txt (one line per check: PASS / FAIL, the name) and logs the same with
// a total. 0 = all passed, 1 otherwise.
int text_test(const char *dir);

}
