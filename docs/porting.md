# Bringing a game to vrcore

This is how a game ends up in the headset on vrcore: what the game has to provide, what the host already does, how
the two connect, and how it is built. It assumes a decomp or recomp that already runs the game's logic and draws
its picture as C or C++ code you can compile. The interface itself is `abi/vrcore.h`; this page explains how the
pieces around it fit together.

## The three pieces

```
  the game's repo                                     vrcore (the clone beside it)
  ┌───────────────────────────────┐                  ┌──────────────────────────────────┐
  │ PLUGIN  libgame_plugin.so     │   vr_* (C ABI)   │ HOST  host/                      │
  │  game code + vrcore native/   │ <─────────────── │  OpenXR session, Vulkan, SDL3,   │
  │  its own thread, board rate   │   dlopen, bind   │  pacing, rig, menus, audio, ROMs │
  └───────────────────────────────┘    by name       │                                  │
  ┌───────────────────────────────┐                  │                                  │
  │ FRONT  folder with CMakeLists │  AppInfo hooks   │                                  │
  │  vrcore_add_app() -> libmain  │ ───────────────> │                                  │
  │  shaders, menus, controls     │  + host/ APIs    │                                  │
  └───────────────────────────────┘                  └──────────────────────────────────┘
```

- **The plugin** is the game: its emulated or recompiled board and its game logic, built as a shared library
  that exports the `vr_*` functions in `abi/vrcore.h`. It runs the game on its own thread at the board's rate,
  and hands out frames, sound and state when asked. It knows nothing about VR, Vulkan or the headset.
- **The front** is the game's C++ code inside the host. It opens the plugin, turns the plugin's frames into
  draws with the game's own shaders, maps the controllers onto the game's controls, and builds the game's menu
  pages. It is a folder whose `CMakeLists.txt` calls `vrcore_add_app()`.
- **The host** is vrcore. It provides the OpenXR session (or the Mac sim), the Vulkan renderer, frame pacing, the
  seat and free camera, the hand wheel, stick shaping, the shifter, rumble, the menu framework, graphics profiles,
  spatial audio, the ROM checks and the file picker, settings, and the self test and soak test drivers. All of
  it is game-agnostic. Anything that only makes sense for one board belongs in that game's plugin or front.

The plugin and the front are kept separate on purpose. The plugin is plain C that can also be driven headless
(`tools/vr_plugin_test.c`) and tested on its own, and the front can be rebuilt without touching the game code.

## The plugin contract

Everything below is declared in `abi/vrcore.h`, and the host binds it in `host/app/plugin.h`. The plugin is
opened with `dlopen` and every function is looked up by name, so a front can also bind extra functions of its own
(`Plugin::bind`).

### Lifetime

