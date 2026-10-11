/*
 * log.h -- the host's log: logcat tag "vrcore" on Android, stdout on desktop.
 */
#pragma once

namespace vrcore {

#if defined(__GNUC__) || defined(__clang__)
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void log(const char *fmt, ...);
#endif

}
