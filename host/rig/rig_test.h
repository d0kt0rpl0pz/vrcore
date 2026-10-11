/*
 * rig_test.h -- the rig's device-free checks (--rig-test <dir>, Mac): the hand wheel's steering maths, the stick's
 * chain, the shifter's gates, the rumble's strengths. No window, no plugin.
 */
#pragma once

namespace vrcore {
namespace rig {

// Runs every check; writes <dir>/rig_test.txt (one line per check: PASS / FAIL, the name and values) and logs a
// summary. 0 = all passed, 1 otherwise.
int rig_test(const char *dir);

}
}
