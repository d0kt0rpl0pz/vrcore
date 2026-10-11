/*
 * vr_plugin_test.c -- a game plugin's C API (abi/vrcore.h) driven the way a host drives
 * it, with no host: a headless check of the plugin, on the desktop or on a Quest over
 * adb (quest_bench.sh). Built by each game's CMake as its own target, from VRCORE_DIR.
 *
 *   vr_plugin_test <name.bundle | libname.so> <rom_dir> [seconds] [shots_dir]
 *
 * Settings come from the environment as VR_TEST_<name>.
 *   VR_TEST_ARGS="--perf --replay f.rec"   the game's own options for vr_start (Rave Racer:
 *                                        --perf prints frame-time percentiles when each
 *                                        pass stops; --replay plays a recording)
 *   VR_TEST_HASH=<prefix>                every new frame's FNV-1a 64 hash, one "frame hash" line
 *                                        each, into <prefix>1.txt (the flat picture) and
 *                                        <prefix>2.txt (vertices, indices and every table the
 *                                        frame carries). With a replay the frames are the
 *                                        recording's, so two builds' files agree on every
 *                                        frame both fetched: a refactor's proof
 *   VR_TEST_INTERP=1                     the 3D export with frame interpolation's matching,
 *                                        its stats and census (below)
 *   VR_TEST_LEVEL=<0..3>                 the level camera's option (VR_OPT_LEVEL), and its census
 *   VR_TEST_OPT="9=1,12=6"               options (key=value) set before start; each one's
 *                                        value in force is printed when the pass stops
 *   VR_TEST_EXPLORE=<seconds>            explore mode (VR_OPT_EXPLORE) on at that time for 4 s,
 *                                        then off (pass 2): it must freeze, and its census
 *   VR_TEST_REPAINT=<seconds>            (pass 2) paused at that time, the draw distance
 *                                        (VR_OPT_DRAW_EXTRA) changed to +24 while paused: the
 *                                        game must run exactly the repaint's 24 frames and hold
 *                                        again, and the frame held must have the extra quads in
 *                                        (at least 90% of a frame 30 frames after it runs on)
 *   VR_TEST_MIRROR=1                     (pass 2) the sub-window quads' census (VR_V_SUBWIN):
 *                                        their depths, for a mirror's stereo in VR (below)
 *   VR_TEST_FOV=<frame> / VR_TEST_LOOK=<frame>  censuses of the geometry sent (below)
 *
 * Pass 1 takes the flat picture (vr_picture; the game's option --sw), pass 2 the 3D
 * frames (vr_frame). Like a host it loads a fresh COPY of the
 * plugin's binary (twice, one after the other, to prove a second Play in the same
 * process starts clean), then: start, a coin at 2 s, gas held from 4 s (it starts the
 * game), audio pulled on a thread the way OnAudioFilterRead does at 48 kHz, the frame
 * copied out 60 times a second, a PPM every 5 s, stop. Prints the game's frame and its
 * three health counters (vr_stats; Rave Racer's traps, unmapped, ROM writes), audio
 * underruns and the picture's content, and exits 0 only if the run was clean.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include "../abi/vrcore.h"

typedef int      (*fn_version)(void);
typedef int      (*fn_start)(const char *, int, const char *);
typedef int      (*fn_state)(void);
typedef int      (*fn_stop)(int);
typedef void     (*fn_controls)(uint32_t, int, int, int, int, int, int);
typedef uint32_t (*fn_frame)(uint8_t *, int, uint32_t, int *, int *);
typedef int      (*fn_audio)(float *, int, int);
typedef void     (*fn_stats)(uint32_t *, uint32_t *, uint32_t *, uint32_t *);
typedef uint32_t (*fn_vrframe)(uint32_t, uint32_t, float *, int, uint32_t *, int, uint8_t *const *, int *, int *, uint32_t *, int64_t *, int64_t *);
typedef void     (*fn_istats)(double *);
typedef int      (*fn_int)(void);
typedef void     (*fn_world)(int, int);
typedef void     (*fn_setopt)(int, int);
typedef const char *(*fn_name)(void);
typedef int      (*fn_size)(int);

static struct { fn_version version; fn_start start; fn_state state; fn_stop stop; fn_controls controls;
                fn_frame frame; fn_audio audio; fn_stats stats; fn_vrframe vrframe; fn_int renderer; fn_world world; fn_istats istats;
                fn_int vfloats; fn_name game; fn_int ntables; fn_size table_size; } api;
static atomic_int audio_run;
static double audio_rms_sum; static long audio_n, audio_nonzero;

/* VR_TEST_<name> */
static const char *env(const char *name)
{
    char k[64]; snprintf(k, sizeof k, "VR_TEST_%s", name);
    return getenv(k);
}

/* floats a vertex (vr_vertex_floats: the board's layout). The censuses read only the slots
 * every board shares (abi/vrcore.h vr_vert, by its VR_VERT_* offsets): position, flags, clip /
 * focal, the previous corner; Rave Racer's own checks read its board block */
static int VF;
#define VF_SHARED VR_VERT_FLOATS
#define MAX_TABLES 16
static int ntab, tab_size[MAX_TABLES];
static uint8_t *tab[MAX_TABLES];
static int rr;                                  /* the plugin is Rave Racer's (vr_game "raverace") */
static int scr_w = 640, scr_h = 480;            /* the flat picture's size, from pass 1 */

