/*
 * vr_level.c -- the level camera (VR comfort, not part of any board's hardware model).
 *
 * A racing game's camera rolls with the car in turns and pitches with the road. On a
 * monitor that is the arcade's feel; in a headset the whole world tilting while your head
 * stays still is what makes people sick. So the game's plugin turns the corners of what
 * its camera sees about the camera until the world's up is straight up in view space:
 * roll about the view axis, pitch about the horizontal one, either or both (VR_OPT_LEVEL).
 * The camera's position and heading stay the game's.
 *
 * The board gives the world's up in its camera's view space (for example the view matrix's
 * y row) and applies the rotation to the
 * corners it chooses, when it chooses (for example world objects only, while a course is
 * on screen). A frame drawn between two game frames needs each
 * frame's corners under that frame's own camera: one matrix from this frame's up, one
 * from the last's.
 */
#include <math.h>
#include <string.h>
#include "vr_level.h"

void vr_level_matrix(const float up[3], int mode, float L[3][3])
{
    memset(L, 0, 9 * sizeof(float)); L[0][0] = L[1][1] = L[2][2] = 1;
    const float n = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (n <= 0) return;
    const float ux = up[0] / n, uy = up[1] / n, uz = up[2] / n;
    const float r = sqrtf(ux * ux + uy * uy);
    if (r < 1e-4f) return;                          /* looking straight up or down: no roll to take out */
    const float c = uy / r, s = ux / r;
    /* rz: about the view axis, up into the y-z plane; rx: then about the x axis, up to +y */
    const float rz[3][3] = { { c, -s, 0 }, { s, c, 0 }, { 0, 0, 1 } };
    const float rx[3][3] = { { 1, 0, 0 }, { 0, r, uz }, { 0, -uz, r } };
    float a[3][3], b[3][3];
    if (mode == VR_LEVEL_ROLL) { memcpy(L, rz, sizeof rz); return; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        a[i][j] = rx[i][0] * rz[0][j] + rx[i][1] * rz[1][j] + rx[i][2] * rz[2][j];
    if (mode != VR_LEVEL_PITCH) { memcpy(L, a, sizeof a); return; }
    /* pitch alone: level in the unrolled frame, then roll back (rz transposed) */
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        b[i][j] = rz[0][i] * a[0][j] + rz[1][i] * a[1][j] + rz[2][i] * a[2][j];
    memcpy(L, b, sizeof b);
}
