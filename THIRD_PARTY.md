# Third-party components

Every component vrcore fetches, links or reads from: upstream, pinned commit, licence, how it is used and what was
changed. `tools/fetch_extern.py` holds the pins (one list for every game built on vrcore) and fetches them into a
git-ignored `extern/` folder (default `../../extern` from here, CMake's `VRCORE_EXTERN_DIR`); nothing fetched is
committed. A game's own THIRD_PARTY.md says which vrcore tag pins them and points here.

| Component | Upstream | Commit | Licence | Use | Changes |
| --- | --- | --- | --- | --- | --- |
| SDL 3 | https://github.com/libsdl-org/SDL | `fa2c02bb6e21974a89ea9824bc53c9932abe5f9c` (release-3.4.16) | Zlib | The host's window, input, gamepads and Android activity (`android/`: SDLActivity, compiled from the pinned checkout); linked into every app (`vrcore_add_app`). A game's desktop build may link it too. Fetched by `tools/fetch_extern.py` (`--desktop` too) into git-ignored `extern/sdl3`, built from source. | None. |
| Dear ImGui | https://github.com/ocornut/imgui | `f1cc2ae15e53a861a874c3034aae6798fde194ab` (v1.92.9b) | MIT | Its core files alone (no backends) in `vrcore_host`: the headset's menus (`host/ui`), with its built-in font. Fetched by `tools/fetch_extern.py` (`--desktop` too) into git-ignored `extern/imgui`. | None. |
| OpenXR SDK (Khronos loader and headers) | https://github.com/KhronosGroup/OpenXR-SDK | `f2448a8797c85814aa892efc1ab8707900fbcc78` (release-1.1.63) | Apache-2.0 (loader: Apache-2.0 OR MIT) | The host (from M1): the OpenXR loader and headers, desktop and Android. Fetched by `tools/fetch_extern.py` into git-ignored `extern/openxr`. Used: the loader built unmodified as a shared library and packaged in the APK's `lib/arm64-v8a`; the APK carries the SDK's Apache-2.0 licence under `assets/licenses/openxr/`. | None. |
| Vulkan-Headers | https://github.com/KhronosGroup/Vulkan-Headers | `6802bb4733b63ed5efd3adb308a6c885ef180ea1` (vulkan-sdk-1.4.363.0) | Apache-2.0 OR MIT | The host (from M1): the Vulkan API headers. Fetched by `tools/fetch_extern.py` into git-ignored `extern/vulkan-headers`. Not used yet. | None. |
| Vulkan Memory Allocator | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | `3aa921224c154a0d2c43912bc88e1c42ce1f7607` (v3.4.0) | MIT | The host (from M1): GPU allocations, lazily allocated MSAA attachments. Fetched by `tools/fetch_extern.py` into git-ignored `extern/vma`. Not used yet. | None. |
| volk | https://github.com/zeux/volk | `54fc0d7a72887cbfc5b030d26897b2b40ac53fb8` (vulkan-sdk-1.4.363.0) | MIT | The host (from M1): Vulkan function loading. Fetched by `tools/fetch_extern.py` into git-ignored `extern/volk`. Not used yet. | None. |
| cgltf | https://github.com/jkuhlmann/cgltf | `360db1a95480fe102ae9c69b27c5d101167ff5ba` (v1.15) | MIT | The host (from M3): loads the hand wheel's `assets/wheel.glb` (`host/rig/wheel_model.cpp`) and the controllers (`host/ui/controllers.cpp`; the implementation compiled once in `host/rig/cgltf_impl.cpp`). Fetched by `tools/fetch_extern.py` into git-ignored `extern/cgltf`. | None. |
| Steam Audio | https://github.com/ValveSoftware/steam-audio (release zip `steamaudio_4.8.1.zip`, sha256 `4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449`) | `0da18255cca520771f363ee01f100572b39a308e` (v4.8.1) | Apache-2.0 | The host (from M4): HRTF spatialisation of the cabinet speakers. The release carries the prebuilt libraries, so it is fetched as that zip by `tools/fetch_extern.py` into git-ignored `extern/steam-audio` (not a git checkout). Used: linked unmodified as the prebuilt libphonon (the Mac dylib for the desktop sim, the Android arm64 `.so` packaged in the APK's `lib/arm64-v8a`). The APK carries its Apache-2.0 licence and the release's `THIRDPARTY.md` (the notices of its own bundled parts) under `assets/licenses/steam-audio/`. | None. |
| Gradle wrapper (`gradlew`, `gradlew.bat`, `gradle-wrapper.jar`) | https://github.com/gradle/gradle (copied from SDL 3.4.16's `android-project/`) | the SDL 3.4.16 template | Apache-2.0 | Committed in `android/`: runs Gradle 8.12 (downloaded from services.gradle.org on first use) for the APK (a game's build passes its front: `android/app/build.gradle`). Android Gradle Plugin 8.7.3 (as SDL's template) and NDK r27d are resolved at build time, never vendored. | `gradle-wrapper.properties`: the timestamp comment dropped. |
| "MOMO STEERING WHEEL" by ijiklvn (3D model) | https://skfb.ly/o9MyX | — | CC BY 4.0 (http://creativecommons.org/licenses/by/4.0/) | Committed as `assets/wheel.glb` (converted once from the model's `SM_Wheel.fbx` with assimp 6.0.5: `assimp export SM_Wheel.fbx wheel.glb -fglb2`), packed into the native host's APK and drawn by `host/rig/wheel_model.cpp`. Credited in `README.md` (Credits) and `LICENSE`. | wheel.glb: converted to glTF binary, geometry unchanged (7091 triangles). |
| Generic controller models (`generic-trigger-squeeze-thumbstick` `left.glb`, `right.glb`) | https://github.com/immersive-web/webxr-input-profiles (`packages/assets/profiles/generic-trigger-squeeze-thumbstick`) | `main` as of 2026-10-15 (left.glb git blob `16abb111085324215b3e677b96f2d76ec9685232`, 1442452 bytes; right.glb git blob `1787077da663bcc30601b877584f5b927b0efca2`, 1332668 bytes) | MIT, Copyright (c) 2019 Amazon (the notice below) | Committed as `assets/controller_left.glb` and `assets/controller_right.glb`, packed into the native host's APK and drawn by `host/ui/controllers.cpp` at each shown controller's grip while a menu is up. | Renamed only; the files are byte for byte the upstream's. |

The generic controller models' licence, as in the upstream's `packages/assets/LICENSE.md`:

> MIT License
>
> Copyright (c) 2019 Amazon
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
> rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
> permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice (including the next paragraph) shall be included in all copies
> or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
> WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
> COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
> OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

vrcore's own code is MIT (`LICENSE`).
