/*
 * vrcore.h -- the C interface every game plugin exports to the VR host (host/app/plugin.h
 * binds it by name). Board-neutral: what is the same for every
 * board is here; what a board adds (its vertex attributes' meaning, its tables, its
 * option keys and actions) the game documents beside its plugin.
 *
 * THREADS. The game runs on its own thread, paced to its board's rate. The host's main
 * thread takes the newest frame whenever it likes (vr_frame / vr_picture), its audio
 * thread pulls sound (vr_audio), and everything else may be called from either.
 *
 * ONE RUN PER LOADED COPY. A game whose state is globals can run once per load; the
 * host loads a fresh copy of the binary for every run and a second
 * vr_start in the same copy returns -1.
 */
#ifndef VRCORE_H
#define VRCORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VR_ABI_VERSION 1
#if defined(_WIN32)
#define VR_API __declspec(dllexport)
#else
#define VR_API __attribute__((visibility("default")))
#endif

/* ---- identity ---------------------------------------------------------- */
VR_API int         vr_abi_version(void);    /* VR_ABI_VERSION the plugin was built with */
VR_API const char *vr_game(void);           /* the game's short name ("raverace"), static */
VR_API int         vr_vertex_floats(void);  /* floats per vertex of vr_frame (the board's layout) */

/* ---- the game thread ---------------------------------------------------- */
enum { VR_IDLE = 0, VR_STARTING = 1, VR_RUNNING = 2, VR_STOPPED = 3 };
/* rom_dir: the game's ROMs (a folder or where its zip is, the game says); audio_hz: the
 * host's output rate (0 = no sound); args: the game's own command-line options, space-
 * separated, or NULL. 0 = started; -1 = this copy already ran; -2 = no thread. */
VR_API int  vr_start(const char *rom_dir, int audio_hz, const char *args);
VR_API int  vr_state(void);                 /* VR_IDLE .. VR_STOPPED */
VR_API int  vr_exit_code(void);             /* once VR_STOPPED: why */
VR_API int  vr_stop(int timeout_ms);        /* 1 = stopped (or never ran), 0 = still running */
VR_API void vr_pause(int on);               /* the host's menu: the game holds its frame */

/* ---- controls ----------------------------------------------------------- */
/* held: bit (1 << action) per held control. Actions 0..15 are shared by every game that has
 * them; 16..31 are the game's own. steer -32767..32767 (+ = right) and the pedals
 * 0..vr_pedal_max() count when their *_on flag is set (an analogue source out of its dead
 * zone); off, the digital actions steer and press. */
enum { VR_ACT_COIN1 = 0, VR_ACT_COIN2 = 1, VR_ACT_SERVICE = 2, VR_ACT_TEST = 3,
       VR_ACT_SHIFT_DOWN = 4, VR_ACT_SHIFT_UP = 5, VR_ACT_VIEW = 6,
       VR_ACT_STEER_LEFT = 7, VR_ACT_STEER_RIGHT = 8, VR_ACT_GAS = 9, VR_ACT_BRAKE = 10,
       VR_ACT_GAME = 16 };                  /* the game's own from here */
VR_API void vr_set_controls(uint32_t held, int steer_on, int steer, int gas_on, int gas, int brake_on, int brake);
VR_API int  vr_pedal_max(void);             /* a pedal's full travel, in the game's own units */
/* the cabinet's force feedback this frame, -1..1 (+ pushes the wheel right); 0 with none,
 * the option off, or the game held */
VR_API float vr_feedback(void);

/* ---- sound -------------------------------------------------------------- */
/* frames of interleaved float, channels wide, at vr_start's audio_hz; returns frames written */
VR_API int  vr_audio(float *buf, int frames, int channels);

/* ---- pictures ----------------------------------------------------------- */
/* 1 = the game hands over 3D frames (vr_frame); 0 = only a flat picture (vr_picture) */
VR_API int  vr_renderer(void);
/* The newest flat picture, RGBA32 bottom row first, into dst (cap bytes); the game frame it
 * was drawn on (0 = none yet), *w, *h its size. Not copied if dst is too small or it is
 * `have`, the one the caller holds. */
VR_API uint32_t vr_picture(uint8_t *dst, int cap, uint32_t have, int *w, int *h);

/* ASSETS: what the board's shader needs once, loaded by the game at start-up (texture ROMs).
 * vr_assets copies every one into dst[i] (vr_asset_size(i) bytes each; a NULL is skipped):
 * 1 = copied, 0 = not loaded yet, ask again. */
VR_API int  vr_asset_count(void);
VR_API int  vr_asset_size(int i);
VR_API int  vr_assets(uint8_t *const *dst);

/* THE FRAME. A ring of recent game frames; `want` picks one (0 = the newest; newer than the
 * newest = the newest; older than the ring = its oldest), not copied if it is `have`.
 * v: *nv vertices of vr_vertex_floats() floats; idx: *ni triangle indices; tables[i]: the
 * board's per-frame tables (vr_table_size(i) bytes each). *present: bit i = table i came
 * with this frame (one absent keeps its last contents; tables[i] is not written). If *nv or
 * *ni is above vcap / icap nothing is copied and 0 returned: grow and ask again.
 * *t_ns: when the frame's slot began on the game's pacer clock (vr_now_ns), *ready_ns: when
 * it was published. Returns the frame number, 0 = none. */
VR_API int      vr_table_count(void);
VR_API int      vr_table_size(int i);
VR_API uint32_t vr_frame(uint32_t have, uint32_t want, float *v, int vcap, uint32_t *idx, int icap,
                         uint8_t *const *tables, int *nv, int *ni, uint32_t *present, int64_t *t_ns, int64_t *ready_ns);
