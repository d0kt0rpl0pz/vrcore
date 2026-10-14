/*
 * storage.cpp -- storage.h: paths through POSIX on every platform, and on Android the activity's side
 * (VrcoreActivity.java) through JNI for the permission, the folder picker and a picked folder's documents.
 */
#include "storage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

#ifdef __ANDROID__
#include <jni.h>
#endif

#include "../app/log.h"

namespace vrcore::audio::storage {

namespace {

constexpr const char *kContent = "content://";

bool is_content(const std::string &ref)
{
    return !ref.compare(0, std::strlen(kContent), kContent);
}

bool list_path(const std::string &folder, std::vector<Entry> &out, bool *missing, std::string *why)
{
    DIR *d = opendir(folder.c_str());
    if (!d) {
        *missing = errno == ENOENT || errno == ENOTDIR;
        *why = std::strerror(errno);
        return false;
    }
    while (dirent *e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        Entry en;
        en.name = e->d_name;
        en.ref = folder + "/" + e->d_name;
        struct stat st;
        if (stat(en.ref.c_str(), &st) != 0) continue;       // a broken link
        en.folder = S_ISDIR(st.st_mode);
        if (!en.folder && !S_ISREG(st.st_mode)) continue;
        out.push_back(std::move(en));
    }
    closedir(d);
    return true;
}

#ifdef __ANDROID__
// A call to the activity: the env, the activity and its class, released when it ends.
struct Jni {
    JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject act = env ? static_cast<jobject>(SDL_GetAndroidActivity()) : nullptr;
    jclass cls = act ? env->GetObjectClass(act) : nullptr;

    ~Jni()
    {
        if (cls) env->DeleteLocalRef(cls);
        if (act) env->DeleteLocalRef(act);
    }
    explicit operator bool() const { return cls != nullptr; }