1. **Identity.** `vr_abi_version` (must equal the host's `VR_ABI_VERSION`, or the plugin is refused), `vr_game`
   (a short name) and `vr_vertex_floats` (floats per vertex: 21, or more if the board appends its own).
2. **Options before start.** `vr_set_option` may be called before `vr_start`. The values take effect as the game
   runs.
3. **`vr_start(rom_dir, audio_hz, args)`** starts the game thread. `rom_dir` is where the ROM set is, `audio_hz`
   is the host's output rate (0 means no sound), and `args` are the game's own command-line options. It returns
   0 on success, -1 if this loaded copy already ran, and -2 if the thread could not be made.
4. **Running.** `vr_state` goes `VR_IDLE -> VR_STARTING -> VR_RUNNING -> VR_STOPPED`. Once stopped,
   `vr_exit_code` says why. `vr_pause(1)` holds the game on its current frame while the host's menu is open.
5. **`vr_stop(timeout_ms)`** stops the thread. The host never unloads the plugin, because the game's threads may
   still be idling in it at exit.

**One run per loaded copy.** Most decomps keep the game's state in C globals that are set up once. The host
handles this by loading a fresh copy of the binary for each run: on the desktop it copies the binary to a temporary
folder, and on Android it copies the `.so` out of the APK into the app's files folder. A "restart game" from the
menu (`request_restart` in `host/app/app.h`) shuts the front down and starts it again with a new copy, in the same
process and XR session. A plugin only has to refuse a second `vr_start` in the same copy.

### Threads

- The **game thread** is the plugin's own. It runs the board at its real rate and publishes finished frames.
- The host's **main thread** takes the newest frame whenever it likes (`vr_frame`, `vr_picture`) and sets
  controls and options.
- The host's **audio thread** pulls sound (`vr_audio`). It must never block: return what is ready, and the host
  treats any frames not written as silence.
- Everything else may be called from either host thread, so the plugin has to make those calls thread-safe.

### Pictures

A plugin hands over pictures in one of two forms, and it says which with `vr_renderer`:

- **The flat picture** (`vr_picture`): the board's screen as RGBA32, bottom row first. This is the fallback, and
  the form every plugin should support first. A front can show it on a screen in the room before any 3D work
  exists.
- **3D frames** (`vr_frame`): the board's display list, with each vertex in the game camera's view space. This is
  what puts the player inside the world. `vr_set_world(1, keep_behind)` turns it on. Set `keep_behind` to 1 to
  keep geometry behind the camera, so the player can look round.

A 3D frame is a ring of recent game frames. The host asks for one by number (`want`; 0 means the newest), and
passes the one it already holds (`have`) so nothing is copied twice. A frame carries vertices, triangle indices and
the board's per-frame **tables** (`vr_table_count`/`vr_table_size`; a bit in `*present` says which tables came with
this frame). It also carries two timestamps on the plugin's clock: when its slot began and when it was published.
**Assets** (`vr_asset_count`/`vr_asset_size`/`vr_assets`) are what the board's shader needs once, such as the
texture ROMs, copied out when they are loaded. `vr_revision` changes when a paused frame is published again with
new contents, for example after an option changed.

### Controls, feedback and sound

- `vr_set_controls(held, steer_on, steer, gas_on, gas, brake_on, brake)`: a bit per held action, plus analogue
  steering (-32767..32767) and pedals (0..`vr_pedal_max`). The analogue values are only used when their `*_on`
  flag is set. Actions 0-15 are shared (coin, service, test, shift, view, steer, gas, brake), and 16-31 belong to
  the game (`VR_ACT_GAME`).
- `vr_feedback`: the cabinet's force feedback this frame, -1..1. The front sends it to `rig/rumble.h`.
- `vr_audio(buf, frames, channels)`: interleaved floats at `audio_hz`. The channel count is the front's layout. The
  host can place each channel as a speaker fixed to the seat (Steam Audio, `audio/spatial.h`), or play the
  game's own stereo mix untouched.

### Options and health

- `vr_set_option`/`vr_get_option`: keys below 100 are shared (free play, draw distance, volumes, level camera, HUD,
  explore, feedback), and game keys start at `VR_OPT_GAME` (100). Options are applied on the game thread at its next
  frame. `vr_get_option` returns -1 for a key the game doesn't have.
- `vr_stats(frame, c0, c1, c2)`: the game frame and three counters that must stay 0. The game decides what they
  count, for example traps, unmapped memory accesses or ROM writes. The self test and soak test fail on them.
- `vr_interp_stats`: totals from the frame matching below.

### Optional exports

A plugin may leave these out. The host binds what it finds, and leaving one out never needs a new ABI version.

- `vr_roms`: the files the ROM set needs, one per line (`name\tsize\tcrc32`). With it, the host checks the
  player's set, says exactly what is missing, and adds a "choose ROM file" row so the set can be picked on the
  headset with no cable (`app/roms.h`, `ui/pick.h`).

## Vertices and smooth motion

Boards run at their own rate, usually somewhere between 57 and 60 Hz, while a headset draws at 72-120 Hz. vrcore
does not decouple the game from its frame rate. It draws *between* game frames instead, one game frame behind:

- Every 3D vertex carries `prev`, where that corner was one game frame earlier. The front's shader draws
  `lerp(prev, pos, alpha)`.
- `host/app/pacing.h` picks, for each display frame, which game frame to draw and its `alpha`. It does this from
  the plugin's own clock (`vr_now_ns`, `vr_frame_ns`) and the frames' timestamps, and never extrapolates. A frame
  that is late holds at alpha 1.

