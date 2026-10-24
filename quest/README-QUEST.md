# Wing Commander II VR (Meta Quest 3 and PICO) — 1.0.1

Standalone Quest build of the wc2-re reconstruction (SDL2 port of *Wing
Commander II: Vengeance of the Kilrathi*).  Space flight is in stereo 3D with
head-look: ships are placed at their real depth, stars sit at infinity, and the
original cockpit art is wrapped onto a 3D cockpit shell around you.  Menus,
briefings and cutscenes appear on a big floating screen with a laser pointer,
there is a VR keyboard for names, and the Touch controllers are the flight
stick.

The game data is **not** included; you need your own copy of the game.  Tested
with the DOS CD-ROM edition (1995, *Wing Commander II* with *Special
Operations 1 and 2*).  wc2-re plays that data with its AdLib music and
synthesized sound effects; *Kilrathi Saga* data should also work.

## Install

1. Install the APK (SideQuest, or):
   `adb install -r WingCommanderIIVR-1.0.1.apk`
2. Copy the game data: the contents of the `WING2` folder of the CD (or of your
   installed game folder: `WC2.EXE` and the whole `GAMEDAT` folder) go to
   `/sdcard/WingCommander2` on the headset:
   ```
   adb shell mkdir -p /sdcard/WingCommander2/GAMEDAT
   adb push WC2.EXE /sdcard/WingCommander2/
   adb push GAMEDAT/. /sdcard/WingCommander2/GAMEDAT/
   ```
3. Start *Wing Commander II VR* from Library → Unknown Sources. On the very
   first start, allow "Access to all files" when asked (or:
   `adb shell appops set com.bilko.wc2vr MANAGE_EXTERNAL_STORAGE allow`).
   A red screen means the game data was not found.

### PICO (untested)

The same APK also runs on PICO 4 / Neo 3 (PICO OS 5.9.0 or
newer): the PICO controllers use the same button layout as the Touch
controllers below.  PICO support is new and not tested on PICO hardware yet.
Install with `adb install -r WingCommanderIIVR-1.0.1.apk` (or the PICO file manager), copy
the game data to `/sdcard/WingCommander2` the same way, and allow storage access when
the app asks on the first start (it restarts by itself afterwards).

Saved games and pilot rosters are written to `/sdcard/WingCommander2/GAMEDAT`
as in the DOS game.

## Controls

**Menus, rooms and briefings** (laser beam from the controller):

| Control | Action |
| --- | --- |
| Aim + trigger | Left click (either hand; the hand that pulls last aims) |
| Grip | Right click |
| A | Enter |
| B or Menu | Escape |
| Y / X | "Y" / "N" (Yes/No in "Are you sure?" questions, then A to confirm) |
| Hold left stick click 1 s | Recentre the screen in front of you |

Text prompts (name, callsign, saved games) open a keyboard below the screen:
point and pull the trigger. Names are entered in capitals, as in the original.

**Flying:**

| Control | Action |
| --- | --- |
| Left stick | Pitch / yaw |
| Right stick left/right | Roll |
| Right stick up/down | Faster / slower |
| Right trigger | Guns |
| Left trigger | Fire missile |
| Right grip (hold) | Afterburner |
| A | Cycle target |
| B | Select missile |
| X | Select guns |
| Y | Pause |
| Menu | Escape |
| Left stick click | Navigation map |
| Right stick click | Autopilot |
| Left grip + A | Target lock |
| Left grip + B | Stop |
| Left grip + X | Communications (then left grip + Y/B to choose, A to send, X to close) |
| Left grip + Y | Full speed |

## Building

```
quest/tools/fetch_deps.sh                  # SDL2 2.32.10 source + Java glue
cd quest/android && ./gradlew assembleDebug
```
Needs the Android SDK (platform 34, build-tools 34, CMake 3.22.1), NDK
25.1.8937393 and JDK 17. Output: `app/build/outputs/apk/debug/app-debug.apk`.

`quest/tools/quest_run.sh` builds, installs, launches and pulls a headset
screenshot; `quest/tools/quest_cmd.sh` drives the game headless through the
developer command file `/sdcard/WingCommander2/vrcmd` (`click X Y`, `move X Y`,
`key NAME`, `type TEXT`, `fake FIELD VALUE MS`, `wait`).

## Code map

* `src/quest/quest_xr.c` — OpenXR thread: EGL context, session, virtual
  screen quad, keyboard panel, laser beam, Touch sampling, haptics, recentre.
* `src/quest/quest_input.c` — game-side glue: data directory, logcat, virtual
  SDL gamepad for the port's 4button-4axis joystick mode, laser → mouse,
  keyboard → key events.
* `src/quest/quest_stereo.c` — captures each space frame: objects become 3D
  billboards, the cockpit/HUD frame is split into panels.
* `#ifdef WC2_QUEST` hooks, applied by `quest/tools/apply_quest_hooks.py`, in
  `src/sdl/{video,events,joystick,launcher,compat}.c`, `src/eventmgr.c`
  (3D objects skip the software draw) and `src/geom.c` (`ReadTextInput`
  shows the keyboard).
* `quest/CMakeLists.txt`, `quest/android/` — NDK/Gradle build.

## Stereo space

In flight, space is rendered per eye: ships, missiles and dust are billboards at
their real 3D position, stars sit on a fixed sphere at infinity, and you can
look around with your head.  The cockpit art is a panel 0.9 m in front of you;
HUD pixels in the canopy window (reticle, target brackets) are on a panel at
8 m.  Tuning goes in `/sdcard/WingCommander2/vr.cfg` (`world_scale=`,
`dash_distance=`, `hud_distance=`, `stretch=`, `stereo=0` to disable).

**3D cockpit:** the 2D cockpit art is projected from the pilot's eye onto a
curved shell, so it matches the original pixel for pixel from the default
head position. The console tilts down toward your lap (about 0.75 m to
0.45 m away), the canopy frame arches away overhead (about 1 m), and the sides
curve in. Where the window's bottom edge sits in each column decides what is
console and what is canopy, so this works for every ship's cockpit. Outside the
original art, the edges wrap around you and darken. `cockpit3d=0` brings back
the flat panel; `cockpit_wrap=0` disables the wrap-around. `dash_distance=`
scales the whole shell.

## Flight stick tuning

Pitch, yaw and roll go through a dead zone and an expo curve, for precise aim
near the centre while full deflection keeps the full turn rate. Defaults are
in `quest/vr.cfg.example` (copied to `/sdcard/WingCommander2/vr.cfg`):
`stick_sensitivity=1.0`, `stick_expo=0.65`, `stick_deadzone=0.10`.
