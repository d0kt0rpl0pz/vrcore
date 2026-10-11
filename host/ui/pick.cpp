#include "pick.h"

#include <mutex>

#include <SDL3/SDL.h>

#ifdef __ANDROID__
#include <jni.h>
#endif

#include "log.h"

namespace vrcore::ui {

namespace {

std::mutex g_lock;
bool g_busy = false, g_done = false;
std::vector<Pick> g_picks;
std::string g_error;

// the URI's last part, %-decoded: "content://.../document/primary%3ADownload%2Fdaytona93.zip" -> daytona93.zip
std::string name_of(const char *uri)
{
    std::string d;
    for (const char *p = uri; *p; ++p) {
        if (p[0] == '%' && SDL_isxdigit(p[1]) && SDL_isxdigit(p[2])) {
            char h[3] = {p[1], p[2], 0};
            d += (char)SDL_strtol(h, nullptr, 16);
            p += 2;
        } else {
            d += *p;
        }
    }
    size_t cut = d.find_last_of("/:");
    std::string n = cut == std::string::npos ? d : d.substr(cut + 1);
    return n.empty() ? std::string("the file") : n;
}

#ifdef __ANDROID__
void back_to_vr()
{
    JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !act) return;
    jclass c = env->GetObjectClass(act);
    jmethodID m = env->GetMethodID(c, "backToVr", "()V");
    if (m) env->CallVoidMethod(act, m);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        log("ui: pick: backToVr failed");
    }
    env->DeleteLocalRef(c);
    env->DeleteLocalRef(act);
}
#endif

void SDLCALL on_pick(void *, const char *const *files, int)
{
    std::vector<Pick> picks;
    std::string error;
    if (!files) error = std::string("The file picker failed: ") + SDL_GetError();
    else
        for (const char *const *f = files; *f; ++f) picks.push_back({*f, name_of(*f)});
    log("ui: pick: %zu file(s)%s%s", picks.size(), error.empty() ? "" : ": ", error.c_str());
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_picks = picks;
        g_error = error;
        g_done = true;
        g_busy = false;
    }
#ifdef __ANDROID__
    back_to_vr();
#endif
}

}

bool pick_open(bool multiple)
{
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_busy) return false;
        g_busy = true;
        g_done = false;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, multiple);
    log("ui: pick: opening the file picker (%s)", multiple ? "several files" : "one file");
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, on_pick, nullptr, props);
    SDL_DestroyProperties(props);
    return true;
}

bool pick_busy()
{
    std::lock_guard<std::mutex> l(g_lock);
    return g_busy;
}

bool pick_take(std::vector<Pick> &picks, std::string &error)
{
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_done) return false;
    g_done = false;
    picks.swap(g_picks);
    g_picks.clear();
    error.swap(g_error);
    g_error.clear();
    return true;
}

}