**The vertex** (`vr_vert`, 21 floats) has a common part (position, flags, clip, prev) and a ten-float **board
block** (`b0`, `b1`, `b2`) that only the board's own shader reads: its texture coordinates, palette, fog, whatever
the hardware has. The split follows the shader attribute slots, so a mesh takes the vertices as they come. A board
that needs more appends floats after float 20 and reports the total in `vr_vertex_floats`. Flag bits 0-2 and 10 up
are the board's, and the `VR_V_*` bits are shared (3D, background quad, explore, sub-window).

**Filling in `prev`** is the plugin's job, and `native/` helps with it. A board's display list says nothing about
which object in this frame was which object in the last one, so `native/vr_interp` pairs them:

1. Call `vr_interp_begin()` before walking the frame's objects.
2. Call `vr_interp_add(obj)` for each object, with a key (group, world, id; only objects with the same key can
   match), its translation relative to the camera in world axes, and its rotations.
3. Call `vr_interp_end(snap)`. Pass `snap = 1` on a cut. The helper also snaps on its own when the camera jumps or
   too few objects match.
4. For each object, `vr_interp_prev(i)` gives its partner in the last frame (if any). Your adapter then places
   that object's corners again with *last frame's* transform, in the board's own arithmetic, and writes the result
   into `prev`.

Most of the work in a new port is this adapter: deciding what an "object" is on the board, and keeping each
object's previous transform. `VR_INTERP_LOG=1` and `VR_INTERP_DUMP=<frame>` show the matching.

`native/vr_level` takes the camera's roll and/or pitch out of what the player sees, for comfort (`VR_OPT_LEVEL`).
The board says where up points in its camera's view space, and turns its corners with the matrix it gets back.

Both helpers are C99 (they also build as C++) and are compiled into the plugin. They are not part of the host.

## The front

A front is a folder with a `CMakeLists.txt`:

```cmake
vrcore_add_app(MyGame SOURCES mygame.cpp world.cpp controls.cpp menu.cpp ...)
vrcore_add_shaders(MyGame mygame_world.vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mygame_world.vert)
```

`vrcore_add_app` adds the host's `main()` (`host/app/entry.cpp`) and links the host library. On the desktop the
result is an executable (the sim); on Android it is the APK's `libmain.so`. The front defines one function,
`vrcore::front_info()`, which returns an `AppInfo` (`host/app/app.h`) with its callbacks:

| Callback | When | What a front does there |
|---|---|---|
| `init(Renderer&, Options&)` | once | open the plugin, make pipelines and buffers, start the game (or leave that to the first update) |
| `update(Tick&)` | every display frame, before drawing | read input, drive the rig, menus, controls and rumble |
| `prepare(Frame&)` | every drawn frame, before the eye pass | copy the frame from the plugin, upload it, draw offscreen and layer pictures |
| `draw(Frame&)` | in the eye pass (multiview, both eyes) | draw the world with the board's shaders |
| `shutdown(Renderer&)` | at exit, also after a failed init | stop the game, free what init made |
| `option` | launch argument parsing | take the game's own command-line flags |
| `load_settings(Options&)` | before launch options | `settings::open(org, app)`, load the saved choices |
| `headset(Headset&, Options&)` | once the headset is known | apply this headset's graphics profile |

The fronts that exist so far all have the same shape, which is a good template for a new one:

- `<game>.cpp`: `front_info`, opening the plugin, the main update/prepare/draw.
- `world.cpp`: turns `vr_frame` output into draws (buffers, the board block's textures and tables, the pacing's
  alpha), placed in the room by `rig/rig.h`'s seat matrix.
- `controls.cpp`: maps `app/input.h`'s snapshot (Touch controllers, pads, keyboard) onto `vr_set_controls`, with
  `rig/wheel.h` (hands as a wheel), `rig/stick.h` (shaped thumbstick steering), `rig/shifter.h` and `rig/rumble.h`.
- `menu*.cpp`: the pages, built on `ui/menu.h`, with `ui/graphics.h` providing the headset's Graphics rows.
- `device_profiles.cpp`: the Performance and Fidelity values for each headset kind (`app/headset.h`).
- `quads.cpp`: screens and panels as compositor quad layers (`app/layers.h`), which cost the eye pass nothing.
- `selftest.cpp`, `soak.cpp`: fill `app/selftest.h` and `app/soak.h`'s hooks (how to read the game's state, how
  to drive it, what its counters are called).
