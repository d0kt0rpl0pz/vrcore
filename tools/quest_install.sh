#!/bin/sh
# quest_install.sh <apk> <package> [rom_zip] [log_tag ...] -- a game's native VR build on the Quest:
# install it, give the app its ROM zip (pushed once to Android/data/<package>/roms/, where the
# app unpacks it on first start -- ROMs stay on the headset, never in the APK or git), start
# it, and show its log.
#   log_tag ...   logcat filter specs (default: vrcore:I, the host's tag)
#   VR_ACTIVITY   the app's activity (default: vrcore's, com.doktorplopz.letsgoaway.host.LetsGoAwayActivity)
#   ANDROID_SERIAL  the headset; needed when more than one is plugged in (see quest_device.sh)
#   ADB           the adb to use (default: adb on the PATH)
#   VR_DRY_RUN=1  print the adb commands instead of running them (no headset needed)
#   VR_NO_LOG=1   return once the app is started instead of following its log (an editor's push)
# The paths are taken from the current folder. A game's wrapper passes its own defaults.
set -e
case "$1" in -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;; esac
[ $# -ge 2 ] || { echo "usage: $0 <apk> <package> [rom_zip] [log_tag ...]  (--help)" >&2; exit 2; }
. "$(dirname "$0")/quest_device.sh"
APK=$1; PKG=$2; ZIP=${3:-}
[ $# -ge 3 ] && shift 3 || shift $#
[ $# -gt 0 ] || set -- vrcore:I
ACT=${VR_ACTIVITY:-com.doktorplopz.letsgoaway.host.LetsGoAwayActivity}
# a question to the headset: in a dry run the answer is no
ask() { if [ -n "$VR_DRY_RUN" ]; then echo "+ adb $* (dry run: no)" >&2; return 1; fi; "$ADB" "$@" >/dev/null 2>&1; }
quest_device
[ -n "$VR_DRY_RUN" ] || [ -f "$APK" ] || { echo "no $APK"; exit 1; }
adb install -r "$APK"
if [ -n "$ZIP" ]; then
    [ -n "$VR_DRY_RUN" ] || [ -f "$ZIP" ] || { echo "no $ZIP"; exit 1; }
    adb shell mkdir -p "/sdcard/Android/data/$PKG/roms"
    ask shell ls "/sdcard/Android/data/$PKG/roms/$(basename "$ZIP")" || adb push "$ZIP" "/sdcard/Android/data/$PKG/roms/"
    # made by adb before the app's first start, roms/ is the shell's (mode 2770) and the app
    # cannot read it ("no ROM set", measured on a Quest 3, 2026-10-01): open it to the app
    adb shell chmod 777 "/sdcard/Android/data/$PKG/roms"
    adb shell chmod 666 "/sdcard/Android/data/$PKG/roms/$(basename "$ZIP")"
fi
adb logcat -c
adb shell am start -n "$PKG/$ACT" >/dev/null
[ -z "$VR_NO_LOG" ] || { echo "started $PKG"; exit 0; }
echo "started; the game's log (Ctrl-C to stop):"
adb logcat -s "$@"
