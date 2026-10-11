/*
 * settings_test.cpp -- the settings file's device-free checks, run by `--settings-test <dir>` before any host
 * starts. Each check starts from a file written by hand (or none) and reads it back through a fresh open(), as the
 * next start would. One line a check in <dir>/settings_test.txt and in the log, then the total.
 */
#include "settings_test.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "log.h"
#include "settings.h"

namespace vrcore::settings {

namespace {

struct Report {
    std::FILE *f = nullptr;
    int pass = 0, fail = 0;

    void check(bool ok, const char *fmt, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        ++(ok ? pass : fail);
        log("settings-test: %s %s", ok ? "PASS" : "FAIL", buf);
        if (f) std::fprintf(f, "%s %s\n", ok ? "PASS" : "FAIL", buf);
    }
};

std::string slurp(const std::string &file)
{
    size_t size = 0;
    void *d = SDL_LoadFile(file.c_str(), &size);
    if (!d) return "<none>";
    std::string s((const char *)d, size);
    SDL_free(d);
    return s;
}

void spit(const std::string &file, const std::string &text)
{
    SDL_IOStream *io = SDL_IOFromFile(file.c_str(), "wb");
    if (!io) return;
    SDL_WriteIO(io, text.data(), text.size());
    SDL_CloseIO(io);
}

bool bits_equal(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// a fresh start: the file as it is on disk
void reopen(const std::string &file)
{
    set_path(file.c_str());
    open("vrcore", "settings-test");      // set_path gives the file: no folder is made
}

void fresh(const std::string &file)
{
    SDL_RemovePath(file.c_str());
    SDL_RemovePath((file + ".tmp").c_str());
    reopen(file);
}

}

int settings_test(const char *dir)
{
    SDL_CreateDirectory(dir);
    const std::string d = dir, file = d + "/settings.txt", tmp = file + ".tmp";
    Report r;
    r.f = std::fopen((d + "/settings_test.txt").c_str(), "w");

    // a missing file loads empty
    fresh(file);
    r.check(!has("dy.volume") && get_int("dy.volume", 77) == 77 && get_float("x", 1.5f) == 1.5f &&
                get_string("x", "d") == "d" && get_bool("x", true),
            "missing file: nothing saved, the defaults come back");
    r.check(path() == file, "the path is the one given");

    // every type, read back by a fresh load
    const float floats[] = {0.1f, 1e-7f, -0.f, 0.f, 3.4028235e38f, 1.17549435e-38f, -123456.789f, 1.f / 3.f, 2.5f};
    r.check(set_int("i.pos", 100) && set_int("i.neg", -60) && set_int("i.max", 2147483647) &&
                set_int("i.min", -2147483647 - 1) && set_bool("b.t", true) && set_bool("b.f", false) &&
                set_string("s.plain", "hello") && set_string("s.sp", "a b = c # d") && set_string("s.empty", "") &&
                set_string("s.utf8", "caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x8F\x8E") && set_string("s.nl", "a\nb"),
            "set_int / set_bool / set_string succeed");
    for (size_t i = 0; i < sizeof floats / sizeof *floats; ++i) set_float(("f." + std::to_string(i)).c_str(), floats[i]);
    reopen(file);
    r.check(get_int("i.pos", 0) == 100 && get_int("i.neg", 0) == -60 && get_int("i.max", 0) == 2147483647 &&
                get_int("i.min", 0) == -2147483647 - 1,
            "ints read back (100, -60, INT_MAX, INT_MIN)");
    r.check(get_bool("b.t", false) && !get_bool("b.f", true), "bools read back");
    r.check(get_string("s.plain", "x") == "hello" && get_string("s.sp", "x") == "a b = c # d" &&
                has("s.empty") && get_string("s.empty", "x").empty() &&
                get_string("s.utf8", "x") == "caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x8F\x8E" &&
                get_string("s.nl", "x") == "a b",
            "strings read back (spaces, = and #, empty, UTF-8, a newline made a space)");
    bool all = true;
    for (size_t i = 0; i < sizeof floats / sizeof *floats; ++i) {
        float g = get_float(("f." + std::to_string(i)).c_str(), 42.f);
        if (!bits_equal(g, floats[i])) {
            all = false;
            r.check(false, "float %zu: wrote %.9g, read %.9g", i, (double)floats[i], (double)g);
        }
    }
    r.check(all, "floats read back bit-exact (0.1, 1e-7, -0, 0, FLT_MAX, FLT_MIN, -123456.789, 1/3, 2.5)");
    r.check(std::signbit(get_float("f.2", 1.f)) && get_float("f.2", 1.f) == 0.f, "-0 keeps its sign");
    r.check(get_int("s.plain", 9) == 9 && get_float("s.plain", 9.f) == 9.f, "a non-number reads as the default");
    r.check(get_int("f.0", 9) == 9, "a float is not an int (the default)");
    r.check(!has("never"), "has() false for an unsaved key");

    // overwrite keeps one line
    set_int("i.pos", 50);
    reopen(file);
    std::string text = slurp(file);
    size_t first = text.find("i.pos="), last = text.rfind("i.pos=");
    r.check(get_int("i.pos", 0) == 50 && first == last, "a changed key is one line, the new value");

    // unknown keys and comments survive a write; bad lines are skipped; CRLF, spaces
    spit(file, "# header comment\r\n\r\ndy.volume = 40\r\nfuture.key=xyz\n   # indented comment\nnot a pair\n=novalue\n"
               "dy.music=30\ndy.music=35\n# trailing\n");
    reopen(file);
    r.check(get_int("dy.volume", 0) == 40 && get_string("future.key", "") == "xyz" && get_int("dy.music", 0) == 35,
            "spaces round the key, CRLF, an unknown key and a repeated key (the last wins) load");
    set_int("dy.effects", 20);
    reopen(file);
    text = slurp(file);
    r.check(text.find("# header comment\n") == 0 && text.find("# indented comment") != std::string::npos &&
                text.find("# trailing\n") != std::string::npos && get_string("future.key", "") == "xyz" &&
                get_int("dy.effects", 0) == 20,
            "comments and the unknown key survive a write");
    r.check(text.find("not a pair") == std::string::npos && text.find("novalue") == std::string::npos &&
                text.find('\r') == std::string::npos,
            "malformed lines are dropped at the write, line ends are \\n");
    r.check(text.find("# header comment") < text.find("dy.volume") && text.find("dy.volume") < text.find("dy.effects"),
            "order kept, a new key at the end");

    // atomic write: no temp file left; a stale temp from a crash is replaced; the old file is whole at every step
    spit(tmp, "half a fi");
    set_int("dy.hills", 1);
    r.check(slurp(tmp) == "<none>", "the temp file is gone after a write (also a stale one)");
    text = slurp(file);
    r.check(!text.empty() && text.back() == '\n' && text.find("dy.hills=1\n") != std::string::npos,
            "the file is whole and ends in a newline");
    SDL_PathInfo info;
    r.check(SDL_GetPathInfo(file.c_str(), &info) && info.type == SDL_PATHTYPE_FILE &&
                !SDL_GetPathInfo(tmp.c_str(), nullptr),
            "settings.txt is a file, settings.txt.tmp does not exist");
    // a write that cannot happen leaves the old file untouched
    std::string before = slurp(file);
    {
        // a path under a plain file: the folder cannot be made
        spit(d + "/plain", "x");
        set_path((d + "/plain/settings.txt").c_str());
        open("a", "b");
        bool ok = set_int("k", 1);
        r.check(!ok && slurp(file) == before, "a failed write reports false and leaves the other file whole");
    }

    // erase
    reopen(file);
    r.check(has("dy.hills") && erase("dy.hills") && !has("dy.hills") && erase("never.there"), "erase");
    reopen(file);
    r.check(!has("dy.hills") && get_int("dy.volume", 0) == 40 && get_string("future.key", "") == "xyz",
            "an erased key stays gone after a reload, the rest stays");

    // upgrade: version 1 -> 3, resets since 2 and 3
    fresh(file);
    set_int("a", 1);
    set_int("b", 2);
    set_int("c", 3);
    set_int("d", 4);
    const std::vector<Reset> resets = {{2, {"a", "b"}}, {3, {"c"}}};
    upgrade("v", 3, resets);
    reopen(file);
    r.check(!has("a") && !has("b") && !has("c") && has("d") && get_int("v", 0) == 3,
            "upgrade 1 -> 3: both resets' keys erased, others kept, version stored");
    set_int("a", 9);
    upgrade("v", 3, resets);
    reopen(file);
    r.check(get_int("a", 0) == 9, "a second upgrade to the same version is a no-op");
    upgrade("v", 2, resets);
    reopen(file);
    r.check(get_int("a", 0) == 9 && get_int("v", 0) == 3, "a saved version above is left alone");
    // 2 -> 3 drops only the since-3 keys
    fresh(file);
    set_int("v", 2);
    set_int("a", 1);
    set_int("c", 3);
    upgrade("v", 3, resets);
    reopen(file);
    r.check(has("a") && !has("c") && get_int("v", 0) == 3, "upgrade 2 -> 3: only the since-3 keys erased");
    // nothing saved at all: the version key counts as 1
    fresh(file);
    upgrade("v", 2, {{2, {"z"}}});
    reopen(file);
    r.check(get_int("v", 0) == 2, "upgrade on an empty file stores the version");

    // leave the global state as a real run would not find it
    set_path(nullptr);
    log("settings-test: %d passed, %d failed", r.pass, r.fail);
    if (r.f) {
        std::fprintf(r.f, "total %d passed, %d failed\n", r.pass, r.fail);
        std::fclose(r.f);
    }
    return r.fail == 0 ? 0 : 1;
}

}
