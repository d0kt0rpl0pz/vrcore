/*
 * picker_test.h -- M1's check of SDL's file dialog on Android (--pick-test): does it hand back a URI the
 * ROM importer could read? It opens the dialog with no filters (so .7z shows: Android has no MIME type for
 * it), and when the user picks a file logs the URI, the thread the callback ran on, the size, the first
 * four bytes and a read from the end. The answer goes into notes/native-host-plan.md's Progress.
 */
#pragma once

namespace vrcore {

void picker_test_start();       // on SDL_main's thread
void picker_test_poll();        // every loop pass, on SDL_main's thread: opens the pick once it is in

}
