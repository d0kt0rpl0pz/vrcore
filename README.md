# vrcore

Vrcore is a simple shared VR library for arcade game ports & decomps.

A decomp can be compiled as a native plugin exporting a small C interface. vrcore's host loads that plugin and puts
the game in the headset: C++ on Vulkan and OpenXR, with SDL3 for windows, input and sound and Steam Audio for the
speakers. It matches the headset's display, and it provides the rig, menus, controls, wheel, rumble and graphics
settings around them. A game adds its own front: its controls, menus and how its picture is drawn.

```
game code ──> plugin (exports vr_*) ──> vrcore host (host/) ──> headset
              + vrcore native/          + the game's front (vrcore_add_app)
```

## Status

- ABI version 1
- Quest APK (arm64-v8a, NDK r27d, Gradle) and a Mac sim (MoltenVK) that runs the same host in a window.
- Tested on Quest 3 and Quest 2, auto-detects hardware to set graphics profiles. Should work on other OpenXR
  headsets, though it makes a few Meta-specific calls (refresh rate, foveation, sharpening) where they are offered.
  I don't know if this will function on Steam Frame, it's not like I can afford one lol.

## Layout

```
abi/vrcore.h   the C interface every game plugin exports (vr_*), and the vertex vr_vert
native/        C99 shared by the plugins and compiled into each one (also builds as C++)
host/          the host: app (loop, plugin, pacing, settings, layers), xr, gpu, audio, rig, ui (menus), sim
shaders/       the host's GLSL (compiled to SPIR-V by glslc at build time)
android/       the Gradle project for the APK (SDL's activity, the file picker)
assets/        the hand wheel's model and the licences packed into the APK
tools/         vr_plugin_test.c (headless plugin test), quest_install.sh / quest_bench.sh / quest_profile.py
```

**`abi/vrcore.h`** is the contract between a game's plugin and the host. The game runs on its own thread, paced to
its board's rate. The host's render loop takes the newest frame, and its audio thread pulls sound. Each vertex
carries its position one game frame earlier, so the host can interpolate between game frames and we can hit that
juicy 120fps given enough graphics optimization. We're a frame or two behind, but it sure beats decoupling the game
from the frame rate in terms of effort. Actions 0-15, option keys below 100 and the `VR_V_*` vertex flags are
shared. Everything above those ranges belongs to the game.

**`native/`** holds board-neutral algorithms that a game feeds through its own adapter: `vr_interp` (matching
objects from frame to frame for drawing between game frames) and `vr_level` (taking roll and/or pitch out of the
camera for comfort and so you don't throw up).

**`host/`** is game-agnostic; a game's front fills its hooks. `app/plugin.h` (plugin loader), `app/pacing.h`
(render clock), `rig/` (seat, free camera, wheel, stick, shifter, rumble), `ui/menu.h` (menus, the file picker),
`xr/` (OpenXR session, refresh, foveation and resolution), `audio/` (sound and spatial speakers).

## Using it

A game uses it in two places:

- **The plugin:** the game's CMake compiles `native/*.c` into its plugin and includes `abi/`, finding vrcore through
  `VRCORE_DIR`.
- **The front:** a folder whose `CMakeLists.txt` calls `vrcore_add_app()`, passed to this project as
  `-DVRCORE_FRONT_DIR=<folder>`. On the Mac it builds the sim; for the APK it is `libmain.so`. The head of
  `CMakeLists.txt` has the commands.

A change to `abi/vrcore.h` must be made in `host/app/plugin.h` and `tools/vr_plugin_test.c` at the same time,
and every game has to be rebuilt afterwards. `VR_ABI_VERSION` is bumped only when an existing call or float offset
changes.

## Checks

```bash
# native: C99 and C++17, 0 warnings expected
cc  -std=c99   -Wall -Wextra -fsyntax-only native/*.c
for f in native/*.c; do c++ -std=c++17 -x c++ -Wall -Wextra -fsyntax-only "$f"; done

# headless plugin test: two passes (flat picture, then 3D frames); exit 0 = clean run
cc -std=c99 -Wall -Wextra -o vr_plugin_test tools/vr_plugin_test.c -lpthread -lm
./vr_plugin_test <plugin.bundle | libplugin.so> <rom_dir> [seconds] [shots_dir]
```

The comment at the head of `tools/vr_plugin_test.c` documents every `VR_TEST_*` setting. A refactor has to leave the
plugin's output unchanged: run with `VR_TEST_HASH=<prefix>` and `VR_TEST_ARGS="--replay f.rec"` before and after the
change, then compare the hash files.

The Quest scripts print their usage with `--help`. Set `VR_DRY_RUN=1` to print the adb commands without a headset.

## License

MIT (see `LICENSE`), except the third-party assets listed under Credits, which keep their own licenses.

## Credits

The steering wheel model (`assets/wheel.glb`, drawn by `host/rig/wheel_model.cpp`):

"MOMO STEERING WHEEL" (https://skfb.ly/o9MyX) by ijiklvn is licensed under Creative Commons Attribution
(http://creativecommons.org/licenses/by/4.0/).
