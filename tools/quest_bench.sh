#!/bin/sh
# quest_bench.sh <plugin.so> <vr_plugin_test> <rom_dir> [seconds] -- THE QUEST CPU CHECK: a game's
# plugin built for Android, run on the headset over adb with no host by the headless test
# (vr_plugin_test.c, built for Android by the game's CMake) for `seconds` a pass (default 60).
# Needs the headset on USB with developer mode. The ROMs go to the headset's
# <remote>/<rom_dir's name>, never into git or an APK; pushed again only when files are missing.
#   VR_REMOTE     the folder on the headset (default /data/local/tmp/<plugin name>)
#   VR_REC        a recording, pushed as <remote>/race.rec (the game's VR_ARGS replay it)
#   VR_ARGS       the game's options for the test (its VR_TEST_ARGS), e.g. "--replay race.rec"
#   VR_ENV        more environment for the test on the headset, e.g. "RR_U_STDERR=1"
#   VR_GREP       more of the test's lines to show (an extended regex; always: pass lines, OK, FAIL)
#   ANDROID_SERIAL  the headset; needed when more than one is plugged in (see quest_device.sh)
#   ADB           the adb to use (default: adb on the PATH)
#   VR_DRY_RUN=1  print the adb commands instead of running them (no headset needed)
# The paths are taken from the current folder. A game's wrapper builds the plugin and passes
# its own defaults.
set -e
case "$1" in -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;; esac
[ $# -ge 3 ] || { echo "usage: $0 <plugin.so> <vr_plugin_test> <rom_dir> [seconds]  (--help)" >&2; exit 2; }
. "$(dirname "$0")/quest_device.sh"
SO=$1; TEST=$2; ROMS=${3%/}; SECS=${4:-60}
NAME=$(basename "$SO" .so); NAME=${NAME#lib}
REMOTE=${VR_REMOTE:-/data/local/tmp/$NAME}
RDIR=$(basename "$ROMS")
quest_device
for f in "$SO" "$TEST"; do [ -n "$VR_DRY_RUN" ] || [ -f "$f" ] || { echo "no $f"; exit 1; }; done
[ -d "$ROMS" ] || { echo "no ROM folder $ROMS"; exit 1; }
adb shell mkdir -p "$REMOTE/$RDIR"
if [ -n "$VR_DRY_RUN" ] || [ "$("$ADB" shell ls "$REMOTE/$RDIR" | wc -l)" -lt "$(ls "$ROMS" | wc -l)" ]; then
    echo "pushing the ROMs ..."; adb push "$ROMS/." "$REMOTE/$RDIR/" >/dev/null
fi
adb push "$SO" "$TEST" "$REMOTE/" >/dev/null
[ -n "$VR_REC" ] && [ -f "$VR_REC" ] && adb push "$VR_REC" "$REMOTE/race.rec" >/dev/null
adb shell getprop ro.product.model
adb shell "cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq 2>/dev/null | sort -u | tr '\n' ' '"; echo
T=$(basename "$TEST"); P=$(basename "$SO")
PAT="pass [12]:|^OK|FAIL${VR_GREP:+|$VR_GREP}"
RUN="cd $REMOTE && chmod +x $T && TMPDIR=$REMOTE $VR_ENV VR_TEST_ARGS='$VR_ARGS' ./$T ./$P $RDIR $SECS"
if [ -n "$VR_DRY_RUN" ]; then adb shell "$RUN"; echo "+ (its output filtered by: $PAT)"; exit 0; fi
adb shell "$RUN" 2>&1 | grep -E "$PAT"
