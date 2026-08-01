/*
 * lean_hitbox.c - make the server bullet hitbox follow the lean (anti "clip").
 *
 * Target: game.mp.i386.so (the CoD1 v1.5 Linux game module), md5
 *         343f99cd67b79ac74aeaa5261f63c011. RVAs below are offsets within that
 *         module; it is PIC and loaded at a runtime base we resolve from
 *         /proc/self/maps. The module is reloaded (and may be rebased) on every
 *         map change, so a watcher re-installs the hook each time. A runtime
 *         prologue check makes a mismatched binary a no-op instead of a crash.
 *
 * Mechanism (RE-verified):
 *   - The bullet trace clips a single AABB anchored at gentity.r.currentOrigin
 *     (engine CM_TransformedBoxTrace reads it LIVE at trace time).
 *   - ClientThink_real sets r.currentOrigin = ps.origin (un-leaned) and links;
 *     the lean (G_AddLean/AddLeanToPosition, ~20u right) is applied only to the
 *     eye/muzzle/damage points, never to the box -> leaning body is off its box.
 *   - We run after ClientThink_real and WIDEN r.mins/r.maxs on the lean side by the
 *     full AddLeanToPosition offset, then re-link so absmin/absmax (broadphase) and
 *     the fine trace both cover the leaned body.
 *
 *   Earlier revisions TRANSLATED the box by a fraction of the lean instead. That
 *   cannot work: the body pivots about the feet, so feet and head need different
 *   offsets, and any single translation leaves one of them outside the box - in
 *   practice the head, giving "standing + lean right and you cannot be shot in the
 *   head". The swept volume is the only correct answer for one AABB.
 *
 * Render safety: r.mins/r.maxs are collision-only. The rendered model leans
 * client-side from es.leanf and is untouched, and r.currentOrigin is left alone, so
 * movement, prediction and the visible player are all unaffected.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>

#include "hooks.h"

#define TAG "[lean_hitbox]"

/* MUST MATCH mss32/lean_fix.cpp body_shift_lean_scale / body_shift_right_scale.
 * The client draws the body this much further out than the engine lean; the box has to
 * follow it or the leaning player is visible but unhittable. */
#define LEAN_BODY_SHIFT_SCALE        1.0f
#define LEAN_BODY_SHIFT_RIGHT_SCALE  2.0f  /* = client body_shift_right_scale; MODULE DISABLED (bullet no-op + movement wall) - if ever re-enabled, note the client draws NO shift in crouch */

#define GAME_SO_NAME           "game.mp.i386.so"

/* game.mp.i386.so RVAs (md5 343f99cd67b79ac74aeaa5261f63c011) */
#define RVA_CLIENTTHINK_REAL   0x39116
#define RVA_TRAP_LINKENTITY    0x68313
#define RVA_ADDLEANTOPOSITION  0x71a7c

/* ClientThink_real entry: push ebp; mov ebp,esp; push esi; push ebx (5 bytes,
 * clean instruction boundary -> a safe 5-byte detour cut point). */
static const unsigned char CT_PROLOGUE[5] = { 0x55, 0x89, 0xe5, 0x56, 0x53 };

/* gentity_t offsets. mins/maxs confirmed by disassembly of this exact binary: the
 * module writes 3-float groups at 0x104/0x108/0x10c and 0x110/0x114/0x118, immediately
 * before contents (0x11c), absmin (0x120), absmax (0x12c) and currentOrigin (0x138) -
 * the standard entityShared_t order, anchored by the already-known 0x138. */
#define E_NUMBER    0x00    /* gentity->s.number (entityState_t starts with it) */
#define E_MINS      0x104   /* gentity->r.mins   (vec3)              */
#define E_MAXS      0x110   /* gentity->r.maxs   (vec3)              */
#define E_CLIENT    0x15c   /* gentity->client (gclient*)            */
#define E_ORIGIN    0x138   /* gentity->r.currentOrigin  (vec3)      */

/* gclient/playerState offsets — binary-verified (the cod1_defs.h playerState_t
 * top layout is skewed by a few bytes; use raw offsets like G_AddLean does). */
#define PS_LEANF    0x40    /* ps.leanf          (G_AddLean reads client+0x40) */
#define PS_VIEWYAW  0xc4    /* ps.viewangles[1]  (G_AddLean reads client+0xc4) */

/* ClientThink_real(gentity_t *ent, usercmd_t *cmd) — TWO args. The 2nd (cmd) is
 * dereferenced early (serverTime clamp), so it MUST be forwarded or the original
 * reads garbage off our stack and segfaults. */
