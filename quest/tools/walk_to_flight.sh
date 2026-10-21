#!/bin/bash
# Headless walk: intro -> new campaign -> name/callsign -> high scores -> rec
# room.  Leaves the game in the rec room for further commands.
#   quest/tools/walk_to_recroom.sh   (game must already be running)
ADB=${ADB:-$HOME/workspace/android-sdk/platform-tools/adb}
T=$(dirname "$0")
wait_log() { # pattern timeout
    for _ in $(seq 1 "$2"); do
        timeout 5 $ADB logcat -d -v time WC2:V '*:S' | grep -q "$1" && return 0
        sleep 1
    done
    echo "timeout waiting for: $1"; return 1
}
$T/quest_cmd.sh 'key return' 8 w0 > /dev/null
$T/quest_cmd.sh 'move 160 66\nclick 160 66' 1 w1 > /dev/null
wait_log 'text input on' 90 || exit 1
sleep 1
$T/quest_cmd.sh 'key backspace\nkey backspace\nkey backspace\nkey backspace\nkey backspace\ntype tester bob\nwait\nwait\nwait\nwait\nwait\nkey return\nwait\nwait\nkey return' 6 w2 > /dev/null
$T/quest_cmd.sh 'move 160 100\nclick 160 100' 14 recroom | tail -1
