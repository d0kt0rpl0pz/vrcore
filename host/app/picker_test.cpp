#include "picker_test.h"

#include <cstring>

#include <SDL3/SDL.h>

#include "log.h"

namespace vrcore {

namespace {

// The callback runs on Android's UI thread; it hands the URI over through these.
SDL_AtomicInt g_state;              // 0 idle, 1 open, 2 picked, 3 cancelled, 4 error, 5 done
char g_uri[2048];
SDL_ThreadID g_main_thread;

void SDLCALL on_pick(void *, const char *const *files, int filter)
{
    if (!files) {
        log("pick test: dialog error: %s", SDL_GetError());
        SDL_SetAtomicInt(&g_state, 4);
        return;
    }
    log("pick test: callback on thread %llu (SDL_main's: %llu), filter %d, first %s",
        (unsigned long long)SDL_GetCurrentThreadID(), (unsigned long long)g_main_thread, filter,
        files[0] ? files[0] : "(none: cancelled)");
    if (!files[0]) {
        SDL_SetAtomicInt(&g_state, 3);
        return;
    }
    SDL_strlcpy(g_uri, files[0], sizeof g_uri);
    SDL_SetAtomicInt(&g_state, 2);
}

}

void picker_test_start()
{
    g_main_thread = SDL_GetCurrentThreadID();
    SDL_SetAtomicInt(&g_state, 1);
    log("pick test: opening SDL's file dialog (no filters)");
    SDL_ShowOpenFileDialog(on_pick, nullptr, nullptr, nullptr, 0, nullptr, false);
}

void picker_test_poll()
{
    if (SDL_GetAtomicInt(&g_state) != 2) return;
    SDL_SetAtomicInt(&g_state, 5);

    SDL_IOStream *io = SDL_IOFromFile(g_uri, "rb");
    if (!io) {
        log("pick test: SDL_IOFromFile(%s) failed: %s", g_uri, SDL_GetError());
        return;
    }
    Sint64 size = SDL_GetIOSize(io);
    unsigned char head[4] = {}, tail[22] = {};
    size_t got = SDL_ReadIO(io, head, sizeof head);
    const char *kind = "unknown";
    if (got == 4 && !std::memcmp(head, "PK\x03\x04", 4)) kind = "zip";
    else if (got == 4 && !std::memcmp(head, "7z\xBC\xAF", 4)) kind = "7z";
    Sint64 at = size >= 22 ? SDL_SeekIO(io, -22, SDL_IO_SEEK_END) : -1;
    size_t tail_got = at >= 0 ? SDL_ReadIO(io, tail, sizeof tail) : 0;
    log("pick test: opened %s: size %lld, first bytes %02x %02x %02x %02x (%s), seek to end-22 -> %lld, read %u "
        "(zip end record: %s)",
        g_uri, (long long)size, head[0], head[1], head[2], head[3], kind, (long long)at, (unsigned)tail_got,
        tail_got == 22 && !std::memcmp(tail, "PK\x05\x06", 4) ? "yes" : "no");
    SDL_CloseIO(io);
}

}