typedef void  (*ClientThink_real_t)(void *ent, void *cmd);
typedef void  (*trap_LinkEntity_t)(void *ent);
typedef void  (*AddLeanToPosition_t)(float *pos, float yaw, float leanf,
                                     float scaleRoll, float scaleLat);

static hook_t              g_think_hook;
static ClientThink_real_t  orig_ClientThink_real = NULL;
static trap_LinkEntity_t   p_trap_LinkEntity     = NULL;
static AddLeanToPosition_t p_AddLeanToPosition   = NULL;
static uintptr_t           g_game_base           = 0;

/* Fraction of the head's lean offset the box is widened by.
 * 1.0 = cover it fully. Anything less leaves the head outside the box, which is the
 * bug this module exists to fix, so only lower it if the widened box turns out too
 * generous in play. Tunable: COD1RELOADED_LEAN_HITBOX_FRAC. */
static float               g_lean_frac           = 1.0f;

/* Saved un-leaned box per entity, so widening never accumulates frame over frame. */
#define LH_MAX_ENT 64
static float g_saved_mins[LH_MAX_ENT][3];
static float g_saved_maxs[LH_MAX_ENT][3];
static int   g_saved[LH_MAX_ENT];

/* The hook: after the engine finished the think (currentOrigin = ps.origin and the
 * entity is linked), make the collision box cover the leaned body and re-link.
 *
 * WIDEN, DO NOT TRANSLATE. The body pivots about the feet: the feet stay on the
 * origin while the head swings ~20 units to the side. Translating the whole box by a
 * fraction of that (this module used 0.5) centres it on neither - it leaves the feet
 * over-shifted AND the head hanging outside the box, which is exactly the reported
 * "standing + lean right, cannot be shot in the head". A single AABB cannot follow a
 * tilted body, so the honest answer is the swept volume: keep the box where it is and
 * extend it on the lean side by the full head offset. Feet and head both stay
 * hittable. The cost is a little over-coverage on the lean side, bounded by the lean
 * distance, which is the right way round for a competitive server.
 */
static void hook_ClientThink_real(void *ent, void *cmd)
{
    char *cl0 = *(char **)((char *)ent + E_CLIENT);
    int   num = *(int *)((char *)ent + E_NUMBER);

    /* Undo last frame's widening before the engine thinks, so nothing accumulates
     * even if the engine does not rewrite the box every frame. */
    if (cl0 && num >= 0 && num < LH_MAX_ENT && g_saved[num]) {
        float *mins = (float *)((char *)ent + E_MINS);
        float *maxs = (float *)((char *)ent + E_MAXS);
        for (int i = 0; i < 3; i++) {
            mins[i] = g_saved_mins[num][i];
            maxs[i] = g_saved_maxs[num][i];
        }
        g_saved[num] = 0;
    }

    orig_ClientThink_real(ent, cmd);                    /* full think + link    */

    char *cl = *(char **)((char *)ent + E_CLIENT);      /* gentity->client      */
    if (!cl) return;                                    /* players only         */
    if (num < 0 || num >= LH_MAX_ENT) return;

    float leanf = *(float *)(cl + PS_LEANF);
    if (leanf == 0.0f) return;                          /* not leaning: no-op   */

    float *mins = (float *)((char *)ent + E_MINS);
    float *maxs = (float *)((char *)ent + E_MAXS);

    /* Fail-safe: if these are not a player box, the offsets are wrong for this
     * binary - do nothing rather than corrupt the entity. */
    if (!(mins[0] > -48.0f && mins[0] < -4.0f &&
          maxs[0] >   4.0f && maxs[0] <  48.0f &&
          maxs[2] >  16.0f && maxs[2] < 128.0f))
        return;

    float  yaw = *(float *)(cl + PS_VIEWYAW);
    float off[3] = { 0.0f, 0.0f, 0.0f };
    p_AddLeanToPosition(off, yaw, leanf, 16.0f, 20.0f); /* off = full head offset */

    for (int i = 0; i < 3; i++) {
        g_saved_mins[num][i] = mins[i];
        g_saved_maxs[num][i] = maxs[i];
    }
    g_saved[num] = 1;

    /* The client draws the body FURTHER out than the engine lean. Its animation
     * controller adds tag_origin_offset[1] on top (CoD2 vanilla behaviour, ported in
     * mss32/lean_fix.cpp): the "helicopter" fix, without which a peeker shows an arm and
     * nothing else. That extra offset has to be in the box too, or the model stands
     * beside its own hitbox and cannot be shot - which is exactly what happened when the
     * client-side shift was briefly removed instead of mirrored.
     *
     * Deterministic from leanf and stance, so both sides compute it independently and no
     * network sync is needed. KEEP IN STEP WITH lean_fix.cpp's body_shift_* config. */
    const float crouch_h = 55.0f;                 /* standing box ~70, crouched ~40 */
    const int   is_crouch = (maxs[2] - mins[2]) < crouch_h;
    const int   is_left   = (leanf < 0.0f);
    float K = is_crouch ? (is_left ? 12.5f : 2.5f) : (is_left ? 5.0f : 2.5f);
    if (!is_left) K *= LEAN_BODY_SHIFT_RIGHT_SCALE;
    const float body_extra = K * fabsf(leanf) * LEAN_BODY_SHIFT_SCALE;

    /* body_extra runs along the same lateral direction as the eye offset */
    float mag = sqrtf(off[0] * off[0] + off[1] * off[1]);
    float ux = 0.0f, uy = 0.0f;
    if (mag > 0.01f) { ux = off[0] / mag; uy = off[1] / mag; }

    float total[2] = { (off[0] + ux * body_extra) * g_lean_frac,
                       (off[1] + uy * body_extra) * g_lean_frac };

    /* extend on the lean side only (x/y); the head does not change the box height */
    for (int i = 0; i < 2; i++) {
        if (total[i] > 0.0f) maxs[i] += total[i];
        else                 mins[i] += total[i];
    }
    p_trap_LinkEntity(ent);                             /* re-file widened box  */
}

