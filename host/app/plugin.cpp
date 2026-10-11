#include "plugin.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <unistd.h>

#include "log.h"

namespace vrcore {

namespace {

// Binds `name` into `fn`; false (and `missing` set) when the plugin has no such symbol.
template <typename T>
bool take(void *handle, const char *name, T &fn, std::string &missing)
{
    fn = reinterpret_cast<T>(dlsym(handle, name));
    if (!fn && missing.empty()) missing = name;
    return fn != nullptr;
}

}

// Where a restart's fresh copy is written: Android: the app's internal files folder (a library the app may load);
// desktop: $TMPDIR.
std::string copy_dir()
{
#ifdef __ANDROID__
    const char *d = SDL_GetAndroidInternalStoragePath();
    return std::string(d ? d : ".") + "/";
#else
    const char *tmp = std::getenv("TMPDIR");
    std::string d = tmp && *tmp ? tmp : "/tmp";
    return d.back() == '/' ? d : d + "/";
#endif
}

uint32_t le16(const uint8_t *b) { return b[0] | b[1] << 8; }
uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

// The binary a loaded `handle` came from, copied to `dest`; empty, or why not. Android loads an APK's libraries
// straight from base.apk ("<apk>!/lib/arm64-v8a/lib<name>.so", stored uncompressed): that entry is copied out.
std::string copy_binary(void *handle, const std::string &name, const std::string &dest)
{
    void *sym = dlsym(handle, "vr_abi_version");
    Dl_info info = {};
    if (!sym || !dladdr(sym, &info) || !info.dli_fname) return "where " + name + " was loaded from is not known";
    std::string from = info.dli_fname;
    size_t bang = from.find("!/");
    if (bang == std::string::npos) return SDL_CopyFile(from.c_str(), dest.c_str()) ? std::string() : SDL_GetError();

    std::string apk = from.substr(0, bang), entry = from.substr(bang + 2);
    SDL_IOStream *io = SDL_IOFromFile(apk.c_str(), "rb");
    if (!io) return SDL_GetError();
    std::string why = "no " + entry + " stored in " + apk;
    Sint64 len = SDL_GetIOSize(io);
    std::vector<uint8_t> tail((size_t)std::min<Sint64>(len, 22 + 0xffff));
    if (SDL_SeekIO(io, len - (Sint64)tail.size(), SDL_IO_SEEK_SET) >= 0 &&
        SDL_ReadIO(io, tail.data(), tail.size()) == tail.size()) {
        int e = -1;
        for (int i = (int)tail.size() - 22; i >= 0 && e < 0; --i)
            if (le32(&tail[i]) == 0x06054b50) e = i;
        std::vector<uint8_t> cd(e >= 0 ? le32(&tail[e + 12]) : 0);
        if (e >= 0 && SDL_SeekIO(io, le32(&tail[e + 16]), SDL_IO_SEEK_SET) >= 0 &&
            SDL_ReadIO(io, cd.data(), cd.size()) == cd.size()) {
            for (size_t p = 0; p + 46 <= cd.size() && le32(&cd[p]) == 0x02014b50;) {
                uint32_t nl = le16(&cd[p + 28]), xl = le16(&cd[p + 30]), cl = le16(&cd[p + 32]);
                if (p + 46 + nl > cd.size()) break;
                if (le16(&cd[p + 10]) == 0 && entry.compare(0, std::string::npos,
                                                            reinterpret_cast<const char *>(&cd[p + 46]), nl) == 0) {
                    uint32_t size = le32(&cd[p + 24]), at = le32(&cd[p + 42]);
                    uint8_t lh[30];
                    std::vector<uint8_t> data(size);
                    if (SDL_SeekIO(io, at, SDL_IO_SEEK_SET) >= 0 && SDL_ReadIO(io, lh, 30) == 30 &&
                        SDL_SeekIO(io, at + 30 + le16(&lh[26]) + le16(&lh[28]), SDL_IO_SEEK_SET) >= 0 &&
                        SDL_ReadIO(io, data.data(), size) == size)
                        why = SDL_SaveFile(dest.c_str(), data.data(), size) ? std::string() : SDL_GetError();
                    break;
                }
                p += 46 + nl + xl + cl;
            }
        }
    }
    SDL_CloseIO(io);
    return why;
}

// A failed open: the library closed again and no function left bound.
bool Plugin::fail(void *h)
{
    dlclose(h);
    *this = Plugin{};
    return false;
}

bool Plugin::open(const char *name, std::string &error)
{
    error.clear();
    if (!name || !*name) {
        error = "no plugin named";
        return false;
    }
    std::string path = name;
    if (!std::strchr(name, '/')) {
#ifdef __ANDROID__
        path = std::string("lib") + name + ".so";
#else
        error = std::string("plugin ") + name + ": give its path";
        return false;
#endif
    }
    // A restart (app.h's request_restart) opens the plugin again: dlopen would hand back the copy already loaded,
    // its game's globals as the last run left them, so a fresh copy of the binary is loaded from a file of our own
    // (unlinked once loaded; the old copy stays loaded, never closed).
    static std::vector<std::pair<std::string, void *>> opened;     // the name asked for, its first handle
    void *first = nullptr;
    for (const auto &o : opened)
        if (o.first == path) first = o.second;
    std::string load = path;
    if (first) {
        static int copies = 0;
        load = copy_dir() + "vrcore-plugin-" + std::to_string((long)getpid()) + "-" + std::to_string(++copies) + ".so";
        std::string why = copy_binary(first, path, load);
        if (!why.empty()) {
            error = "a fresh copy of " + path + " for the restart: " + why;
            return false;
        }
    }
    void *h = dlopen(load.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (first) {
        unlink(load.c_str());
        if (h) log("plugin: a fresh copy for the restart (%s)", load.c_str());
    }
    if (!h) {
        const char *e = dlerror();
        error = "dlopen " + path + ": " + (e ? e : "failed");
        return false;
    }

    std::string missing;
    take(h, "vr_abi_version", abi_version, missing);
    if (!abi_version) {
        error = path + " has no vr_abi_version: not a vrcore plugin";
        return fail(h);
    }
    int v = abi_version();
    if (v != VR_ABI_VERSION) {
        error = path + " speaks vrcore " + std::to_string(v) + ", this host " + std::to_string(VR_ABI_VERSION) +
                ": rebuild one of them";
        return fail(h);
    }
    take(h, "vr_game", game, missing);
    take(h, "vr_vertex_floats", vertex_floats, missing);
    take(h, "vr_start", start, missing);
    take(h, "vr_state", state, missing);
    take(h, "vr_exit_code", exit_code, missing);
    take(h, "vr_stop", stop, missing);
    take(h, "vr_pause", pause, missing);
    take(h, "vr_set_controls", set_controls, missing);
    take(h, "vr_pedal_max", pedal_max, missing);
    take(h, "vr_feedback", feedback, missing);
    take(h, "vr_audio", audio, missing);
    take(h, "vr_renderer", renderer, missing);
    take(h, "vr_picture", picture, missing);
    take(h, "vr_asset_count", asset_count, missing);
    take(h, "vr_asset_size", asset_size, missing);
    take(h, "vr_assets", assets, missing);
    take(h, "vr_table_count", table_count, missing);
    take(h, "vr_table_size", table_size, missing);
    take(h, "vr_frame", frame, missing);
    take(h, "vr_revision", revision, missing);
    take(h, "vr_now_ns", now_ns, missing);
    take(h, "vr_frame_ns", frame_ns, missing);
    take(h, "vr_set_world", set_world, missing);
    take(h, "vr_set_option", set_option, missing);
    take(h, "vr_get_option", get_option, missing);
    take(h, "vr_stats", stats, missing);
    take(h, "vr_interp_stats", interp_stats, missing);
    if (!missing.empty()) {
        error = path + " has no " + missing;
        return fail(h);
    }
    // remembered only once it passed every check: a handle fail() closed is never copied from
    if (!first) opened.push_back({path, h});
    handle_ = h;
    path_ = path;
    log("plugin: %s (%s), vrcore %d, %d floats a vertex", path.c_str(), game(), v, vertex_floats());
    return true;
}

void *Plugin::bind(const char *name) const
{
    return handle_ ? dlsym(handle_, name) : nullptr;
}

}