/* vr_frame with the board's tables (tab[]); *present: which came with the frame */
static uint32_t quads(uint32_t have, uint32_t want, float *v, int vcap, uint32_t *idx, int icap,
                      int *nv, int *ni, uint32_t *present, int64_t *t_ns, int64_t *ready_ns)
{
    *present = 0;
    return api.vrframe(have, want, v, vcap, idx, icap, tab, nv, ni, present, t_ns, ready_ns);
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void *audio_thread(void *arg)
{
    (void)arg;
    float buf[1024 * 2];
    double t0 = now_s(); long pulled = 0;
    while (atomic_load(&audio_run)) {           /* 1024 frames at 48 kHz, on the audio clock */
        api.audio(buf, 1024, 2);
        for (int i = 0; i < 2048; i++) { audio_rms_sum += (double)buf[i] * buf[i]; if (buf[i] != 0) audio_nonzero++; }
        audio_n += 2048; pulled += 1024;
        const double due = t0 + pulled / 48000.0 - now_s();
        if (due > 0) usleep((useconds_t)(due * 1e6));
    }
    return NULL;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

/* A 3D frame's hash: its vertices, its indices, then its tables. Any board: the present
 * mask, then every table that came. Rave Racer keeps the layout its hash files have always
 * had (its four tables: palette and post table always come, the text and shadow
 * layers when drawn): palette, which layers came, those layers, post table -- so its files
 * from before the test moved here still compare. */
static uint64_t frame_hash(const float *v, int nv, const uint32_t *idx, int ni, uint32_t present)
{
    uint64_t x = fnv(0xcbf29ce484222325ull, v, (size_t)nv * VF * sizeof *v);
    x = fnv(x, idx, (size_t)ni * sizeof *idx);
    if (rr && ntab == 4) {
        const int layers = (int)(present >> 1 & 3);
        x = fnv(x, tab[0], (size_t)tab_size[0]); x = fnv(x, &layers, sizeof layers);
        if (layers & 1) x = fnv(x, tab[1], (size_t)tab_size[1]);
        if (layers & 2) x = fnv(x, tab[2], (size_t)tab_size[2]);
        return fnv(x, tab[3], (size_t)tab_size[3]);
    }
    x = fnv(x, &present, sizeof present);
    for (int i = 0; i < ntab; i++) if (present >> i & 1) x = fnv(x, tab[i], (size_t)tab_size[i]);
    return x;
}

static int write_ppm(const char *p, const uint8_t *rgba, int w, int h)
{
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)                /* bottom row first -> top first */
        for (int x = 0; x < w; x++) fwrite(rgba + ((size_t)y * w + x) * 4, 1, 3, f);
    fclose(f);
    return 1;
}

/* VR_TEST_FOV=<first frame>: THE VR QUESTION -- how far to the side does the geometry the game
 * sends reach? Over the frame pass (flat, no 3D export), every vertex of a full-screen
 * viewport's quads (not the mirror or other sub-windows), as a horizontal / vertical angle
 * from the view axis: atan(offset / focal length), focal length VR_TEST_FOCAL px (default 554:
 * Rave Racer's race camera, ~60 degrees across 640). The screen is pass 1's picture. Rave
 * Racer's vertices are near- and guard-band-clipped (+-4096 px, 82 deg). */
static double fov_h[181], fov_v[181], fov_n;
static double fov_frame_max[20000]; static int fov_frames;
static void fov_census(const float *qv, int nv)
{
    static double f = 0; if (!f) { const char *e = env("FOCAL"); f = e ? atof(e) : 554.0; }
    double fmax = 0;
    for (int i = 0; i < nv; i++) {
        const float *v = qv + (size_t)i * VF;
        if (!(v[VR_VERT_CLIP] <= 0 && v[VR_VERT_CLIP + 1] >= scr_w && v[VR_VERT_CLIP + 2] <= 0 && v[VR_VERT_CLIP + 3] >= scr_h)) continue;   /* full-frame clip only */
        const double ah = atan((v[0] - scr_w / 2.0) / f) * 180 / M_PI, av = atan((scr_h / 2.0 - v[1]) / f) * 180 / M_PI;
        fov_h[(int)lround(ah) + 90]++; fov_v[(int)lround(av) + 90]++; fov_n++;
        if (fabs(ah) > fmax) fmax = fabs(ah);
    }
    if (fov_frames < 20000) fov_frame_max[fov_frames++] = fmax;
}
static int cmpd(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void fov_report(void)
{
    if (!fov_n) return;
    printf("FOV census: %.0f vertices over %d frames (focal %s px)\n", fov_n, fov_frames, env("FOCAL") ? env("FOCAL") : "554");
    printf("  |angle| beyond   horizontal   vertical\n");
    for (int a = 30; a <= 80; a += 10) {
        double h = 0, v = 0;
        for (int k = 0; k <= 180; k++) if (abs(k - 90) > a) { h += fov_h[k]; v += fov_v[k]; }
        printf("  %3d deg         %8.2f%%   %8.2f%%\n", a, 100 * h / fov_n, 100 * v / fov_n);
    }
    qsort(fov_frame_max, fov_frames, sizeof *fov_frame_max, cmpd);
    printf("  widest vertex per frame: p10 %.0f  p50 %.0f  p90 %.0f deg\n", fov_frame_max[fov_frames / 10],
           fov_frame_max[fov_frames / 2], fov_frame_max[fov_frames * 9 / 10]);
    printf("  horizontal histogram, 5-degree bins from -90 (share of vertices, %%):\n  ");
    for (int b = 0; b < 36; b++) { double h = 0; for (int k = b * 5; k < b * 5 + 5; k++) h += fov_h[k]; printf("%.1f ", 100 * h / fov_n); }
    printf("\n");
}

/* VR_TEST_LOOK=<first frame>: THE LOOKING-AROUND QUESTION -- turn your head in VR: is
 * there anything there? The engine-quad pass runs in 3D (vr_set_world(1, 1): world
 * quads as view-space corners, quads behind the game camera kept); every 4th frame or
 * so from the first, rays from the game camera -- 72 yaws (5 degrees) x 24 pitches
 * (-57.5 .. +57.5) -- are cast at every triangle. Race frames only: those with the race
 * sky (the attract demo's is nearer, menus have none). A ray hits "world" (track,
 * scenery, cars) or "sky" (a quad whose nearest corner is beyond 1.5M units: Rave Racer's
 * race sky is ~2.1M out); "none" is a hole. And PAINTER'S ORDER: the headset draws the mesh
 * in its order with no depth test, so the quad drawn last on a ray is what you see --
 * counted when it is not the nearest. Reported by |yaw| from the camera's heading. The
 * options under test go in the environment (RR_DRAW_BEHIND, RR_CARS_ALL_ROUND, ...). */
#define LOOK_YAW 72
#define LOOK_PITCH 24
static double look_world[LOOK_YAW], look_sky[LOOK_YAW], look_any[LOOK_YAW], look_rays[LOOK_YAW], look_skyover[LOOK_YAW], look_wrong[LOOK_YAW];
static double look_quads_sum; static int look_quads_max, look_frames;
static float look_dir[LOOK_YAW * LOOK_PITCH][3];
static float ray_tri(const float *d, const float *a, const float *b, const float *c)   /* Moller-Trumbore, origin 0: t, or -1 */
{
    const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    const float p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
    const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (fabsf(det) < 1e-12f) return -1;
    const float inv = 1.0f / det, t0[3] = { -a[0], -a[1], -a[2] };
    const float u = (t0[0] * p[0] + t0[1] * p[1] + t0[2] * p[2]) * inv;
    if (u < 0 || u > 1) return -1;
    const float q[3] = { t0[1] * e1[2] - t0[2] * e1[1], t0[2] * e1[0] - t0[0] * e1[2], t0[0] * e1[1] - t0[1] * e1[0] };
    const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0 || u + v > 1) return -1;
    const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return t > 0 ? t : -1;
}
static void look_census(const float *qv, int nv)
{
    if (!look_dir[0][2]) for (int y = 0; y < LOOK_YAW; y++) for (int p = 0; p < LOOK_PITCH; p++) {
        const double yaw = (y * 5 - 180 + 2.5) * M_PI / 180, pit = (p * 5 - 60 + 2.5) * M_PI / 180;
        float *d = look_dir[y * LOOK_PITCH + p];
        d[0] = (float)(sin(yaw) * cos(pit)); d[1] = (float)sin(pit); d[2] = (float)(cos(yaw) * cos(pit));
    }
    static unsigned char hit[LOOK_YAW * LOOK_PITCH];
    /* painter's order (the mesh's, no depth test) against the truth (the nearest hit):
     * the quad drawn last on each ray, and the nearest one */
    static int last_q[LOOK_YAW * LOOK_PITCH], near_q[LOOK_YAW * LOOK_PITCH]; static float near_t[LOOK_YAW * LOOK_PITCH];
    static unsigned char qsky[200000 / 4];
    memset(hit, 0, sizeof hit);
    for (int r = 0; r < LOOK_YAW * LOOK_PITCH; r++) { last_q[r] = near_q[r] = -1; near_t[r] = 1e30f; }
    int quads = 0, skyq = 0;
    for (int i = 0; i + 3 < nv; i += 4) {                 /* quads come as 4 vertices, 2 triangles */
        const float *v0 = qv + (size_t)i * VF;
        if (!((int)v0[VR_VERT_FLAGS] & VR_V_3D)) continue;             /* 3D only */
        const float *c[4];
        double near = 1e30;
        for (int k = 0; k < 4; k++) {
            c[k] = qv + (size_t)(i + k) * VF;
            const double r = sqrt((double)c[k][0] * c[k][0] + (double)c[k][1] * c[k][1] + (double)c[k][2] * c[k][2]);
            if (r < near) near = r;
        }
        const unsigned char bit = near > 1.5e6 ? 2 : 1;
        if (bit == 1) quads++; else skyq++;
        qsky[i / 4] = bit == 2;
        for (int r = 0; r < LOOK_YAW * LOOK_PITCH; r++) {
            float t = ray_tri(look_dir[r], c[0], c[1], c[2]);
            if (t < 0) t = ray_tri(look_dir[r], c[0], c[2], c[3]);
            if (t < 0) continue;
            hit[r] |= bit; last_q[r] = i / 4;
            if (t < near_t[r]) { near_t[r] = t; near_q[r] = i / 4; }
        }
    }
    if (!quads || !skyq) return;                          /* not a race frame */
    /* VR_TEST_LOOK_MAP=<every n census frames>: the frame as a map, yaw -180 (left) .. +180
     * across, pitch +60 at the top: # world, ~ sky, . nothing; painter's order wrong:
     * S sky drawn over nearer world, x another surface drawn over the nearest */
    static int map_every = -2; if (map_every == -2) { const char *e = env("LOOK_MAP"); map_every = e ? atoi(e) : 0; }
    if (map_every > 0 && look_frames % map_every == 0) {
        int holes = 0; for (int r = 0; r < LOOK_YAW * LOOK_PITCH; r++) holes += !hit[r];
        printf("LOOK map, census frame %d: %d world quads, %d holes (ahead is the middle column)\n", look_frames, quads, holes);
        for (int p = LOOK_PITCH - 1; p >= 0; p--) {
            char row[LOOK_YAW + 1];
            for (int y = 0; y < LOOK_YAW; y++) {
                const int r = y * LOOK_PITCH + p, h = hit[r];
                row[y] = !h ? '.' : last_q[r] != near_q[r] ? (qsky[last_q[r]] && !qsky[near_q[r]] ? 'S' : 'x') : h & 1 && !qsky[last_q[r]] ? '#' : '~';
            }
            row[LOOK_YAW] = 0; printf("  %s\n", row);
        }
    }
    for (int r = 0; r < LOOK_YAW * LOOK_PITCH; r++) {
        const int y = r / LOOK_PITCH;
        look_rays[y]++; look_world[y] += hit[r] & 1; look_sky[y] += (hit[r] >> 1) & 1; look_any[y] += hit[r] != 0;
        if (last_q[r] >= 0 && last_q[r] != near_q[r]) {
            if (qsky[last_q[r]] && !qsky[near_q[r]]) look_skyover[y]++;     /* sky painted over the world: looks missing */
            else look_wrong[y]++;                                           /* another surface painted over the nearest */
        }
    }
    look_quads_sum += quads; if (quads > look_quads_max) look_quads_max = quads; look_frames++;
}
static void look_report(void)
{
    if (!look_frames) return;
    printf("LOOK census: %d frames, world quads a frame mean %.0f max %d\n", look_frames, look_quads_sum / look_frames, look_quads_max);
    printf("  |yaw|      world    sky   neither (a hole)   painter's order: sky over world, other wrong\n");
    static const int band[] = { 0, 30, 60, 90, 120, 150, 180 };
    for (int b = 0; b < 6; b++) {
        double w = 0, s = 0, a = 0, n = 0, so = 0, wr = 0;
        for (int y = 0; y < LOOK_YAW; y++) {
            const double mid = fabs(y * 5 - 180 + 2.5);
            if (mid >= band[b] && mid < band[b + 1]) { w += look_world[y]; s += look_sky[y]; a += look_any[y]; n += look_rays[y]; so += look_skyover[y]; wr += look_wrong[y]; }
        }
        printf("  %3d-%-3d  %5.1f%%  %5.1f%%  %5.1f%%             %5.1f%%        %5.1f%%\n", band[b], band[b + 1], 100 * w / n, 100 * s / n, 100 * (n - a) / n, 100 * so / n, 100 * wr / n);
    }
}

/* VR_TEST_INTERP=1: of the 3D vertices sent, the share carrying a previous position that
 * differs from the current one (a matched, moving object), and how far they move in one
 * frame, as an angle seen from the game camera: what the interpolation spreads out. */
static double ic_frames, ic_verts, ic_moving; static double ic_ang[20000]; static int ic_n;
/* AT ALPHA 0 THE MESH IS THE LAST FRAME: a matched corner's previous position is exactly
 * a corner of the frame before (the same object's same quad, the same arithmetic), unless
 * the match is across codes (level of detail) or the quad was culled then. Checked for
 * every consecutive pair of frames: the last frame's 3D corners in a hash set. */
#define HB (1 << 18)
static float hk[HB][3]; static uint32_t hgen[HB], gen;
static double ic_checked, ic_found; static uint32_t ic_last;
static uint32_t hpos(const float *p) { uint32_t h; memcpy(&h, &p[0], 4); uint32_t a, b; memcpy(&a, &p[1], 4); memcpy(&b, &p[2], 4); h = h * 2654435761u ^ a * 2246822519u ^ b * 3266489917u; return h & (HB - 1); }
static void interp_census(const float *qv, int nv, uint32_t f)
{
    double fmax = 0;
    if (f == ic_last + 1)
        for (int i = 0; i < nv; i++) {
            const float *v = qv + (size_t)i * VF;
            if (!((int)v[VR_VERT_FLAGS] & VR_V_3D) || (v[0] == v[VR_VERT_PREV] && v[1] == v[VR_VERT_PREV + 1] && v[2] == v[VR_VERT_PREV + 2])) continue;
            ic_checked++;
            for (uint32_t h = hpos(v + VR_VERT_PREV); hgen[h] == gen; h = (h + 1) & (HB - 1))
                if (hk[h][0] == v[VR_VERT_PREV] && hk[h][1] == v[VR_VERT_PREV + 1] && hk[h][2] == v[VR_VERT_PREV + 2]) { ic_found++; break; }
        }
    gen++; ic_last = f;
    for (int i = 0; i < nv; i++) {
        const float *v = qv + (size_t)i * VF;
        if (!((int)v[VR_VERT_FLAGS] & VR_V_3D)) continue;
        uint32_t h = hpos(v);
        while (hgen[h] == gen && !(hk[h][0] == v[0] && hk[h][1] == v[1] && hk[h][2] == v[2])) h = (h + 1) & (HB - 1);
        hgen[h] = gen; memcpy(hk[h], v, 12);
    }
    for (int i = 0; i < nv; i++) {
        const float *v = qv + (size_t)i * VF;
        if (!((int)v[VR_VERT_FLAGS] & VR_V_3D)) continue;
        ic_verts++;
        const double c[3] = { v[0], v[1], v[2] }, p[3] = { v[VR_VERT_PREV], v[VR_VERT_PREV + 1], v[VR_VERT_PREV + 2] };
        if (c[0] == p[0] && c[1] == p[1] && c[2] == p[2]) continue;
        ic_moving++;
        const double lc = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), lp = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (lc < 1 || lp < 1) continue;
        double d = (c[0] * p[0] + c[1] * p[1] + c[2] * p[2]) / (lc * lp); if (d > 1) d = 1;
        const double a = acos(d) * 180 / M_PI;
        if (a > fmax) fmax = a;
    }
    ic_frames++;
    if (ic_n < 20000) ic_ang[ic_n++] = fmax;
}
static void interp_report(void)
{
    if (!ic_frames) return;
    printf("INTERP census: %.0f frames, %.0f 3D vertices a frame, %.1f%% with a different previous position\n",
           ic_frames, ic_verts / ic_frames, ic_verts ? 100 * ic_moving / ic_verts : 0);
    qsort(ic_ang, ic_n, sizeof *ic_ang, cmpd);
    printf("  previous positions that are a corner of the frame before: %.3f%% of %.0f (consecutive frames)\n",
           ic_checked ? 100 * ic_found / ic_checked : 0, ic_checked);
    printf("  a frame's largest move of one vertex, seen from the camera: p50 %.2f  p90 %.2f  p99 %.2f  max %.2f deg\n",
           ic_ang[ic_n / 2], ic_ang[ic_n * 9 / 10], ic_ang[ic_n * 99 / 100], ic_ang[ic_n - 1]);
}

