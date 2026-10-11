/*
 * vr_interp.c -- where each object of this game frame was one frame earlier.
 *
 * An arcade board runs at its own rate (System 22 59.906 Hz, Model 2 57.524 Hz); a headset
 * draws at 72-120 Hz. Between two game frames a renderer draws each corner part-way from
 * where it was to where it is: every vertex carries where it was one game frame earlier.
 * A board's display list says nothing about identity from frame to frame, so this pairs
 * each object of this frame with one of the last:
 *
 *   THE KEY: (group, world, id) -- System 22: (viewport record, under a view transform,
 *   model code); Model 2: (window, polygon-data address). Only objects with the same key
 *   can be the same object.
 *   THE POSITION: an object's translation BEFORE the view (vr_interp_obj.pos) is camera-
 *   relative in world axes, so between two frames every static object's moves by the
 *   same amount -- minus the camera's own move -- whatever the camera's turn. That
 *   move is the median over all world objects paired by occurrence, then again over
 *   the pairs found; what is left (the residual) is the object's own motion.
 *   Candidates within the match limit of that residual are taken nearest first. A
 *   game that appends pieces relative to the player (Rave Racer's track, rd_b2.c) shifts
 *   the order when a piece boundary is crossed; a level-of-detail switch changes the id
 *   and is matched across ids when nothing else is left (below).
 *
 * The whole frame goes unmatched (a SNAP: drawn where it is, as before) when the
 * caller says so, or when the camera moved or turned further in one frame than it
 * honestly can (a cut), or when fewer than a quarter of the world objects found a partner.
 *
 * The board keeps its own copy of each object's transform (its adapter: System 22's is
 * namco22-decompile's engine/frame_interp.c) and places the matched object's corners
 * again in its own arithmetic. VR_INTERP_LOG=1 prints every frame's matching to stderr;
 * VR_INTERP_DUMP=<frame> also lists the objects of three frames from that one.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_interp.h"

vr_interp_counts g_vr_interp;

static vr_interp_obj tab[2][VR_INTERP_MAX];
static int     ntab[2], cur;
static int16_t match_of[VR_INTERP_MAX];     /* this frame's object -> the last frame's, -1 none */
static int     have_matches;
static vr_interp_limits lim;

void vr_interp_set_limits(const vr_interp_limits *l) { lim = *l; }

void vr_interp_begin(void)
{
    cur ^= 1;
    ntab[cur] = 0;
    have_matches = 0;
}

int vr_interp_add(const vr_interp_obj *o)
{
    if (ntab[cur] == VR_INTERP_MAX) return 0;
    tab[cur][ntab[cur]] = *o;
    return ++ntab[cur];
}

/* the key's order: group, world flag, id */
static int key_order(const vr_interp_obj *x, const vr_interp_obj *y)
{
    if (x->group != y->group) return x->group < y->group ? -1 : 1;
    if (x->world != y->world) return x->world < y->world ? -1 : 1;
    if (x->id != y->id) return x->id < y->id ? -1 : 1;
    return 0;
}
static const vr_interp_obj *sort_tab;
static int key_cmp(const void *a, const void *b)
{
    const int16_t i = *(const int16_t *)a, j = *(const int16_t *)b;
    const int k = key_order(&sort_tab[i], &sort_tab[j]);
    return k ? k : i < j ? -1 : 1;                   /* then occurrence order */
}

static int cmpd(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double median(double *v, int n)
{
    if (n == 0) return 0;
    qsort(v, (size_t)n, sizeof *v, cmpd);
    return v[n / 2];
}

static double resid(const vr_interp_obj *c, const vr_interp_obj *p, const double d[3])
{
    const double w = c->world ? 1.0 : 0.0;
    const double x = c->pos[0] - p->pos[0] - w * d[0], y = c->pos[1] - p->pos[1] - w * d[1], z = c->pos[2] - p->pos[2] - w * d[2];
    return sqrt(x * x + y * y + z * z);
}

/* the angle between two rotations, degrees */
static double turn_deg(const double a[3][3], const double b[3][3])
{
    double tr = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) tr += a[i][j] * b[i][j];
    double c = (tr - 1) / 2; if (c > 1) c = 1; if (c < -1) c = -1;
    return acos(c) * (180 / 3.14159265358979323846);
}

/* the groups of one key: [ps, pe) of the last frame's sorted order, [cs, ce) of this one's */
static int16_t ps_idx[VR_INTERP_MAX], cs_idx[VR_INTERP_MAX];
static double  dx[VR_INTERP_MAX], dy[VR_INTERP_MAX], dz[VR_INTERP_MAX];
static uint8_t used[VR_INTERP_MAX];
typedef struct { int16_t c, p; float r; } cand;
static cand   *cands; static int ncand, cand_cap;
static int     n_lod;                       /* matches across ids, the last frame */