    jmethodID method(const char *name, const char *sig) { return env->GetMethodID(cls, name, sig); }
    // An exception thrown by the last call cleared (and logged as `what`): true when there was one.
    bool failed(const char *what)
    {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionDescribe();
        env->ExceptionClear();
        log("soundtrack: storage: %s failed in Java", what);
        return true;
    }
    std::string string(jstring s)
    {
        std::string out;
        if (!s) return out;
        if (const char *c = env->GetStringUTFChars(s, nullptr)) {
            out = c;
            env->ReleaseStringUTFChars(s, c);
        }
        env->DeleteLocalRef(s);
        return out;
    }
    // A String method with String arguments (null: an empty string, or the call failed).
    template <typename... A>
    std::string call_string(const char *name, const char *sig, A... args)
    {
        jmethodID m = method(name, sig);
        if (!m) {
            failed(name);
            return std::string();
        }
        jstring r = static_cast<jstring>(env->CallObjectMethod(act, m, args...));
        if (failed(name)) return std::string();
        return string(r);
    }
};

// A UTF-8 string handed to Java, released with the scope.
struct JStr {
    JNIEnv *env;
    jstring s;
    JStr(Jni &j, const std::string &v) : env(j.env), s(j.env->NewStringUTF(v.c_str())) {}
    ~JStr()
    {
        if (s) env->DeleteLocalRef(s);
    }
};

constexpr const char *kPermission = "android.permission.READ_MEDIA_AUDIO";

void (*g_done)(bool) = nullptr;

void SDLCALL on_permission(void *, const char *, bool granted)
{
    log("soundtrack: storage: music access %s", granted ? "granted" : "refused");
    if (g_done) g_done(granted);
}
#endif

}

std::string music_folder()
{
#ifdef __ANDROID__
    Jni j;
    return j ? j.call_string("musicFolder", "()Ljava/lang/String;") : std::string();
#else
    const char *home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/Music/Soundtracks" : std::string();
#endif
}

std::string music_folder_name()
{
#ifdef __ANDROID__
    return "Music/Soundtracks";
#else
    return "~/Music/Soundtracks";
#endif
}

Permission permission()
{
#ifdef __ANDROID__
    Jni j;
    jmethodID m = j ? j.method("permissionState", "(Ljava/lang/String;)I") : nullptr;
    if (!m) return Permission::Ask;
    JStr p(j, kPermission);
    jint s = j.env->CallIntMethod(j.act, m, p.s);
    if (j.failed("permissionState")) return Permission::Ask;
    return s == 0 ? Permission::Granted : s == 2 ? Permission::Refused : Permission::Ask;
#else
    return Permission::Granted;
#endif
}

bool request_permission(void (*done)(bool granted))
{
#ifdef __ANDROID__
    g_done = done;
    log("soundtrack: storage: asking for music access");
    return SDL_RequestAndroidPermission(kPermission, on_permission, nullptr);
#else
    (void)done;
    return false;
#endif
}

bool pick_folder()
{
#ifdef __ANDROID__
    Jni j;
    jmethodID m = j ? j.method("pickFolder", "()Z") : nullptr;
    if (!m) return false;
    bool ok = j.env->CallBooleanMethod(j.act, m);
    if (j.failed("pickFolder")) return false;
    log("soundtrack: storage: folder picker %s", ok ? "opened" : "could not be opened");
    return ok;
#else
    return false;
#endif
}

bool take_folder(std::string &ref)
{
#ifdef __ANDROID__
    Jni j;
    jmethodID m = j ? j.method("takeFolderPick", "()Ljava/lang/String;") : nullptr;
    if (!m) return false;
    jstring r = static_cast<jstring>(j.env->CallObjectMethod(j.act, m));
    if (j.failed("takeFolderPick") || !r) return false;
    ref = j.string(r);
    log("soundtrack: storage: folder picked: %s", ref.empty() ? "(cancelled)" : ref.c_str());
    return true;
#else
    (void)ref;
    return false;
#endif
}

bool folder_granted(const std::string &ref)
{
    if (!is_content(ref)) return access(ref.c_str(), R_OK) == 0;
#ifdef __ANDROID__
    Jni j;
    jmethodID m = j ? j.method("folderGranted", "(Ljava/lang/String;)Z") : nullptr;
    if (!m) return false;
    JStr u(j, ref);
    bool ok = j.env->CallBooleanMethod(j.act, m, u.s);
    return !j.failed("folderGranted") && ok;
#else
    return false;
#endif
}

std::string ref_name(const std::string &ref)
{
    if (!is_content(ref)) return ref;
    // content://<authority>/tree/<id>[/document/<id>]: the last id, %-decoded ("primary%3AMusic%2FSoundtracks"),
    // without its volume ("primary:")
    size_t at = ref.rfind('/');
    std::string d;
    for (size_t i = at + 1; i < ref.size(); ++i) {
        if (ref[i] == '%' && i + 2 < ref.size() && SDL_isxdigit(ref[i + 1]) && SDL_isxdigit(ref[i + 2])) {
            char h[3] = {ref[i + 1], ref[i + 2], 0};
            d += (char)SDL_strtol(h, nullptr, 16);
            i += 2;
        } else {
            d += ref[i];
        }
    }
    size_t colon = d.find(':');
    return colon == std::string::npos ? d : d.substr(colon + 1);
}

bool list(const std::string &folder, std::vector<Entry> &out, bool *missing, std::string *why)
{
    *missing = false;
    if (!is_content(folder)) return list_path(folder, out, missing, why);
#ifdef __ANDROID__
    // listFolder: "d" or "f", a tab, the name, a tab, the document's URI, one an entry; null: it cannot be read
    Jni j;
    jmethodID m = j ? j.method("listFolder", "(Ljava/lang/String;)[Ljava/lang/String;") : nullptr;
    if (!m) {
        *why = "no listFolder";
        return false;
    }
    JStr u(j, folder);
    jobjectArray a = static_cast<jobjectArray>(j.env->CallObjectMethod(j.act, m, u.s));
    if (j.failed("listFolder") || !a) {
        *why = "the system cannot list it";
        return false;
    }
    jsize n = j.env->GetArrayLength(a);
    for (jsize i = 0; i < n; ++i) {
        std::string row = j.string(static_cast<jstring>(j.env->GetObjectArrayElement(a, i)));
        size_t t1 = row.find('\t'), t2 = t1 == std::string::npos ? t1 : row.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        Entry e;
        e.folder = row[0] == 'd';
        e.name = row.substr(t1 + 1, t2 - t1 - 1);
        e.ref = row.substr(t2 + 1);
        out.push_back(std::move(e));
    }
    j.env->DeleteLocalRef(a);
    return true;
#else
    *why = "not a path";
    return false;
#endif
}

int open_fd(const std::string &ref, std::string *why)
{
    if (!is_content(ref)) {
        int fd = ::open(ref.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) *why = std::string("cannot read it (") + std::strerror(errno) + ")";
        return fd;
    }
#ifdef __ANDROID__
    Jni j;
    jmethodID m = j ? j.method("openFd", "(Ljava/lang/String;)I") : nullptr;
    if (!m) {
        *why = "no openFd";
        return -1;
    }
    JStr u(j, ref);
    jint fd = j.env->CallIntMethod(j.act, m, u.s);
    if (j.failed("openFd")) fd = -1;
    if (fd < 0) *why = "cannot read it (the chosen folder's grant may be gone)";
    return fd;
#else
    *why = "not a path";
    return -1;
#endif
}

}
