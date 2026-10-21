#!/bin/bash
# Send developer commands to the running game and grab a headset screenshot.
#   quest/tools/quest_cmd.sh 'key escape\nclick 160 100' [wait-seconds]
ADB=${ADB:-$HOME/workspace/android-sdk/platform-tools/adb}
OUT=${OUT:-$HOME/.hermes/cache/scratch/wc2q}
N=${3:-shot}
printf "$1\n" > "$OUT/vrcmd"
timeout 10 $ADB push "$OUT/vrcmd" /sdcard/WingCommander2/vrcmd > /dev/null
sleep "${2:-3}"
timeout 10 $ADB shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT > /dev/null
sleep 3
SHOT=$(timeout 10 $ADB shell ls -t /sdcard/Oculus/Screenshots | head -1 | tr -d '\r')
timeout 30 $ADB pull "/sdcard/Oculus/Screenshots/$SHOT" "$OUT/$N.jpg" > /dev/null && echo "$OUT/$N.jpg ($SHOT)"