static void add_cand(int c, int p, double r)
{
    if (ncand == cand_cap) {
        const int n = cand_cap ? cand_cap * 2 : 4096;
        cand *nb = (cand *)realloc(cands, (size_t)n * sizeof *cands);
        if (!nb) return;
        cands = nb; cand_cap = n;
    }
    cand *k = &cands[ncand++];
    k->c = (int16_t)c; k->p = (int16_t)p; k->r = (float)r;
}
static int cand_cmp(const void *a, const void *b)
{
    const cand *x = (const cand *)a, *y = (const cand *)b;
    if (x->r != y->r) return x->r < y->r ? -1 : 1;
    if (x->c != y->c) return x->c < y->c ? -1 : 1;   /* a stable order for ties */
    return x->p < y->p ? -1 : x->p > y->p;
}
/* the candidates, nearest first: each object takes the nearest partner still free */
static int take_cands(void)
{
    qsort(cands, (size_t)ncand, sizeof *cands, cand_cmp);
    int n = 0;
    for (int k = 0; k < ncand; k++)
        if (match_of[cands[k].c] < 0 && !used[cands[k].p]) { match_of[cands[k].c] = cands[k].p; used[cands[k].p] = 1; n++; }
    ncand = 0;
    return n;
}

/* Pair this frame's objects with the last frame's, the camera's move being d.
 * pairs_only: by occurrence within each key alone, no limit (the first estimate of d).
 * Otherwise every pair of the same key within the match limit is a candidate, taken nearest
 * first; then LEVEL OF DETAIL: an object still alone may take a partner still alone of
 * ANOTHER id in the same group and world, if it is as near and turned the same way (a car's
 * body switches models with distance; its wheels, their own objects, do not). Fills
 * match_of, and dx/dy/dz with the world pairs' moves; returns their count. */
static int match(const double d[3], int pairs_only)
{
    const vr_interp_obj *P = tab[cur ^ 1], *C = tab[cur];
    const int np = ntab[cur ^ 1], nc = ntab[cur];
    memset(used, 0, (size_t)np);
    for (int i = 0; i < nc; i++) match_of[i] = -1;
    ncand = 0; n_lod = 0;
    int pi = 0;
    for (int ci = 0; ci < nc; ) {
        const vr_interp_obj *c0 = &C[cs_idx[ci]];
        int ce = ci; while (ce < nc && !key_order(&C[cs_idx[ce]], c0)) ce++;
        while (pi < np && key_order(&P[ps_idx[pi]], c0) < 0) pi++;
        int pe = pi; while (pe < np && !key_order(&P[ps_idx[pe]], c0)) pe++;
        for (int k = ci; k < ce; k++) {
            const int c = cs_idx[k];
            if (pairs_only) {
                const int j = pi + (k - ci);
                if (j < pe) { match_of[c] = ps_idx[j]; used[ps_idx[j]] = 1; }
                continue;
            }
            for (int j = pi; j < pe; j++) {
                const int p = ps_idx[j];
                const double r = resid(&C[c], &P[p], d);
                if (r <= lim.match) add_cand(c, p, r);
            }
        }
        ci = ce; pi = pe;
    }
    if (!pairs_only) {
        take_cands();
        for (int c = 0; c < nc; c++) {
            if (match_of[c] >= 0) continue;
            for (int p = 0; p < np; p++) {
                if (used[p] || P[p].group != C[c].group || P[p].world != C[c].world) continue;
                const double r = resid(&C[c], &P[p], d);
                if (r <= lim.match && turn_deg(C[c].rot, P[p].rot) <= lim.lod_turn_deg) add_cand(c, p, r);
            }
        }
        n_lod = take_cands();
    }
    int nd = 0;
    for (int c = 0; c < nc; c++) {
        if (match_of[c] < 0 || !C[c].world) continue;
        const vr_interp_obj *p = &P[match_of[c]];
        dx[nd] = C[c].pos[0] - p->pos[0]; dy[nd] = C[c].pos[1] - p->pos[1]; dz[nd] = C[c].pos[2] - p->pos[2];
        nd++;
    }
    return nd;
}