/* VR_TEST_LEVEL=<mask>: THE LEVEL CAMERA (VR_OPT_LEVEL: VR_LEVEL_ROLL, VR_LEVEL_PITCH taken
 * out). Walls, posts and buildings have edges that are vertical in the world; in the
 * camera's view space they lean by its roll (seen along the view axis) and its pitch (seen
 * from the side). Per frame: the median lean of the 3D edges within 20 degrees of view-space
 * up, frames with at least 20 such edges; over the frames, the spread of those medians.
 * Levelled, the leans the option takes out are 0; the game's camera shows its tilt. */
static double lv_roll[20000], lv_pitch[20000]; static int lv_n;
static int cmpd_abs(const void *a, const void *b) { const double x = fabs(*(const double *)a), y = fabs(*(const double *)b); return x < y ? -1 : x > y; }
static void level_census(const float *qv, int nv)
{
    static double r[40000], p[40000]; int n = 0;
    for (int i = 0; i + 3 < nv && n < 40000; ) {
        const float *v = qv + (size_t)i * VF;
        if (!((int)v[VR_VERT_FLAGS] & VR_V_3D)) { i++; continue; }    /* a 3D quad: four vertices in a row */
        for (int e = 0; e < 4 && n < 40000; e++) {
            const float *a = qv + (size_t)(i + e) * VF, *b = qv + (size_t)(i + (e + 1) % 4) * VF;
            double d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l < 64) continue;
            if (d[1] < 0) for (int k = 0; k < 3; k++) d[k] = -d[k];
            if (d[1] / l < cos(20 * M_PI / 180)) continue;
            r[n] = atan2(d[0], d[1]) * 180 / M_PI; p[n] = atan2(d[2], d[1]) * 180 / M_PI; n++;
        }
        i += 4;
    }
    if (n < 20 || lv_n >= 20000) return;
    qsort(r, n, sizeof *r, cmpd); qsort(p, n, sizeof *p, cmpd);
    lv_roll[lv_n] = r[n / 2]; lv_pitch[lv_n] = p[n / 2]; lv_n++;
}
static void level_report(int mask)
{
    if (!lv_n) { printf("LEVEL census (option %d): no frames\n", mask); return; }
    qsort(lv_roll, lv_n, sizeof *lv_roll, cmpd_abs); qsort(lv_pitch, lv_n, sizeof *lv_pitch, cmpd_abs);
    printf("LEVEL census (option %d): %d frames; |median lean| of vertical edges, p50 / p90 / p99 / max deg:\n", mask, lv_n);
    printf("  roll  %.2f / %.2f / %.2f / %.2f\n", fabs(lv_roll[lv_n / 2]), fabs(lv_roll[lv_n * 9 / 10]), fabs(lv_roll[lv_n * 99 / 100]), fabs(lv_roll[lv_n - 1]));
    printf("  pitch %.2f / %.2f / %.2f / %.2f\n", fabs(lv_pitch[lv_n / 2]), fabs(lv_pitch[lv_n * 9 / 10]), fabs(lv_pitch[lv_n * 99 / 100]), fabs(lv_pitch[lv_n - 1]));
}

