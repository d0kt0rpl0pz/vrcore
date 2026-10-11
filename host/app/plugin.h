/*
 * plugin.h -- a game plugin's vrcore C API (abi/vrcore.h), opened with dlopen and bound by name. One run per
 * loaded copy: a game's state is C globals that are set up once (ONE RUN PER LOADED COPY). A restart (app.h)
 * opens it again in the same process, from a fresh copy: desktop from $TMPDIR, Android copied out of base.apk into the app's files folder.
 *   Android: lib<name>.so in the APK, found by name (the app's library folder is on the linker's path).
 *   Desktop: a path to the binary (a name alone is refused: the front says where its build puts it).
 * Never closed: a game's threads may still idle in it at exit.
 *
 * dlopen, not a link, so any game's plugin loads the same way and a front binds its own extras (bind).
 */
#pragma once

#include <cstdint>
#include <string>

#include "vrcore.h"

namespace vrcore {

struct Plugin {
    // Opens `name` (above) and binds every ABI 1 function, and the optional exports it has; false and `error` when
    // the binary is missing, lacks a required one, or speaks another VR_ABI_VERSION.
    bool open(const char *name, std::string &error);
    // A game's own function by name (null: the plugin has none).
    void *bind(const char *name) const;
    const std::string &path() const { return path_; }

    // abi/vrcore.h, the vr_ prefix dropped; all set once open() succeeds.
    int (*abi_version)(void) = nullptr;
    const char *(*game)(void) = nullptr;
    int (*vertex_floats)(void) = nullptr;
    int (*start)(const char *rom_dir, int audio_hz, const char *args) = nullptr;
    int (*state)(void) = nullptr;
    int (*exit_code)(void) = nullptr;
    int (*stop)(int timeout_ms) = nullptr;
    void (*pause)(int on) = nullptr;
    void (*set_controls)(uint32_t held, int steer_on, int steer, int gas_on, int gas, int brake_on,
                         int brake) = nullptr;
    int (*pedal_max)(void) = nullptr;
    float (*feedback)(void) = nullptr;
    int (*audio)(float *buf, int frames, int channels) = nullptr;
    int (*renderer)(void) = nullptr;
    uint32_t (*picture)(uint8_t *dst, int cap, uint32_t have, int *w, int *h) = nullptr;
    int (*asset_count)(void) = nullptr;
    int (*asset_size)(int i) = nullptr;
    int (*assets)(uint8_t *const *dst) = nullptr;
    int (*table_count)(void) = nullptr;
    int (*table_size)(int i) = nullptr;
    uint32_t (*frame)(uint32_t have, uint32_t want, float *v, int vcap, uint32_t *idx, int icap,
                      uint8_t *const *tables, int *nv, int *ni, uint32_t *present, int64_t *t_ns,
                      int64_t *ready_ns) = nullptr;
    uint32_t (*revision)(void) = nullptr;
    int64_t (*now_ns)(void) = nullptr;
    int64_t (*frame_ns)(void) = nullptr;
    void (*set_world)(int world3d, int keep_behind) = nullptr;
    void (*set_option)(int key, int value) = nullptr;
    int (*get_option)(int key) = nullptr;
    void (*stats)(uint32_t *frame, uint32_t *c0, uint32_t *c1, uint32_t *c2) = nullptr;
    void (*interp_stats)(double out[9]) = nullptr;
    // The optional exports, bound when the plugin has them (null: it has not).
    const char *(*roms)(void) = nullptr;

private:
    bool fail(void *h);
    void *handle_ = nullptr;
    std::string path_;
};

}
