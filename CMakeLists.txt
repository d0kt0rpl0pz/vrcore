# vrcore: the VR host. A self-contained project (MIT, no game includes): a game's front
# links the host static library through vrcore_add_app() and gets an app target.
#
#   Mac desktop:  cmake -S <this folder> -B build-native -G Ninja [-DVRCORE_FRONT_DIR=<a game's front>]
#                 cmake --build build-native        -> build-native/vrcore_scene (the sim), and the front's app
#   Quest APK:    the game's Android build script  (Gradle runs this file through
#                                  externalNativeBuild, arm64-v8a)
#
# VRCORE_FRONT_DIR: a game's front, a folder whose CMakeLists.txt calls vrcore_add_app(); on Android its
# app is the APK's libmain.so (in place of the test scene's).
#
# Libraries come from VRCORE_EXTERN_DIR (default: the checkout's extern/, fetched and pinned
# by tools/fetch_extern.py): SDL3, volk, VMA, the Vulkan headers and, on Android, the
# OpenXR loader. The Mac sim runs on MoltenVK (Homebrew's molten-vk). Shaders are compiled by
# glslc (Homebrew's shaderc on the Mac, the NDK's shader-tools for the APK).
cmake_minimum_required(VERSION 3.20)
project(vrcore LANGUAGES C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(VRCORE_EXTERN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../extern" CACHE PATH "Pinned third-party sources")

# SDL3: shared on Android (SDL's activity loads libSDL3.so and libmain.so), static on desktop.
if(NOT EXISTS ${VRCORE_EXTERN_DIR}/sdl3/CMakeLists.txt)
    message(FATAL_ERROR "vrcore: no SDL3 at ${VRCORE_EXTERN_DIR}/sdl3 (run tools/fetch_extern.py)")
endif()
if(ANDROID)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(_vrcore_sdl_exclude "")
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(_vrcore_sdl_exclude EXCLUDE_FROM_ALL)
endif()
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${VRCORE_EXTERN_DIR}/sdl3 ${CMAKE_CURRENT_BINARY_DIR}/sdl3 ${_vrcore_sdl_exclude} SYSTEM)

foreach(_dir volk vma/include vulkan-headers/include cgltf imgui)
    if(NOT EXISTS ${VRCORE_EXTERN_DIR}/${_dir})
        message(FATAL_ERROR "vrcore: no ${VRCORE_EXTERN_DIR}/${_dir} (run tools/fetch_extern.py)")
    endif()
endforeach()

# Steam Audio (Apache-2.0): the release's prebuilt libphonon, required (HRTF for the cabinet speakers).
# Shared: the Mac dylib (install name @rpath/libphonon.dylib, so the apps get its folder as build rpath)
# and the arm64 .so, which the game's Android build script copies into the APK's lib/arm64-v8a.
set(VRCORE_PHONON_DIR ${VRCORE_EXTERN_DIR}/steam-audio/steamaudio)
if(NOT EXISTS ${VRCORE_PHONON_DIR}/include/phonon.h)
    message(FATAL_ERROR "vrcore: no Steam Audio at ${VRCORE_EXTERN_DIR}/steam-audio (run tools/fetch_extern.py)")
endif()
if(ANDROID)
    set(_vrcore_phonon_lib ${VRCORE_PHONON_DIR}/lib/android-armv8/libphonon.so)
elseif(APPLE)
    set(_vrcore_phonon_lib ${VRCORE_PHONON_DIR}/lib/osx/libphonon.dylib)
else()
    message(FATAL_ERROR "vrcore: Steam Audio is wired for macOS and Android only so far")
endif()
if(NOT EXISTS ${_vrcore_phonon_lib})
    message(FATAL_ERROR "vrcore: no ${_vrcore_phonon_lib} (run tools/fetch_extern.py)")
endif()
add_library(vrcore_phonon SHARED IMPORTED)
set_target_properties(vrcore_phonon PROPERTIES IMPORTED_LOCATION ${_vrcore_phonon_lib})
if(ANDROID)
    set_target_properties(vrcore_phonon PROPERTIES IMPORTED_SONAME libphonon.so)
endif()
# SYSTEM, so phonon.h does not warn
target_include_directories(vrcore_phonon SYSTEM INTERFACE ${VRCORE_PHONON_DIR}/include)

# The Khronos OpenXR loader, a shared library in the APK (AGP packages it with libmain.so).
if(ANDROID)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
    set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
    add_subdirectory(${VRCORE_EXTERN_DIR}/openxr ${CMAKE_CURRENT_BINARY_DIR}/openxr SYSTEM)
endif()

# glslc: SPIR-V for Vulkan 1.1, written as a list of numbers (-mfmt=num) that the code #includes.
find_program(VRCORE_GLSLC glslc
    HINTS ${ANDROID_NDK}/shader-tools/darwin-x86_64 ${ANDROID_NDK}/shader-tools/linux-x86_64 /opt/homebrew/bin
    NO_CMAKE_FIND_ROOT_PATH)
if(NOT VRCORE_GLSLC)
    message(FATAL_ERROR "vrcore: no glslc (brew install shaderc; the NDK has one in shader-tools)")
endif()

# vrcore_add_shaders(<target> <out name> <source> [DEFINES ...] [INCLUDES <dirs>...]): <source> compiled to
# <out name>.h in the build tree, which <target> can #include. Shaders include shaders/vrcore_views.glsl, and
# from the INCLUDES folders.
function(vrcore_add_shaders target out src)
    cmake_parse_arguments(S "" "" "DEFINES;INCLUDES" ${ARGN})
    set(_dir ${CMAKE_BINARY_DIR}/vrcore_shaders)
    set(_out ${_dir}/${out}.h)
    get_filename_component(_ext ${src} LAST_EXT)
    string(SUBSTRING ${_ext} 1 -1 _stage)
    set(_defs "")
    foreach(_d ${S_DEFINES})
        list(APPEND _defs -D${_d})
    endforeach()
    foreach(_i ${S_INCLUDES})
        list(APPEND _defs -I ${_i})
    endforeach()
    add_custom_command(OUTPUT ${_out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_dir}
        COMMAND ${VRCORE_GLSLC} --target-env=vulkan1.1 -O -fshader-stage=${_stage} ${_defs}
                -I ${VRCORE_SHADER_DIR} -MD -MF ${_out}.d -mfmt=num -o ${_out} ${src}
        DEPENDS ${src}
        DEPFILE ${_out}.d
        COMMENT "glslc ${out}"
        VERBATIM)
    target_sources(${target} PRIVATE ${_out})
    target_include_directories(${target} PRIVATE ${_dir})
endfunction()
set(VRCORE_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)

# The host: C++17 (no exceptions or RTTI on Android) plus the board-neutral C99 in native/. Every
# Vulkan call goes through volk (VK_NO_PROTOTYPES), loaded at run time.
add_library(vrcore_host STATIC
    host/app/app.cpp
    host/app/headset.cpp
    host/app/input.cpp
    host/app/log.cpp
    host/app/pacing.cpp
    host/app/pads.cpp
    host/app/picker_test.cpp
    host/app/soundtrack_probe.cpp
    host/app/plugin.cpp
    host/app/roms.cpp
    host/app/selftest.cpp
    host/app/settings.cpp
    host/app/soak.cpp
    host/audio/audio.cpp
    host/audio/library.cpp
    host/audio/soundtrack.cpp
    host/audio/spatial.cpp
    host/audio/storage.cpp
    host/rig/rig.cpp
    host/rig/rumble.cpp
    host/rig/shifter.cpp
    host/rig/stick.cpp
    host/rig/wheel.cpp
    host/rig/wheel_model.cpp
    host/rig/cgltf_impl.cpp
    host/gpu/device.cpp
    host/gpu/eye_target.cpp
    host/gpu/ktx2.cpp
    host/gpu/render.cpp
    host/gpu/vk_impl.cpp
    host/ui/controllers.cpp
    host/ui/graphics.cpp
    host/ui/imgui_render.cpp
    host/ui/menu.cpp
    host/ui/pick.cpp
    host/ui/soundtrack_page.cpp
    ${VRCORE_EXTERN_DIR}/imgui/imgui.cpp
    ${VRCORE_EXTERN_DIR}/imgui/imgui_draw.cpp
    ${VRCORE_EXTERN_DIR}/imgui/imgui_tables.cpp
    ${VRCORE_EXTERN_DIR}/imgui/imgui_widgets.cpp
    ${VRCORE_EXTERN_DIR}/volk/volk.c
    native/vr_interp.c
    native/vr_level.c)
if(ANDROID)
    target_sources(vrcore_host PRIVATE
        host/xr/xr.cpp
        host/xr/xr_actions.cpp
        host/xr/xr_layers.cpp
        host/xr/xr_meta.cpp
        host/xr/xr_passthrough.cpp
        host/xr/xr_swapchain.cpp
        host/audio/decode_android.cpp)
else()
    target_sources(vrcore_host PRIVATE host/audio/decode_apple.cpp)
    target_sources(vrcore_host PRIVATE host/rig/rig_test.cpp host/audio/audio_test.cpp host/app/settings_test.cpp host/ui/soundtrack_test.cpp host/sim/sim.cpp host/sim/sim_layers.cpp)
    find_library(VRCORE_MOLTENVK NAMES MoltenVK HINTS /opt/homebrew/lib /usr/local/lib)
    if(NOT VRCORE_MOLTENVK)
        message(FATAL_ERROR "vrcore: no MoltenVK (brew install molten-vk)")
    endif()
    target_compile_definitions(vrcore_host PRIVATE VRCORE_MOLTENVK_PATH="${VRCORE_MOLTENVK}")
endif()
# the layers' quad: the sim's stand-in for every layer; on both hosts an underlay's hole (layer_punch)
vrcore_add_shaders(vrcore_host layer_quad.vert ${VRCORE_SHADER_DIR}/layer_quad.vert)
vrcore_add_shaders(vrcore_host layer_punch.frag ${VRCORE_SHADER_DIR}/layer_punch.frag)
if(NOT ANDROID)
    vrcore_add_shaders(vrcore_host layer_quad.frag ${VRCORE_SHADER_DIR}/layer_quad.frag)
endif()
# the menus (ui/): Dear ImGui's draw data into the layers, both hosts
vrcore_add_shaders(vrcore_host imgui.vert ${VRCORE_SHADER_DIR}/imgui.vert)
vrcore_add_shaders(vrcore_host imgui.frag ${VRCORE_SHADER_DIR}/imgui.frag)
# the controllers and the ray (ui/controllers): the test scene's lit shader, a model and a colour a draw
vrcore_add_shaders(vrcore_host controller.vert ${VRCORE_SHADER_DIR}/scene.vert)
vrcore_add_shaders(vrcore_host controller.frag ${VRCORE_SHADER_DIR}/scene.frag)
vrcore_add_shaders(vrcore_host controller_tex.vert ${VRCORE_SHADER_DIR}/scene.vert DEFINES TEXTURED)
vrcore_add_shaders(vrcore_host controller_tex.frag ${VRCORE_SHADER_DIR}/scene.frag DEFINES TEXTURED)
# the hand wheel (rig/wheel_model): its shaders on both hosts; its model from assets/ (the APK's assets on Android),
# as are the controllers' (ui/controllers)
vrcore_add_shaders(vrcore_host wheel.vert ${VRCORE_SHADER_DIR}/wheel.vert)
vrcore_add_shaders(vrcore_host wheel.frag ${VRCORE_SHADER_DIR}/wheel.frag)
if(NOT ANDROID)
    target_compile_definitions(vrcore_host PRIVATE VRCORE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
endif()
set_target_properties(vrcore_host PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
                      CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(vrcore_host SYSTEM PUBLIC ${VRCORE_EXTERN_DIR}/imgui)     # Dear ImGui's core (no backends)
target_include_directories(vrcore_host PUBLIC host host/app native abi
    ${VRCORE_EXTERN_DIR}/volk ${VRCORE_EXTERN_DIR}/vma/include ${VRCORE_EXTERN_DIR}/vulkan-headers/include)
target_include_directories(vrcore_host PRIVATE ${VRCORE_EXTERN_DIR}/cgltf)
target_compile_definitions(vrcore_host PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(vrcore_host PUBLIC SDL3::SDL3 vrcore_phonon ${CMAKE_DL_LIBS})
if(ANDROID)
    target_compile_options(vrcore_host PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
    target_compile_definitions(vrcore_host PRIVATE XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_VULKAN XR_USE_TIMESPEC)
    target_link_libraries(vrcore_host PUBLIC openxr_loader log android aaudio mediandk)
endif()
# Third-party sources in the host library are not ours to warn about.
set_source_files_properties(${VRCORE_EXTERN_DIR}/volk/volk.c host/gpu/vk_impl.cpp host/rig/cgltf_impl.cpp
                            ${VRCORE_EXTERN_DIR}/imgui/imgui.cpp ${VRCORE_EXTERN_DIR}/imgui/imgui_draw.cpp
                            ${VRCORE_EXTERN_DIR}/imgui/imgui_tables.cpp ${VRCORE_EXTERN_DIR}/imgui/imgui_widgets.cpp
                            PROPERTIES COMPILE_OPTIONS -w)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(vrcore_host PUBLIC m)
endif()
if(APPLE)
    # the soundtrack's decoder (audio/decode_apple.cpp)
    target_link_libraries(vrcore_host PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
endif()
if(NOT MSVC)
    target_compile_options(vrcore_host PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

# vrcore_add_app(<name> SOURCES <front sources...>): the app a game links. Desktop: an
# executable. Android: libmain.so, which SDL's activity runs through SDL_main.
function(vrcore_add_app name)
    cmake_parse_arguments(A "" "" "SOURCES" ${ARGN})
    if(ANDROID)
        add_library(${name} SHARED ${VRCORE_HOST_DIR}/app/entry.cpp ${A_SOURCES})
        set_target_properties(${name} PROPERTIES OUTPUT_NAME main)
    else()
        add_executable(${name} ${VRCORE_HOST_DIR}/app/entry.cpp ${A_SOURCES})
    endif()
    set_target_properties(${name} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_link_libraries(${name} PRIVATE vrcore_host)
endfunction()
set(VRCORE_HOST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/host)

# The test scene: M1's exit target (lit cubes, fine-line grids; the sim on the desktop, the APK on the Quest
# when no front is given).
set(VRCORE_FRONT_DIR "" CACHE PATH "A game's front (its CMakeLists.txt calls vrcore_add_app)")
if(NOT (ANDROID AND VRCORE_FRONT_DIR))
    vrcore_add_app(vrcore_scene SOURCES host/test/scene_front.cpp)
    vrcore_add_shaders(vrcore_scene scene.vert ${VRCORE_SHADER_DIR}/scene.vert)
    vrcore_add_shaders(vrcore_scene scene.frag ${VRCORE_SHADER_DIR}/scene.frag)
    vrcore_add_shaders(vrcore_scene scene_fdm.frag ${VRCORE_SHADER_DIR}/scene.frag DEFINES FDM_VIEW)
    vrcore_add_shaders(vrcore_scene panel.vert ${VRCORE_SHADER_DIR}/panel.vert)
    vrcore_add_shaders(vrcore_scene panel.frag ${VRCORE_SHADER_DIR}/panel.frag)
    vrcore_add_shaders(vrcore_scene offscreen.vert ${VRCORE_SHADER_DIR}/offscreen.vert)
    vrcore_add_shaders(vrcore_scene offscreen.frag ${VRCORE_SHADER_DIR}/offscreen.frag)
endif()
# The menu sample: no game, only the menu. The sim only: on Android an app is libmain.so, and
# the scene's already is.
if(NOT ANDROID)
    vrcore_add_app(vrcore_menu SOURCES host/test/menu_front.cpp)
endif()
if(VRCORE_FRONT_DIR)
    add_subdirectory(${VRCORE_FRONT_DIR} ${CMAKE_CURRENT_BINARY_DIR}/front)
endif()