/* EXPLORE (VR_TEST_EXPLORE): the frozen frame -- its 3D quads, how many are explore quads,
 * one-sided (with a unit normal) and on each coplanar layer -- and whether the game held
 * still while frozen and ran on afterwards. */
static int ex_frozen_frame, ex_quads, ex_explore, ex_onesided, ex_badnormal, ex_layers[4];
static void explore_census(const float *qv, int nv)
{
    ex_quads = ex_explore = ex_onesided = ex_badnormal = 0; memset(ex_layers, 0, sizeof ex_layers);
    for (int v = 0; v + 3 < nv; ) {
        const float *q = qv + (size_t)v * VF;
        const int fl = (int)q[VR_VERT_FLAGS];
        if (!(fl & VR_V_3D)) { v++; continue; }         /* 3D quads come as four vertices */
        ex_quads++;
        if (fl & VR_V_EXPLORE) {
            ex_explore++;
            ex_layers[(fl >> VR_V_LAYER_SHIFT) & VR_V_LAYER_MAX]++;
            if (fl & VR_V_ONESIDED) {
                ex_onesided++;
                const double l = sqrt((double)q[VR_VERT_PREV] * q[VR_VERT_PREV] + (double)q[VR_VERT_PREV + 1] * q[VR_VERT_PREV + 1] + (double)q[VR_VERT_PREV + 2] * q[VR_VERT_PREV + 2]);
                if (fabs(l - 1) > 1e-3) ex_badnormal++;
            }
        }
        v += 4;
    }
}