/* the newest frame's revision: it changes when a frame is published again with other
 * contents under the same number (an option changed while paused); ask again with have = 0 */
VR_API uint32_t vr_revision(void);
VR_API int64_t  vr_now_ns(void);            /* the pacer's clock (monotonic) */
VR_API int64_t  vr_frame_ns(void);          /* one game frame on it: the board's rate */

/* THE 3D EXPORT. world3d 1: the world comes as 3D corners in the game camera's view space,
 * each with where it was one game frame earlier (for drawing between game frames);
 * keep_behind 1: geometry behind the game camera is kept too, for looking round. */
VR_API void vr_set_world(int world3d, int keep_behind);

/* ---- vertices ------------------------------------------------------------ *
 * A vertex is vr_vertex_floats() floats, the first VR_VERT_FLOATS of them laid out as
 * vr_vert: a COMMON part, the same on every board, and a BOARD BLOCK of ten floats between
 * its fields -- the board's material, which only its own shader reads (System 22: u, v and
 * brightness, pen, sub-pens, texture bank, fog; the game documents it beside its plugin and
 * gives the block's floats its own names in a struct of the same layout). A board that
 * needs more than ten appends them after float 20 (TEXCOORD5 on) and says so in
 * vr_vertex_floats. The split is by attribute slot, not a prefix, so a mesh takes the
 * vertices as they are:
 *   pos    POSITION    (0..2)   common. 3D: view-space X, Y, Z (x right, y up, z forward);
 *                               flat: screen pixels x, y (y down) and 1/z
 *   b0     TEXCOORD0   (3..5)   the board's
 *   b1     TEXCOORD1.xyz (6..8) the board's
 *   flags  TEXCOORD1.w (9)      common: VR_V_* below | the board's own bits (VR_V_BOARD)
 *   b2     TEXCOORD2   (10..13) the board's
 *   clip   TEXCOORD3   (14..17) common. flat: the clip window [x0,x1) x [y0,y1) pixels;
 *                               3D: focal px, centre x, centre y, and the draw rank (the host's)
 *   prev   TEXCOORD4   (18..20) common. 3D: the corner one game frame earlier (or, in
 *                               explore frames, a one-sided quad's front normal) */
typedef struct {
    float pos[3];
    float b0[3], b1[3];
    float flags;
    float b2[4];
    float clip[4];
    float prev[3];
} vr_vert;
enum { VR_VERT_POS = 0, VR_VERT_B0 = 3, VR_VERT_B1 = 6, VR_VERT_FLAGS = 9, VR_VERT_B2 = 10,
       VR_VERT_CLIP = 14, VR_VERT_PREV = 18, VR_VERT_FLOATS = 21 };   /* float offsets in a vertex */
/* The flags, a whole number held in a float (exact up to bit 23). Common: 3D (view-space
 * corners, above), BG (the background quad, first in the frame: the whole view),
 * ONESIDED and EXPLORE (explore frames: a frozen frame to look at from anywhere; prev is
 * the front normal of a one-sided quad), SUBWIN (a flat quad of a sub-window's 3D view, pos
 * z = 1 / its view-space depth), and the coplanar layer. The board's own: bits 0..2, and
 * bits 10 up when it needs more. */
enum { VR_V_BOARD = 7,
       VR_V_3D = 8, VR_V_BG = 16, VR_V_ONESIDED = 32, VR_V_EXPLORE = 64, VR_V_SUBWIN = 128 };
#define VR_V_LAYER_SHIFT 8                  /* explore: (flags >> 8) & VR_V_LAYER_MAX = the coplanar layer */
#define VR_V_LAYER_MAX 3
#define VR_V_BOARD_SHIFT 10                 /* the board's further bits start here */

/* ---- options ------------------------------------------------------------- *
 * Applied on the game thread at its next frame; set before vr_start they take effect as it
 * runs. get: the value in force, -1 = never set / not this game's. Keys below 100 are the
 * shared ones here or the game's older ones (Rave Racer's); new game keys start at 100. */
enum { VR_OPT_FREEPLAY = 1,                 /* 0/1 */
       VR_OPT_DRAW_EXTRA = 2,               /* draw distance: extra, 0 = the original */
       VR_OPT_VOLUME = 3,                   /* 0..100, the whole mix */
       VR_OPT_LEVEL = 7,                    /* the level camera: VR_LEVEL_* taken out */
       VR_OPT_HUD = 17,                     /* the HUD in a race 0/1 */
       VR_OPT_EXPLORE = 18,                 /* the frozen world to fly round 0/1; get: 2 once frozen */
       VR_OPT_MUSIC_VOL = 19, VR_OPT_SFX_VOL = 20,   /* 0..100, on top of VR_OPT_VOLUME */
       VR_OPT_FEEDBACK = 21,                /* the game's force feedback 0/1 */
       VR_OPT_GAME = 100 };
enum { VR_LEVEL_ROLL = 1, VR_LEVEL_PITCH = 2 };
VR_API void vr_set_option(int key, int value);
VR_API int  vr_get_option(int key);

/* ---- health -------------------------------------------------------------- */
/* the game frame and three counters that must stay 0 (the game names them: Rave Racer's are
 * traps, unmapped accesses, ROM writes) */
VR_API void vr_stats(uint32_t *frame, uint32_t *c0, uint32_t *c1, uint32_t *c2);
/* drawing between frames, totals since start: frames, objects, matched, world objects, world
 * matched, snaps by the caller, the camera's move, its turn, too few matches */
VR_API void vr_interp_stats(double out[9]);

#ifdef __cplusplus
}
#endif
#endif /* VRCORE_H */
