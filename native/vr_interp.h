/*
 * vr_interp.h -- where each object of this game frame was one frame earlier, for a
 * renderer that draws faster than the game runs (see vr_interp.c). Board-neutral: the
 * board's adapter says what an object is (its key, its position, its rotations) and keeps
 * whatever it needs to place the object again (its own transform, in its own arithmetic).
 * C99, and C++ through extern "C".
 */
#ifndef VR_INTERP_H
#define VR_INTERP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VR_INTERP_MAX 4096          /* objects a frame; more are not matched */

/* One object of the frame, as the board draws it. Two objects can be the same object only
 * when group, world and id are all equal, except that an object left alone may take a
 * partner of another id in the same group and world (a level-of-detail switch). */
typedef struct {
    int32_t group;          /* the window it is drawn in (System 22: the viewport record) */
    int32_t id;             /* its model (System 22: the model code) */
    int     world;          /* 1: seen through the camera; 0: fixed to the view (a HUD object) */
    double  pos[3];         /* its translation BEFORE the view: camera-relative, in world axes */
    double  rot[3][3];      /* its rotation in view space (object x view), unit scale */
    double  view[3][3];     /* the camera's rotation it was drawn under, unit scale */
} vr_interp_obj;

/* The limits, in the board's position units. */
typedef struct {
    double match;           /* an object's own move in one frame, the camera's taken out */
    double move;            /* the camera's move in one frame: further is a cut */
    double turn_deg;        /* the camera's turn in one frame: further is a cut */
    double lod_turn_deg;    /* a match across ids: the two turned at most this far apart */
} vr_interp_limits;
void vr_interp_set_limits(const vr_interp_limits *l);

/* The frame's calls, in order: begin before the board's walk, add per object, then end
 * (snap 1: nothing of this frame is matched -- the caller knows it is not the continuation
 * of the last one: a replaced picture, a cut), or off (the renderer did not ask for this
 * frame: nothing matched, nothing counted, and its objects are still the next frame's
 * last frame). */
void vr_interp_begin(void);
int  vr_interp_add(const vr_interp_obj *o);    /* 1-based index; 0 = full */
void vr_interp_end(int snap);
void vr_interp_off(void);

/* This frame's object `obj` (vr_interp_add's index): the index of the last frame's object
 * it was matched with, 1-based; 0 = no match. */
int  vr_interp_prev(int obj);

enum { VR_SNAP_NONE, VR_SNAP_CALLER, VR_SNAP_MOVE, VR_SNAP_TURN, VR_SNAP_UNMATCHED, VR_SNAP_N };
typedef struct {
    /* the last frame */
    int      objs, matched, world, world_matched, lod, snap;
    double   cam_move, cam_turn_deg;       /* the camera's own motion since the frame before */
    /* totals: frames, snaps by reason, and the matches over the frames that did not snap */
    unsigned frames, snaps[VR_SNAP_N];
    uint64_t objs_sum, matched_sum, world_sum, world_matched_sum, lod_sum;
} vr_interp_counts;
extern vr_interp_counts g_vr_interp;

#ifdef __cplusplus
}
#endif
#endif /* VR_INTERP_H */