/* MIRROR census (VR_TEST_MIRROR=1): the sub-window quads (VR_V_SUBWIN -- Rave Racer's rear-view
 * mirror's picture, the countdown sign) a frame, and their depths, which set each eye's shift
 * of the mirror in VR: the share of the far shift a
 * vertex gets at Rave Racer's default world scale (0.0025 m/unit) and HUD distance (1.5 m). */
static double mi_z[400000]; static int mi_n, mi_frames, mi_with, mi_max;
static void mirror_census(const float *qv, int nv)
{
    int k = 0;
    for (int i = 0; i < nv; i++) {
        const float *v = qv + (size_t)i * VF;
        if (!((int)v[VR_VERT_FLAGS] & VR_V_SUBWIN)) continue;
        k++;
        if (mi_n < (int)(sizeof mi_z / sizeof mi_z[0]) && v[2] > 0) mi_z[mi_n++] = 1.0 / v[2];
    }
    mi_frames++; if (k) mi_with++; if (k > mi_max) mi_max = k;
}
static int cmp_d(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void mirror_report(void)
{
    if (!mi_frames) return;
    printf("MIRROR census: %d frames, %d with sub-window quads, up to %d vertices a frame, %d vertices\n", mi_frames, mi_with, mi_max, mi_n);
    if (!mi_n) return;
    qsort(mi_z, (size_t)mi_n, sizeof mi_z[0], cmp_d);
    const int pc[] = { 1, 10, 50, 90, 99 };
    for (int j = 0; j < 5; j++) {
        const double z = mi_z[(size_t)mi_n * pc[j] / 100], zk = z * 0.0025 / 1.5;
        printf("  p%-2d depth %8.0f units (%6.1f m): %4.0f%% of the far shift\n", pc[j], z, z * 0.0025, 100 * zk / (1 + zk));
    }
}

static int run_once(const char *bundle, const char *roms, double secs, const char *shots, int pass)
{
    /* the binary inside the bundle, copied under a new name: a new image for dyld. The
     * plugin's name is the bundle's (name.bundle/Contents/MacOS/name) or the library's
     * (libname.so) */
    char src[1200], dst[256], cmd[1600], name[128];
    const char *base = strrchr(bundle, '/'); base = base ? base + 1 : bundle;
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') bl--;              /* "name.bundle/" */
    const int so = bl > 3 && !strncmp(base + bl - 3, ".so", 3);
    if (so && !strncmp(base, "lib", 3)) { base += 3; bl -= 3; }
    const size_t ext = so ? 3 : bl > 7 && !strncmp(base + bl - 7, ".bundle", 7) ? 7 : 0;
    snprintf(name, sizeof name, "%.*s", (int)(bl - ext), base);
    if (so) snprintf(src, sizeof src, "%s", bundle);    /* Android */
    else snprintf(src, sizeof src, "%s/Contents/MacOS/%s", bundle, name);
    const char *tmp = getenv("TMPDIR");                  /* adb shell: /data/local/tmp */
    snprintf(dst, sizeof dst, "%s/%s_test_%d_%d", tmp && *tmp ? tmp : "/tmp", name, (int)getpid(), pass);
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", src, dst);
    if (system(cmd) != 0) { fprintf(stderr, "cannot copy %s\n", src); return 1; }
    void *h = dlopen(dst, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
#define SYM(f, n) if (!(api.f = (void *)dlsym(h, n))) { fprintf(stderr, "missing %s\n", n); return 1; }
    SYM(version, "vr_abi_version") SYM(start, "vr_start") SYM(state, "vr_state") SYM(stop, "vr_stop")
    SYM(controls, "vr_set_controls") SYM(frame, "vr_picture") SYM(audio, "vr_audio") SYM(stats, "vr_stats")
    SYM(vrframe, "vr_frame") SYM(renderer, "vr_renderer") SYM(world, "vr_set_world")
    SYM(istats, "vr_interp_stats") SYM(vfloats, "vr_vertex_floats") SYM(game, "vr_game")
    SYM(ntables, "vr_table_count") SYM(table_size, "vr_table_size")
    if (api.version() != VR_ABI_VERSION) { fprintf(stderr, "the plugin's ABI is %d, this test's %d\n", api.version(), VR_ABI_VERSION); return 1; }
    if (pass == 1) {                                     /* the optional exports: each one there, well formed */
        const char *(*roms)(void) = (const char *(*)(void))dlsym(h, "vr_roms");
        int files = 0, bad = 0;
        for (const char *l = roms ? roms() : NULL; l && *l; files++) {     /* name\tsize\tcrc32 a line */
            const char *e = strchr(l, '\n');
            const size_t n = e ? (size_t)(e - l) : strlen(l);
            const char *t1 = memchr(l, '\t', n);
            const char *t2 = t1 ? memchr(t1 + 1, '\t', n - (size_t)(t1 + 1 - l)) : NULL;
            char *end = NULL;
            int ok = t1 && t2 && t1 > l;                /* a name, the size in decimal, the CRC-32 in hex */
            if (ok) { strtoll(t1 + 1, &end, 10); ok = end == t2 && end > t1 + 1; }
            if (ok) { strtoul(t2 + 1, &end, 16); ok = end == l + n && end > t2 + 1; }
            bad += !ok;
            l += n + (e != NULL);
        }
        if (roms) printf("vr_roms: %d files%s\n", files, bad ? ", MALFORMED" : "");
        else printf("vr_roms: none (optional)\n");
        if (bad) { fprintf(stderr, "vr_roms: %d lines not name, decimal size, hex CRC\n", bad); return 1; }
    }
    VF = api.vfloats();
    if (VF < VF_SHARED) { fprintf(stderr, "the plugin's vertex is %d floats, fewer than the %d shared\n", VF, VF_SHARED); return 1; }
    rr = !strcmp(api.game(), "raverace");
    ntab = api.ntables();
    if (ntab < 0 || ntab > MAX_TABLES) { fprintf(stderr, "the plugin has %d tables, this test takes %d\n", ntab, MAX_TABLES); return 1; }
    for (int i = 0; i < ntab; i++) {                     /* the same sizes in both passes */
        tab_size[i] = api.table_size(i);
        if (!tab[i] && !(tab[i] = calloc((size_t)tab_size[i] + 1, 1))) { fprintf(stderr, "no memory for table %d\n", i); return 1; }
    }
    static long look_from = -2; if (look_from == -2) { const char *e = env("LOOK"); look_from = e ? atol(e) : -1; }
    /* VR_TEST_INTERP=1: the 3D export with frame interpolation's matching (native/vr_interp.c),
     * reported at the end of the pass; VR_INTERP_LOG=1 prints every frame's */
    const char *ie = env("INTERP");
    const int interp = ie && *ie == '1';
    const char *le = env("LEVEL");
    const int level = le ? atoi(le) : -1;
    if ((look_from >= 0 || interp || level >= 0) && pass == 2) api.world(1, 1);      /* the 3D export, quads behind kept */
    if (level >= 0) {
        fn_setopt setopt = (fn_setopt)dlsym(h, "vr_set_option");
        if (!setopt) { fprintf(stderr, "missing vr_set_option\n"); return 1; }
        setopt(VR_OPT_LEVEL, level);
    }
    const char *oe = env("OPT");
    fn_setopt setopt = oe ? (fn_setopt)dlsym(h, "vr_set_option") : NULL;
    int (*getopt_)(int) = oe ? (int (*)(int))dlsym(h, "vr_get_option") : NULL;
    if (oe && (!setopt || !getopt_)) { fprintf(stderr, "missing vr_set_option / vr_get_option\n"); return 1; }
    for (const char *o = oe; o && *o; ) {
        int k, v, n;
        if (sscanf(o, "%d=%d%n", &k, &v, &n) != 2) { fprintf(stderr, "VR_TEST_OPT: bad '%s'\n", o); return 1; }
        setopt(k, v);
        o += n; if (*o == ',') o++;
    }
    printf("pass %d: %s, %s, API version %d\n", pass, dst, api.game(), api.version());
    const int sw = pass == 1;                   /* pass 1: the flat picture; pass 2: the 3D frames */
    /* VR_TEST_ARGS: more of the game's options for both passes, e.g. "--perf --replay race.rec" */
    char args[512]; const char *extra = env("ARGS");
    snprintf(args, sizeof args, "%s %s", sw ? "--sw" : "", extra ? extra : "");
    if (api.start(roms, 48000, args) != 0) { fprintf(stderr, "start failed\n"); return 1; }
    if (api.start(roms, 48000, NULL) != -1) { fprintf(stderr, "a second start in one copy was not refused\n"); return 1; }
    audio_rms_sum = 0; audio_n = audio_nonzero = 0;
    atomic_store(&audio_run, 1);
    pthread_t at; pthread_create(&at, NULL, audio_thread, NULL);

    enum { PIC_CAP = 2048 * 2048 * 4 };         /* the flat picture, at most */
    static uint8_t *pic; static float *qv; static uint32_t *qi; static int qv_vf;
    if (!pic) pic = malloc(PIC_CAP);
    if (qv_vf != VF) { free(qv); qv = malloc((size_t)200000 * VF * sizeof *qv); qv_vf = VF; }
    if (!qi) qi = malloc(600000 * sizeof *qi);
    if (!pic || !qv || !qi) { fprintf(stderr, "no memory\n"); return 1; }
    int max_nv = 0, max_ni = 0;
    uint32_t look_last = 0;
    const char *xe = env("EXPLORE");
    const double ex_at = xe && pass == 2 ? atof(xe) : -1;
    fn_setopt ex_set = ex_at >= 0 ? (fn_setopt)dlsym(h, "vr_set_option") : NULL;
    int (*ex_get)(int) = ex_at >= 0 ? (int (*)(int))dlsym(h, "vr_get_option") : NULL;
    if (ex_at >= 0 && (!ex_set || !ex_get)) { fprintf(stderr, "missing vr_set_option / vr_get_option\n"); return 1; }
    if (ex_at >= 0) api.world(1, 1);
    int ex_step = 0; uint32_t ex_f0 = 0, ex_f1 = 0, ex_after = 0; double ex_t = 0;
    const char *re = env("REPAINT");
    const double rp_at = re && pass == 2 ? atof(re) : -1;
    void (*rp_pause)(int) = rp_at >= 0 ? (void (*)(int))dlsym(h, "vr_pause") : NULL;
    fn_setopt rp_set = rp_at >= 0 ? (fn_setopt)dlsym(h, "vr_set_option") : NULL;
    if (rp_at >= 0 && (!rp_pause || !rp_set)) { fprintf(stderr, "missing vr_pause / vr_set_option\n"); return 1; }
    int rp_step = 0, rp_q0 = 0, rp_q1 = 0, rp_q2 = 0, last_nv = 0; uint32_t rp_f0 = 0, rp_f1 = 0, rp_f2 = 0; double rp_t = 0;
    uint32_t have = 0, got = 0, last_shot = 0; int w = 0, hh = 0;
    const char *hp = env("HASH");
    FILE *hf = NULL;
    if (hp) { char p[1024]; snprintf(p, sizeof p, "%s%d.txt", hp, pass); if (!(hf = fopen(p, "w"))) { fprintf(stderr, "cannot write %s\n", p); return 1; } }
    const double t0 = now_s();
    double worst_copy = 0;
    while (now_s() - t0 < secs) {
        const double t = now_s() - t0;
        uint32_t held = 0;
        if (t > 2.0 && t < 2.15) held |= 1u << VR_ACT_COIN1;     /* ~9 frames */
        if (t > 4.0) held |= 1u << VR_ACT_GAS;                   /* starts the game, then drives */
        api.controls(held, 0, 0, 0, 0, 0, 0);
        const double c0 = now_s();
        uint32_t f;
        if (sw) {
            f = api.frame(pic, PIC_CAP, have, &w, &hh);
            if (hf && f && f != have) fprintf(hf, "%u %016llx\n", f, (unsigned long long)fnv(0xcbf29ce484222325ull, pic, (size_t)w * hh * 4));
        }
        else {
            int nv, ni; uint32_t present; int64_t tns, rns;
            f = quads(have, 0, qv, 200000, qi, 600000, &nv, &ni, &present, &tns, &rns);
            if (hf && f && f != have) fprintf(hf, "%u %016llx\n", f, (unsigned long long)frame_hash(qv, nv, qi, ni, present));
            if (interp && f && f != have) interp_census(qv, nv, f);
            if (level >= 0 && f && f != have) level_census(qv, nv);
            static int mirror = -1; if (mirror < 0) mirror = env("MIRROR") != NULL;
            if (mirror && pass == 2 && f && f != have && nv <= 200000) mirror_census(qv, nv);
            if (nv > max_nv) max_nv = nv;
            if (f && f != have) last_nv = nv;
            if (rp_step == 4 && f && f != have && f >= rp_f2 + 30 && !rp_q2) rp_q2 = nv;   /* settled, running again */
            if (ni > max_ni) max_ni = ni;
            /* "lit": Rave Racer's text layer's alpha (table 1, 640x480 RGBA); any other board's
             * frames count as lit when they carry triangles (max_ni, below) */
            if (rr && ntab > 1 && tab_size[1] == 640 * 480 * 4) {
                w = 640; hh = 480;
                if (f && f != have) memcpy(pic, tab[1], (size_t)tab_size[1]);
            }
            static long fov_from = -2; if (fov_from == -2) { const char *e = env("FOV"); fov_from = e ? atol(e) : -1; }
            if (fov_from >= 0 && f && f != have && f >= (uint32_t)fov_from && nv <= 200000) fov_census(qv, nv);
            if (look_from >= 0 && f && f != have && f >= (uint32_t)look_from && f >= look_last + 4 && nv <= 200000) { look_census(qv, nv); look_last = f; }
        }
        if (ex_at >= 0 && !sw) {
            if (ex_step == 0 && t >= ex_at) { ex_set(VR_OPT_EXPLORE, 1); ex_step = 1; ex_t = t; }
            else if (ex_step == 1 && ex_get(VR_OPT_EXPLORE) == 2) {  /* frozen: take the frame once it is published */
                if (!ex_frozen_frame) {                  /* the newest again, whether or not already taken */
                    int nv, ni; uint32_t present; int64_t tns, rns;
                    const uint32_t ff = quads(0, 0, qv, 200000, qi, 600000, &nv, &ni, &present, &tns, &rns);
                    if (ff) { explore_census(qv, nv); ex_frozen_frame = (int)ff; }
                }
                if (ex_frozen_frame && t - ex_t > 1) { api.stats(&ex_f0, NULL, NULL, NULL); ex_step = 2; ex_t = t; }
            }
            else if (ex_step == 2 && t - ex_t >= 3) { api.stats(&ex_f1, NULL, NULL, NULL); ex_set(VR_OPT_EXPLORE, 0); ex_step = 3; ex_t = t; }
            else if (ex_step == 3 && t - ex_t >= 1) { api.stats(&ex_after, NULL, NULL, NULL); ex_step = 4; }
        }
        if (rp_at >= 0 && !sw) {                         /* the menu's pause, the draw distance changed in it */
            if (rp_step == 0 && t >= rp_at) { rp_pause(1); rp_step = 1; rp_t = t; }
            else if (rp_step == 1 && t - rp_t >= 0.5) { api.stats(&rp_f0, NULL, NULL, NULL); rp_q0 = last_nv; rp_set(VR_OPT_DRAW_EXTRA, 24); rp_step = 2; }
            else if (rp_step == 2 && t - rp_t >= 1.0) { api.stats(&rp_f1, NULL, NULL, NULL); rp_step = 3; }
            else if (rp_step == 3 && t - rp_t >= 1.5) { api.stats(&rp_f2, NULL, NULL, NULL); rp_q1 = last_nv; rp_pause(0); rp_step = 4; }
        }
        const double dc = now_s() - c0; if (dc > worst_copy) worst_copy = dc;
        if (f && f != have) { have = f; got++; }
        if (shots && f && f / 300 != last_shot / 300) {
            char p[1024]; snprintf(p, sizeof p, "%s/u%d_f%05u.ppm", shots, pass, f);
            write_ppm(p, pic, w, hh); last_shot = f;
        }
        if (api.state() == 3) { fprintf(stderr, "the game thread stopped by itself\n"); break; }
        usleep(16667);
    }
    const double el = now_s() - t0;
    for (const char *o = oe; o && *o; ) {                      /* before stop: the game's values */
        int k, v, n;
        if (sscanf(o, "%d=%d%n", &k, &v, &n) != 2) break;
        printf("pass %d: option %d set %d, in force %d\n", pass, k, v, getopt_(k));
        o += n; if (*o == ',') o++;
    }
    const int stopped = api.stop(3000);
    if (hf) fclose(hf);
    atomic_store(&audio_run, 0);
    pthread_join(at, NULL);
    uint32_t fr, traps, unm, rw; api.stats(&fr, &traps, &unm, &rw);
    long lit = 0;
    for (int i = 0; i < w * hh; i++) if (sw ? (pic[i * 4] | pic[i * 4 + 1] | pic[i * 4 + 2]) : pic[i * 4 + 3]) lit++;
    if (sw && w > 0 && hh > 0) { scr_w = w; scr_h = hh; }
    if (!sw && !rr && got) lit = 1;             /* no picture in pass 2: the triangles (max_ni) count */
    if (!sw) printf("pass %d: engine quads, renderer %d, up to %d vertices / %d indices a frame\n", pass, api.renderer(), max_nv, max_ni);
    if (!sw && interp) {
        double st[9]; api.istats(st);
        printf("pass %d: interp %.0f frames: objects matched %.1f%%, world objects %.1f%%; snaps: caller %.0f, move %.0f, turn %.0f, unmatched %.0f\n",
               pass, st[0], st[1] ? 100 * st[2] / st[1] : 0, st[3] ? 100 * st[4] / st[3] : 0, st[5], st[6], st[7], st[8]);
        interp_report();
    }
    if (!sw && level >= 0) level_report(level);
    int ex_bad = 0;
    if (ex_at >= 0 && !sw) {
        printf("pass %d: explore frozen at frame %d: %d 3D quads, %d explore, %d one-sided (%d bad normals), layers 0:%d 1:%d 2:%d 3:%d; "
               "game frames while frozen %d, after 1 s off %d\n", pass, ex_frozen_frame, ex_quads, ex_explore, ex_onesided, ex_badnormal,
               ex_layers[0], ex_layers[1], ex_layers[2], ex_layers[3], (int)(ex_f1 - ex_f0), (int)(ex_after - ex_f1));
        ex_bad = ex_step < 4 || !ex_frozen_frame || ex_explore == 0 || ex_explore != ex_quads || ex_badnormal || ex_f1 != ex_f0 || ex_after < ex_f1 + 30;
    }
    int rp_bad = 0;
    if (rp_at >= 0 && !sw) {
        printf("pass %d: repaint: paused at frame %u, draw distance +24: %d frames run, then %d more; vertices %d -> %d (running again, 30 frames on: %d)\n",
               pass, rp_f0, (int)(rp_f1 - rp_f0), (int)(rp_f2 - rp_f1), rp_q0, rp_q1, rp_q2);
        rp_bad = rp_step < 4 || rp_f1 - rp_f0 != 24 || rp_f2 != rp_f1 || rp_q1 <= rp_q0 || !rp_q2 || rp_q1 < rp_q2 * 9 / 10;
    }
    printf("pass %d: %.1f s, game frame %u (%.2f fps), %u pictures taken, picture %dx%d %.1f%% lit, "
           "worst copy %.2f ms, traps %u unmapped %u romwrites %u, audio RMS %.4f (%.1f%% nonzero), stopped %d\n",
           pass, el, fr, fr / el, got, w, hh, w * hh ? 100.0 * lit / (w * hh) : 0, worst_copy * 1e3, traps, unm, rw,
           audio_n ? sqrt(audio_rms_sum / audio_n) : 0, audio_n ? 100.0 * audio_nonzero / audio_n : 0, stopped);
    unlink(dst);                                /* mapped stays mapped; the old copy is never unloaded */
    const double frozen_s = (ex_at >= 0 && !sw ? 4 : 0) + (rp_at >= 0 && !sw ? 1.5 : 0);   /* explore / the pause held the game still */
    return ex_bad || rp_bad || !(stopped && traps == 0 && unm == 0 && fr > 50 * (el - frozen_s) && lit > 0 && audio_nonzero > 0 && (sw || max_ni > 300));
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <name.bundle | libname.so> <rom_dir> [seconds] [shots_dir]\n", argv[0]); return 2; }
    const double secs = argc > 3 ? atof(argv[3]) : 12;
    const char *shots = argc > 4 ? argv[4] : NULL;
    int bad = run_once(argv[1], argv[2], secs, shots, 1);
    bad |= run_once(argv[1], argv[2], secs, NULL, 2) << 1;
    fov_report();
    look_report();
    mirror_report();
    printf("%s\n", bad ? "FAIL" : "OK");
    return bad;
}