- `shaders/`: the board's GLSL. Include `shaders/vrcore_views.glsl` and follow the contract at the head of
  `host/gpu/render.h` (descriptor sets, reverse-Z depth, linear colour).

What the host gives a front without any work: the seat and recentring, look-behind, the free camera and explore
mode, the menu panel and its navigation, the controller pointer in the menu (below), graphics profiles and the
refresh rate, eye resolution, foveation and sharpening, the "could not start" panel with the ROM picker, spatial
speakers, the settings file, and the self test and soak test.

### The menu

A front's pages are `std::vector<ui::Item>` (`ui/menu.h`), handed to `ui::Menu`. The menu owns the panel, the
selection, the keys, the pointer and the controllers. The front owns what the rows say and do.

**Row builders.** Each returns an `Item`; set `group` or `gap` (a new card starts here, below) and `help` (shown
while selected) on it and push it into a page:

| Builder | Row | D-pad / keys | Pointer |
|---|---|---|---|
| `toggle(label, get, set, on, off)` | On / Off | left / right or A flips | a click anywhere on the row flips it |
| `choice(label, labels, get, set)` | one of a list | left / right cycles | a click opens a dropdown under the value |
| `stepper(label, get, set, step, lo, hi, format, reset)` | a number | left / right steps, A resets | `<` `>` halves step, held repeats; a Reset button when `reset` is given |
| `action(label, fn)` | a button row | A runs it | a click runs it |
| `page(label, sub)` | a sub page | A enters | a click enters |
| `confirm(label, fn)` | No / Yes | right to Yes, right or A to the button, A runs | click the row to flip, click the "Confirm label" button |
| `Menu::return_item()` | back one page (the top page closes the menu) | A | a click is B |

Every builder fills the same `value` / `change` / `press` / `sub` fields an item always had, so the d-pad, stick and
keyboard take the same paths whatever the kind; the kind only adds what the pointer and the picture do (`Item::kind`,
`Kind`). **An item filled in by hand** (label, `value`, `change`) still works and looks as it did: `Kind::Legacy`,
no `<` `>`, and a pointer click is A. New `Item` fields are only ever added at the struct's end.

**Cards.** A page's rows are drawn in cards. A card starts at the page's first row and at every row with a `group`
or a `gap`. `Item::group` is the card's title strip (drawn upper case), and `Item::group_value`, when set, adds
` · value` after it in the heading colour; a `gap` without a group starts an untitled card. The Return row sits under
the cards. A card whose rows are all hidden is not drawn, but its room is kept, as hidden rows' is. `ui/graphics.h`'s
rows already start a `PROFILE · <profile>` card, so a front puts them on its Graphics page as they are.

**The theme.** `Hooks::theme` (`ui::Theme`, read once at `Menu::init`) is how a game makes the menu its own; every
field is optional and an empty one is vrcore's default:

```cpp
h.theme.label_font = "fonts/Oswald-Medium.ttf";        // labels, the header, card titles, Return
h.theme.label_bold_font = "fonts/Oswald-SemiBold.ttf";
h.theme.value_font = "fonts/SpaceMono-Regular.ttf";    // values, help, the hint; monospace
h.theme.value_bold_font = "fonts/SpaceMono-Bold.ttf";
h.theme.key = 0xFF3FA4;      // the selected row's bar, a hot arrow or button, a dropdown's edge, the ray over a control
h.theme.heading = 0x4FE3FF;  // the page title, card values, the CONTROLS heading, a toggle's squares
```