static void log_frame(const double d[3], int nd_pairs)
{
    static int on = -1; if (on < 0) { const char *e = getenv("VR_INTERP_LOG"); on = e && *e == '1'; }
    if (!on) return;
    const vr_interp_obj *P = tab[cur ^ 1], *C = tab[cur];
    /* the residuals of the pairs found, world objects only */
    static double r[VR_INTERP_MAX]; int nr = 0;
    for (int i = 0; i < ntab[cur]; i++)
        if (match_of[i] >= 0 && C[i].world) r[nr++] = resid(&C[i], &P[match_of[i]], d);
    qsort(r, (size_t)nr, sizeof *r, cmpd);
    const vr_interp_counts *s = &g_vr_interp;
    static long dump = -2; if (dump == -2) { const char *e = getenv("VR_INTERP_DUMP"); dump = e ? atol(e) : -1; }
    if (dump >= 0 && s->frames >= (unsigned)dump && s->frames < (unsigned)dump + 3)
        for (int i = 0; i < ntab[cur]; i++) {
            const vr_interp_obj *o = &C[i];
            fprintf(stderr, "[OBJ] f%u #%d group %d w %d id %4x pos %9.0f %9.0f %9.0f  prev %d%s\n", s->frames, i, o->group, o->world, o->id,
                    o->pos[0], o->pos[1], o->pos[2], match_of[i], match_of[i] >= 0 ? "" : " (none)");
        }
    fprintf(stderr, "[INTERP] f%u objs %d/%d matched %d (lod %d) world %d/%d move %.0f turn %.2f resid p50 %.0f p90 %.0f max %.0f pairs %d snap %d\n",
            s->frames, s->objs, ntab[cur ^ 1], s->matched, s->lod, s->world_matched, s->world, s->cam_move, s->cam_turn_deg,
            nr ? r[nr / 2] : 0, nr ? r[nr * 9 / 10] : 0, nr ? r[nr - 1] : 0, nd_pairs, s->snap);
}

/* the last frame's figures cleared and nothing matched: every frame starts here */
static void clear_frame(void)
{
    vr_interp_counts *s = &g_vr_interp;
    s->objs = ntab[cur]; s->matched = s->world = s->world_matched = s->lod = 0; s->cam_move = s->cam_turn_deg = 0; s->snap = VR_SNAP_NONE;
    for (int i = 0; i < ntab[cur]; i++) match_of[i] = -1;
}

void vr_interp_off(void) { clear_frame(); }

void vr_interp_end(int snap)
{
    vr_interp_counts *s = &g_vr_interp;
    const vr_interp_obj *P = tab[cur ^ 1], *C = tab[cur];
    const int np = ntab[cur ^ 1], nc = ntab[cur];
    clear_frame();
    if (nc == 0) return;
    s->frames++;
    for (int i = 0; i < nc; i++) s->world += C[i].world != 0;
    double d[3] = { 0, 0, 0 };
    int nd = 0;
    if (snap) s->snap = VR_SNAP_CALLER;
    else if (np > 0) {
        for (int i = 0; i < np; i++) ps_idx[i] = (int16_t)i;
        for (int i = 0; i < nc; i++) cs_idx[i] = (int16_t)i;
        sort_tab = P; qsort(ps_idx, (size_t)np, sizeof *ps_idx, key_cmp);
        sort_tab = C; qsort(cs_idx, (size_t)nc, sizeof *cs_idx, key_cmp);
        sort_tab = NULL;
        for (int pass = 0; pass < 3; pass++) {         /* pairs by occurrence -> d -> matches -> d -> matches */
            nd = match(d, pass == 0);
            if (pass < 2) { d[0] = median(dx, nd); d[1] = median(dy, nd); d[2] = median(dz, nd); }
        }
        s->cam_move = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const vr_interp_obj *cw = NULL, *pw = NULL;
        for (int i = 0; i < nc && !cw; i++) if (C[i].world) cw = &C[i];
        for (int i = 0; i < np && !pw; i++) if (P[i].world) pw = &P[i];
        if (cw && pw) s->cam_turn_deg = turn_deg(cw->view, pw->view);
        for (int i = 0; i < nc; i++) if (match_of[i] >= 0) { s->matched++; s->world_matched += C[i].world != 0; }
        s->lod = n_lod;
        if (s->cam_move > lim.move) s->snap = VR_SNAP_MOVE;
        else if (s->cam_turn_deg > lim.turn_deg) s->snap = VR_SNAP_TURN;
        else if (s->world >= 8 && s->world_matched * 4 < s->world) s->snap = VR_SNAP_UNMATCHED;
    }
    s->snaps[s->snap]++;
    if (s->snap) { for (int i = 0; i < nc; i++) match_of[i] = -1; }
    else {
        s->objs_sum += (uint64_t)nc; s->matched_sum += (uint64_t)s->matched; s->lod_sum += (uint64_t)s->lod;
        s->world_sum += (uint64_t)s->world; s->world_matched_sum += (uint64_t)s->world_matched;
    }
    have_matches = 1;
    log_frame(d, nd);
}

int vr_interp_prev(int obj)
{
    if (!have_matches || obj <= 0 || obj > ntab[cur]) return 0;
    return match_of[obj - 1] + 1;
}
