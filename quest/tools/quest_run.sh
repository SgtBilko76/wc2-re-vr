#!/bin/bash
# Build, install and launch Wing Commander II VR on a connected Quest, record the
# log and take a headset screenshot.
#   quest/tools/quest_run.sh [seconds] [--no-build]
set -u
SECS=${1:-40}
NOBUILD=0
for a in "$@"; do [ "$a" = "--no-build" ] && NOBUILD=1; done
HERE=$(cd "$(dirname "$0")/.." && pwd)
ADB=${ADB:-$HOME/workspace/android-sdk/platform-tools/adb}
PKG=com.bilko.wc2vr
OUT=${OUT:-$HOME/.hermes/cache/scratch/wc2q}
mkdir -p "$OUT"
export JAVA_HOME=${JAVA_HOME:-$HOME/.local/jdk-17.0.20.1+1}

if [ $NOBUILD = 0 ]; then
    (cd "$HERE/android" && ./gradlew assembleDebug --no-daemon -q > "$OUT/gradle.log" 2>&1) ||
        { grep -E 'error|FAILED' "$OUT/gradle.log" | head -20; exit 1; }
fi
APK="$HERE/android/app/build/outputs/apk/debug/app-debug.apk"

top=$(timeout 10 $ADB shell dumpsys activity activities | grep -m1 topResumedActivity)
echo "foreground: $top"

timeout 120 $ADB install -r "$APK" | tail -1
timeout 10 $ADB shell appops set $PKG MANAGE_EXTERNAL_STORAGE allow
timeout 10 $ADB shell 'setprop debug.oculus.guardian_pause 1; am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null; input keyevent KEYCODE_WAKEUP; svc power stayon true'
timeout 10 $ADB shell am force-stop $PKG
timeout 5 $ADB logcat -G 16M
timeout 5 $ADB logcat -c
timeout $((SECS + 10)) $ADB logcat -v time WC2:V WC2VR:V SDL:V SDL/APP:V DEBUG:V AndroidRuntime:E 'libc:F' '*:F' > "$OUT/run.log" 2>&1 &
sleep 1
timeout 15 $ADB shell am start -n $PKG/.WC2Activity > /dev/null
sleep "$SECS"
echo "pid: $(timeout 5 $ADB shell pidof $PKG)"
timeout 10 $ADB shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT > /dev/null
sleep 4
SHOT=$(timeout 10 $ADB shell ls -t /sdcard/Oculus/Screenshots | head -1 | tr -d '\r')
timeout 30 $ADB pull "/sdcard/Oculus/Screenshots/$SHOT" "$OUT/shot.jpg" > /dev/null && echo "screenshot: $OUT/shot.jpg ($SHOT)"
sleep 6
grep -v 'V/SDL ' "$OUT/run.log" | tail -40