The defaults are Barlow Condensed and IBM Plex Mono, key `#FFC21A` and heading `#8CFFCC`. The rest of the palette
(the panel's ground, borders, row and help text) is vrcore's and is not themed. The selected row's text is black or
white, whichever reads better on the key colour, so any key colour works. The fonts a theme can name are the ones in
`assets/fonts/` (SIL OFL 1.1, packed into every APK; the sim reads them from `VRCORE_ASSETS_DIR`); a path is relative
to the assets. Name both weights of a family: a weight drawn is a file loaded, and a missing bold falls back to the
regular. A font that fails to load is logged (`ui: font ...`) and the default is used; if that fails too, ImGui's
built-in font, so the menu always draws. Arrows and triangles are drawn as shapes, so any font will do for them.
The value font must be monospace: `label_fn` rows (a scrolling window) are drawn in it so their width follows the
text's length.

**Sizes.** Every size in the panel is a multiple of one unit, a 38th of the row pitch (`kC` in `ui/menu.cpp`); a row
is 43.8 mm at the panel's 1.1 m. With the default fonts the capitals come to 14.8 mm for labels, 9.9 mm for values,
10.5 mm for help and 8.7 mm for the hint (from the fonts' metrics). Checked on a Quest 3 (2026-10-16): help and hint
were raised a step after reading a little hard, and frame time with the menu open was unchanged, the panel being a
quad layer. A wider font (a theme's)
makes the panel wider, never a row taller; a page that needs more than the 2048 px layer is logged
(`ui: a page needs ...`).

**The pointer.** While a controller is showing, the right hand's aim ray (the left's only while the right is not
tracked; there is no handedness setting) is cast at the panel and the boxes. Hovering a row selects it, the same
selection the d-pad moves, so whichever was used last wins and there is no mode switch. The trigger's press does
what A does on the hovered part of the row. The CONTROLS legend and a front's `Box`es stop the ray but are not
clickable, and nothing scrolls. A hover change gives a light haptic tick, a click a stronger one. The cursor dot and
the row's glow are drawn into the panel's own texture.

**The hide rule.** A controller (and its ray) shows only while the menu or the message panel is up, it is tracked,
and its last input (a button, the stick, trigger or grip, or more than about 2 cm of movement) is newer than the
last gamepad or key input (`Input::controller_shown`, `app/input.h`). Any pad or key press hides both and the menu
looks as it did before a pointer existed; picking a controller up brings them back. They never draw in the game.

**The hint line.** The controls line at the bottom of the panel is the host's: pointer wording ("point and pull the
trigger", "B back, menu close", leaving out B or menu where the controller's profile binds no such button) while a
controller shows, the d-pad wording otherwise. Both are counted when the panel is sized, so it does not change size
with the switch. A front's own CONTROLS legend is not touched.

**The controllers and the ray** are drawn by the menu itself (`ui/controllers.h`): a model for the
device in that hand, at each shown hand's grip: the interaction profile the runtime reports for the hand
(`controller_profile(hand)`, `app/input.h`) picks it (Touch, Touch Plus, Index, Vive, Pico 4, Pico Neo 3 under
`assets/controllers/`, ASTC textures beside each GLB), and any other profile, or none yet (the sim), gets the generic
trigger / squeeze / thumbstick pair (`assets/controller_left.glb`, `controller_right.glb`). The host suggests bindings
for each of those profiles; Pico's need `XR_BD_controller_interaction`. A ray from the pointing hand's aim, and a
thin ray from the pointing hand's aim to what it hits (2 m when it hits nothing, in the theme's key colour over something clickable).
A front draws nothing for them.

**`overlay_draw`** (`app/layers.h`) is the hook that makes this possible, and a front may use it too. The panel and
boxes are compositor quad layers, which sit over the eye pass, so a controller drawn there would vanish behind them.
`overlay_draw(fn)`, called in `prepare` on every frame it is wanted, runs `fn(Frame&)` in a second transparent
projection layer composited above all the quads (on the desktop sim, at the end of the eye pass after them). It uses
the eye pipelines and views, has its own depth, and costs nothing on frames it is not asked for.

**The message panel.** While `Hooks::message` returns a text (the game could not start), the panel shows it in
place of the menu, in the same style, with `Hooks::pick`'s lines under it (a line starting with `\x01` is the picker's
row, drawn on the key bar; `\x02` a note in a dimmer colour). A / Enter, or the trigger with the ray on the `\x01`
row, calls `pick(true)`. Its size only grows while it shows.

