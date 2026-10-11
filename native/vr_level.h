/*
 * vr_level.h -- the level camera (VR comfort): the rotation that takes a game camera's
 * roll, its pitch or both out of what it sees (see vr_level.c). Board-neutral: the board
 * says where the world's up points in its camera's view space, and turns its corners.
 * C99, and C++ through extern "C".
 */
#ifndef VR_LEVEL_H
#define VR_LEVEL_H

#include "../abi/vrcore.h"          /* VR_LEVEL_ROLL, VR_LEVEL_PITCH: VR_OPT_LEVEL's bits */

#ifdef __cplusplus
extern "C" {
#endif

/* up: the world's up in the camera's view space (x right, y up, z forward), any length --
 * the camera matrix's y row. mode: VR_LEVEL_ROLL, VR_LEVEL_PITCH or both. L: the rotation
 * about the camera that stands `up` straight (both), or takes out only the roll about the
 * view axis, or only the pitch. The identity when up is zero, or along the view axis (the
 * camera looking straight up or down: no roll to take out). */
void vr_level_matrix(const float up[3], int mode, float L[3][3]);

/* a view-space point turned by L, in place */
static inline void vr_level_turn(const float L[3][3], float p[3])
{
    const float x = p[0], y = p[1], z = p[2];
    for (int i = 0; i < 3; i++) p[i] = L[i][0] * x + L[i][1] * y + L[i][2] * z;
}

#ifdef __cplusplus
}
#endif
#endif /* VR_LEVEL_H */
