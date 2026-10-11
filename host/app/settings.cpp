#include "settings.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <clocale>

#include <SDL3/SDL.h>

#include "log.h"

namespace vrcore {
namespace settings {

namespace {

// One line of the file, in order. A pair has a key; a comment or blank line is kept as it is.
struct Line {
    bool pair = false;
    std::string key, value;     // a pair
    std::string text;           // a comment / blank line
};

std::string g_override, g_path;
std::vector<Line> g_lines;

Line *find(const char *key)
{
    for (Line &l : g_lines)
        if (l.pair && l.key == key) return &l;
    return nullptr;
}

bool valid_key(const char *key)
{
    if (!key || !*key || *key == '#') return false;
    for (const char *p = key; *p; ++p)
        if (*p == '=' || *p == '\n' || *p == '\r') return false;
    return true;
}

void parse(const char *data, size_t size)
{
    size_t at = 0;
    int n = 0;
    while (at < size) {
        size_t end = at;
        while (end < size && data[end] != '\n') ++end;
        std::string s(data + at, end - at);
        at = end + 1;
        ++n;
        if (!s.empty() && s.back() == '\r') s.pop_back();
        if (n == 1 && s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);     // a byte order mark
        size_t first = s.find_first_not_of(" \t");
        if (first == std::string::npos || s[first] == '#') {
            Line l;
            l.text = s;
            g_lines.push_back(l);
            continue;
        }
        size_t eq = s.find('=');
        size_t last = eq == std::string::npos ? 0 : s.find_last_not_of(" \t", eq - 1);
        if (eq == std::string::npos || eq <= first || last == std::string::npos || last < first) {
            log("settings: line %d is not key=value, skipped: %s", n, s.c_str());
            continue;
        }
        Line l;
        l.pair = true;
        l.key = s.substr(first, last - first + 1);
        l.value = s.substr(eq + 1);                     // spaces round the value are not part of it
        size_t vb = l.value.find_first_not_of(" \t"), ve = l.value.find_last_not_of(" \t");
        l.value = vb == std::string::npos ? "" : l.value.substr(vb, ve - vb + 1);
        if (Line *old = find(l.key.c_str())) old->value = l.value;     // a repeated key: the last wins
        else g_lines.push_back(l);
    }
}

bool write()
{
    if (g_path.empty()) return false;
    std::string out;
    for (const Line &l : g_lines) {
        if (l.pair) out += l.key + "=" + l.value + "\n";
        else out += l.text + "\n";
    }
    std::string dir = g_path.substr(0, g_path.find_last_of('/') == std::string::npos ? 0 : g_path.find_last_of('/'));
    if (!dir.empty()) SDL_CreateDirectory(dir.c_str());
    std::string tmp = g_path + ".tmp";
    SDL_IOStream *io = SDL_IOFromFile(tmp.c_str(), "wb");
    if (!io) {
        log("settings: cannot write %s: %s", tmp.c_str(), SDL_GetError());
        return false;
    }
    bool ok = SDL_WriteIO(io, out.data(), out.size()) == out.size();
    ok = SDL_CloseIO(io) && ok;
    if (ok) ok = SDL_RenamePath(tmp.c_str(), g_path.c_str());
    if (!ok) {
        log("settings: cannot save %s: %s", g_path.c_str(), SDL_GetError());
        SDL_RemovePath(tmp.c_str());
    }
    return ok;
}

bool put(const char *key, const std::string &value)
{
    if (!valid_key(key)) {
        log("settings: bad key \"%s\"", key ? key : "(null)");
        return false;
    }
    if (Line *old = find(key)) {
        old->value = value;
    } else {
        Line l;
        l.pair = true;
        l.key = key;
        l.value = value;
        g_lines.push_back(l);
    }
    return write();
}

bool drop(const char *key)
{
    for (size_t i = 0; i < g_lines.size(); ++i)
        if (g_lines[i].pair && g_lines[i].key == key) {
            g_lines.erase(g_lines.begin() + (long)i);
            return true;
        }
    return false;
}

}

void set_path(const char *file)
{
    g_override = file ? file : "";
}

bool open(const char *org, const char *app)
{
    g_lines.clear();
    g_path.clear();
    if (!g_override.empty()) {
        g_path = g_override;
    } else {
        char *pref = SDL_GetPrefPath(org, app);
        if (!pref) {
            log("settings: no preferences folder (%s): nothing will be saved", SDL_GetError());
            return false;
        }
        g_path = std::string(pref) + "settings.txt";
        SDL_free(pref);
    }
    size_t size = 0;
    void *data = SDL_LoadFile(g_path.c_str(), &size);
    if (data) {
        parse((const char *)data, size);
        SDL_free(data);
    }
    int keys = 0;
    for (const Line &l : g_lines) keys += l.pair;
    log("settings: %s, %d keys%s", g_path.c_str(), keys, data ? "" : " (no file yet)");
    return true;
}

const std::string &path() { return g_path; }

bool has(const char *key) { return find(key) != nullptr; }

int get_int(const char *key, int def)
{
    const Line *l = find(key);
    if (!l) return def;
    const char *b = l->value.data(), *e = b + l->value.size();
    int v = 0;
    auto r = std::from_chars(b, e, v);
    return r.ec == std::errc() && r.ptr == e ? v : def;
}

float get_float(const char *key, float def)
{
    const Line *l = find(key);
    if (!l) return def;
    // strtof reads the locale's decimal mark: the file's '.' is swapped for it (libc++ on Android has no float from_chars)
    std::string s = l->value;
    const char mark = *std::localeconv()->decimal_point;
    for (char &c : s)
        if (c == '.') c = mark;
    char *end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return !s.empty() && end == s.c_str() + s.size() ? v : def;
}

std::string get_string(const char *key, const char *def)
{
    const Line *l = find(key);
    return l ? l->value : std::string(def ? def : "");
}

bool get_bool(const char *key, bool def) { return get_int(key, def ? 1 : 0) != 0; }

bool set_int(const char *key, int v) { return put(key, std::to_string(v)); }

bool set_float(const char *key, float v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", (double)v);
    for (char *p = buf; *p; ++p)
        if (*p == ',') *p = '.';                // a locale's decimal comma
    return put(key, buf);
}

bool set_string(const char *key, const char *v)
{
    std::string s = v ? v : "";
    for (char &c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return put(key, s);
}

bool set_bool(const char *key, bool v) { return put(key, v ? "1" : "0"); }

bool erase(const char *key)
{
    if (!drop(key)) return true;
    return write();
}

void upgrade(const char *version_key, int version, const std::vector<Reset> &resets)
{
    int saved = get_int(version_key, 1);
    if (saved >= version) return;
    for (const Reset &r : resets)
        if (saved < r.since)
            for (const std::string &k : r.keys) drop(k.c_str());
    set_int(version_key, version);              // the one write
}

}
}