**What a front gets for free:** the look (header, cards, the selected bar, the controls' pictures, help and hint)
in its theme, the fonts, the pointer, the cursor, haptics, the hint line, the controller models and ray, the
hide rule, the dropdown, the message panel with its picker row, and sizing (the panel reserves room for the longest choice list,
the Confirm button and each row's reserved value, so it never grows while open). A front writes its pages and
nothing else. `host/test/menu_front.cpp` (the `vrcore_menu` target, desktop only, built beside `vrcore_scene`) is a game-free
page set using every kind; `--menu` opens it at the start and `--page NAME` on a sub page, for a headless `--shot`; in the sim the mouse is the right controller while the menu or the
message panel is up (left button the trigger, `P` pretends a gamepad was pressed); the game never sees it.

### The soundtrack page

The player's own music, played by the host in place of the game's, with a page to choose it (`ui/soundtrack_page.h`).
No plugin export or ABI call is involved: the host mixes it after the game's sound, locked to the head. A front adds
one row and three calls:

```cpp
ui::soundtrack_load(hooks);                 // AppInfo::load_settings, after settings::open
top.push_back(ui::soundtrack_item(menu));   // the "Soundtrack" row, where the front builds its top page
h.on_open = [] { ui::soundtrack_menu_opened(); };         // ui::Hooks: scan the folder again
ui::soundtrack_update(menu, tick, input);   // every update, after menu.update
```

`SoundtrackHooks::options_changed` is called when `ui::soundtrack_replacing()` flips; the front sends the game
`VR_OPT_MUSIC_VOL` 0 while it is true and the player's saved music volume otherwise (so the game only has to honour
that option). `master_volume` gives VR_OPT_VOLUME's value; the soundtrack sits under it.

The songs are in `Music/Soundtracks` in the headset's shared storage (`~/Music/Soundtracks` in the sim); each subfolder
is a playlist and loose files make "All tracks" (saved as an empty name, so a folder called that is its own
playlist). `.mp3 .m4a .aac .flac .ogg .wav`, decoded by the platform. The folder is scanned again each time the menu
opens; a change to the playing playlist's files goes on with the track heard, if it is still there. Access to the
folder is asked for only when its row ("Allow access to music") is pressed, never at launch; the page's alert lines
are `ui::note` rows, which the cursor and the pointer skip. The page's choices are saved per game under
`<prefix>soundtrack.*`.

`Item::label_fn` and `Item::edge` (`ui/menu.h`, A SCROLLING WINDOW) are what let a page show a window of a long
list: a few fixed rows whose text follows an offset, scrolled by up / down off the window's ends.

## Building

**Finding vrcore.** vrcore is its own repository, never copied into a game. A game keeps a small helper script
that prints the vrcore folder to build against: a clone of vrcore beside the game's own, as it stands (its latest
commit and any local changes), cloned when it is not there. Every build step asks the helper: the game's CMake, its
Android script and its Quest tools. Nothing fetches or checks anything out in that clone; keeping it current is
done by hand. Set `VRCORE_DIR` to build against another vrcore folder. A game may instead pin one full commit
hash in a file of its own and have the helper check that commit out as a worktree; none is required to.

**Libraries.** `tools/fetch_extern.py --extern <dir>` fetches the pinned third-party sources (`THIRD_PARTY.md`):
SDL 3, Dear ImGui, the OpenXR SDK, Vulkan-Headers, VMA, volk, cgltf and Steam Audio. A game's setup script calls it
and keeps no copy of the pins. CMake finds them through `VRCORE_EXTERN_DIR`.

**The plugin** is built by the game's own CMake. It compiles `native/*.c` into the plugin and puts `abi/` on the
include path. `tools/vr_plugin_test.c` is usually built beside it as a test target.

**The Mac sim:**

```bash
cmake -S <vrcore> -B build-native -G Ninja -DVRCORE_FRONT_DIR=<the front> -DVRCORE_EXTERN_DIR=<extern>
cmake --build build-native
```

It runs the same host and front in a window on MoltenVK, with keyboard and gamepad standing in for the
controllers. The sim reports no interaction profile, so the menu draws the generic controller pair; **F5 / F6**
step the family shown on the left / right hand through the table (each press logs the profile and the file), and
`VR_SIM_PROFILE=<profile string>` sets both hands from the start, which also makes a headless `--shot --menu` draw
the sim's right hand. Only the right hand is ever drawn in the sim (the mouse is that controller); a key press hides
the controller until the mouse moves.

