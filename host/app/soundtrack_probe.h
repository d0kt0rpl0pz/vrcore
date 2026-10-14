/*
 * soundtrack_probe.h -- the soundtrack library's check on the headset and in the sim (--soundtrack-probe), before
 * its menu page exists: it drives library.h as the page will, logging each step. It scans; when the shared folder
 * cannot be read it asks for the permission, and when that is refused opens the folder picker; once a scan has
 * tracks it plays the first playlist through the soundtrack's ring (soundtrack.h) and logs what is heard every
 * 5 s. A game must be running (its sound opens the device).
 */
#pragma once

namespace vrcore {

void soundtrack_probe_poll(double now);    // every loop pass, on the main thread, once the first frame is shown

}