/* Lowest mapping of game.mp.i386.so == its load base (first PT_LOAD, vaddr 0). */
static uintptr_t find_game_base(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;

    char      line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, GAME_SO_NAME)) continue;
        uintptr_t start = (uintptr_t)strtoul(line, NULL, 16);
        if (start && (base == 0 || start < base)) base = start;
    }
    fclose(f);
    return base;
}

/* Install (or re-install after a map reload). Idempotent + fail-safe:
 *   - 0xE9 at the entry and same base  -> already installed, nothing to do.
 *   - original prologue present        -> fresh (re)load, (re)install.
 *   - anything else                    -> wrong binary, do not touch. */
static void try_install(void)
{
    static int logged_wait = 0;

    uintptr_t base = find_game_base();
    if (!base) return;                                  /* module not loaded yet */

    unsigned char *target = (unsigned char *)(base + RVA_CLIENTTHINK_REAL);

    if (base == g_game_base && target[0] == 0xE9)
        return;                                         /* our hook still live   */

    if (memcmp(target, CT_PROLOGUE, sizeof(CT_PROLOGUE)) != 0) {
        if (target[0] != 0xE9 && !logged_wait) {
            printf("%s game module prologue mismatch @0x%08lx (got %02x %02x %02x ...) "
                   "- different game.mp.i386.so build? hook disabled\n",
                   TAG, (unsigned long)(uintptr_t)target,
                   target[0], target[1], target[2]);
            fflush(stdout);
            logged_wait = 1;
        }
        return;
    }

    p_trap_LinkEntity   = (trap_LinkEntity_t)(base + RVA_TRAP_LINKENTITY);
    p_AddLeanToPosition = (AddLeanToPosition_t)(base + RVA_ADDLEANTOPOSITION);

    if (hook_install(&g_think_hook, (uintptr_t)target,
                     (uintptr_t)hook_ClientThink_real, 5) == 0) {
        orig_ClientThink_real = (ClientThink_real_t)g_think_hook.trampoline;
        g_game_base = base;
        logged_wait = 0;
        printf("%s installed: hitbox follows lean (game base 0x%08lx)\n",
               TAG, (unsigned long)base);
        fflush(stdout);
    } else {
        printf("%s hook_install failed @0x%08lx\n",
               TAG, (unsigned long)(uintptr_t)target);
        fflush(stdout);
    }
}

static void *watcher_thread(void *arg)
{
    (void)arg;
    for (;;) {
        try_install();
        usleep(400 * 1000);   /* 400ms: re-install fast after a map (re)load */
    }
    return NULL;
}

void lean_hitbox_init(void)
{
    const char *e = getenv("COD1RELOADED_LEAN_HITBOX");
    if (e && (*e == '0' || *e == 'f' || *e == 'F' || *e == 'n' || *e == 'N')) {
        printf("%s disabled (COD1RELOADED_LEAN_HITBOX=%s)\n", TAG, e);
        return;
    }

    const char *s = getenv("COD1RELOADED_LEAN_HITBOX_FRAC");
    if (s && *s) {
        float v = (float)atof(s);
        if (v >= 0.0f && v <= 2.0f) g_lean_frac = v;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, watcher_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started (hitbox follows lean, frac=%.2f)\n", TAG, g_lean_frac);
    } else {
        printf("%s failed to start watcher thread\n", TAG);
    }
}