**The Quest APK.** The game builds its plugin for arm64-v8a with the NDK (r27d), puts the `.so` (and
`libphonon.so`) in a `jniLibs/arm64-v8a/` folder, and runs vrcore's Gradle project with its own identity:

```bash
<vrcore>/android/gradlew -p <vrcore>/android assembleDebug \
    -PvrcoreFront=<the front> -PvrcoreJniLibs=<jniLibs folder> -PvrcoreAssets=<extra assets> \
    -PvrcoreExtern=<extern> -PvrcoreSdl=<extern>/sdl3 -PvrcoreNdk=<ndk> \
    -PvrcoreBuildDir=<the game's build folder> \
    -PvrcoreAppId=<application id> -PvrcoreAppName=<name in the Library> \
    -PvrcoreVersionName=<version> -PvrcoreVersionCode=<n>
```

With no front, the APK is vrcore's test scene. The activity (`VrcoreActivity`) is vrcore's in every game's APK,
and the application id, name and version are the game's. `-PvrcoreBuildDir` keeps Gradle's output in the game's
build folder and out of the vrcore checkout. The APK is signed with the builder's own debug key.

## A new game, step by step

1. **Plugin, flat picture first.** Export the identity functions, `vr_start`/`vr_state`/`vr_stop`/`vr_pause`,
   `vr_set_controls`, `vr_audio`, `vr_picture` (`vr_renderer` returns 0) and `vr_stats`, with stubs for the rest.
   Run `vr_plugin_test` until pass 1 is clean.
2. **ROMs.** Add `vr_roms` so the host can check the set and offer the picker.
3. **A minimal front.** Open the plugin, show the flat picture on a quad layer, map the controls, and open the
   menu. Run it in the Mac sim, then as an APK.
4. **3D frames.** Export the board's display list through `vr_frame` with the board block, tables and assets.
   Write the board's shaders in the front. `vr_plugin_test`'s pass 2 checks the frames.
5. **Smooth motion.** Write the `vr_interp` adapter and fill `prev`. Turn on `vr_level` if the camera rolls or
   pitches.
6. **The rest of the front.** Graphics profiles per headset, the game's menu pages, the wheel/stick/shifter/rumble
   choices, and the self test and soak hooks.
7. **Gate it.** A clean `vr_plugin_test` (both passes, and with `VR_TEST_LEVEL=3`), a self test with the expected
   hash, a soak test, then the headsets.

## Checks

```bash
# native/: C99 and C++17, 0 warnings expected
cc  -std=c99   -Wall -Wextra -fsyntax-only native/*.c
for f in native/*.c; do c++ -std=c++17 -x c++ -Wall -Wextra -fsyntax-only "$f"; done

# a plugin driven the way the host drives it, headless; exit 0 = a clean run
vr_plugin_test <plugin.bundle | libplugin.so> <rom_dir> [seconds] [shots_dir]
```

The head of `tools/vr_plugin_test.c` lists every `VR_TEST_*` setting. **A refactor must leave the plugin's output
unchanged:** run with `VR_TEST_HASH=<prefix>` and `VR_TEST_ARGS="--replay <recording>"` before and after the change,
and compare the hash files.

The host's own device-free checks run in the sim: `--rig-test DIR`, `--audio-test DIR`, `--settings-test DIR`,
`--soundtrack-test DIR` (it makes its own songs with `afconvert`; exit 0 when everything passed). On a headset, the `tools/quest_*.sh` scripts print their usage with `--help`.
Set `VR_DRY_RUN=1` to print their adb commands with no headset.

## Changing vrcore

- **Board-neutral only.** A difference between boards is a hook or a setting, never a fork. If something only
  makes sense for one game, it goes in that game.
- **Nothing from any game goes here**: no ROMs, extracted data, generated code or board-specific tables.
- **The ABI.** A change to `abi/vrcore.h` is made in `host/app/plugin.h` and `tools/vr_plugin_test.c` at the same
  time, and every game is rebuilt. Bump `VR_ABI_VERSION` only when an existing call or float offset changes. A new
  optional export needs no bump.
- **Every game is a user.** Try a change against each game (they build against the working clone, or `VRCORE_DIR=<folder>`) before
  pushing it, and rebuild each game after.
