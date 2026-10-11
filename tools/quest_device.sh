# quest_device.sh -- sourced by the quest_*.sh scripts, not run: the adb they use, THE ONE HEADSET
# they talk to, and adb(). Two scripts on two headsets at once (one ANDROID_SERIAL each) never
# meet: every adb call, logcat stream and file is the script's own.
#   ADB             the adb to use (default: adb on the PATH, as its full path)
#   ANDROID_SERIAL  the headset. adb reads it itself, so no command carries -s. Needed when more
#                   than one is plugged in: quest_device refuses to go on without it, and when it
#                   names a serial that is not attached (or not authorised)
#   VR_DRY_RUN=1    adb() prints the command instead of running it
#   VR_DRY_DEVICES  a dry run only: the serials to pretend are plugged in, space separated
#                   (default: what `adb devices` says; none at all, and the checks are skipped:
#                   a dry run needs no headset)
# the adb's full path: the function below is named adb too, and a bare "adb" in ADB would call itself
ADB=${ADB:-$(command -v adb || true)}
if [ -z "$ADB" ] && [ -z "$VR_DRY_RUN" ]; then echo "adb not found: put it on the PATH, or set ADB" >&2; exit 1; fi
adb() { if [ -n "$VR_DRY_RUN" ]; then echo "+ adb $*" >&2; else "$ADB" "$@"; fi; }

# Checks the serial rules and leaves ANDROID_SERIAL exported: the one named, or the only headset
# plugged in (kept, so a second one plugged in during a long run does not confuse adb).
quest_device() {
    if [ -n "$VR_DRY_RUN" ] && [ "${VR_DRY_DEVICES+set}" = set ]; then
        _devs=$(for _s in $VR_DRY_DEVICES; do echo "$_s device"; done)
    else
        _devs=$("$ADB" devices 2>/dev/null | awk 'NR > 1 && NF >= 2 { print $1, $2 }')
    fi
    if [ -z "$_devs" ] && [ -n "$VR_DRY_RUN" ]; then export ANDROID_SERIAL; return 0; fi
    _n=$(printf '%s\n' "$_devs" | grep -c ' device$' || true)
    _list=$(printf '%s\n' "$_devs" | awk 'NF { printf " %s (%s)", $1, $2 }')
    if [ -n "$ANDROID_SERIAL" ]; then
        _state=$(printf '%s\n' "$_devs" | awk -v s="$ANDROID_SERIAL" '$1 == s { print $2 }')
        if [ -z "$_state" ]; then
            echo "ANDROID_SERIAL=$ANDROID_SERIAL is not attached; attached:${_list:- none}" >&2; exit 1
        elif [ "$_state" != device ]; then
            echo "ANDROID_SERIAL=$ANDROID_SERIAL is $_state: allow USB debugging in the headset" >&2; exit 1
        fi
    elif [ "$_n" -gt 1 ]; then
        echo "$_n headsets are plugged in and ANDROID_SERIAL is not set; attached:$_list" >&2
        echo "name one: ANDROID_SERIAL=<serial> $0" >&2; exit 1
    elif [ "$_n" -eq 0 ]; then
        echo "no device: plug the Quest in, allow USB debugging${_list:+ (adb sees:$_list)}" >&2; exit 1
    else
        ANDROID_SERIAL=$(printf '%s\n' "$_devs" | awk '$2 == "device" { print $1 }')
    fi
    export ANDROID_SERIAL
    echo "headset: $ANDROID_SERIAL" >&2
}
