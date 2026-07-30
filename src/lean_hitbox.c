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
 *   - We run after ClientThink_real, add the SAME AddLeanToPosition offset to
 *     r.currentOrigin, and re-link so absmin/absmax (broadphase) and the fine
 *     trace both use the leaned position. The box now follows the visible lean.
 *
 * Render safety: es.pos.trBase is built from ps.origin (BG_PlayerStateToEntity-
 * State), not r.currentOrigin, so moving currentOrigin moves ONLY the collision
 * hull; the rendered model (which leans client-side from es.leanf) is unchanged.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include "hooks.h"

#define TAG "[lean_hitbox]"

#define GAME_SO_NAME           "game.mp.i386.so"

/* game.mp.i386.so RVAs (md5 343f99cd67b79ac74aeaa5261f63c011) */
#define RVA_CLIENTTHINK_REAL   0x39116
#define RVA_TRAP_LINKENTITY    0x68313
#define RVA_ADDLEANTOPOSITION  0x71a7c

/* ClientThink_real entry: push ebp; mov ebp,esp; push esi; push ebx (5 bytes,
 * clean instruction boundary -> a safe 5-byte detour cut point). */
static const unsigned char CT_PROLOGUE[5] = { 0x55, 0x89, 0xe5, 0x56, 0x53 };

/* gentity_t offsets */
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

/* Fraction of the eye's lean offset applied to the hitbox. The visible model
 * leans as a tilt (head ~full, feet ~0), and the box is wide, so ~0.5 best
 * centres the box over the tilted body. 1.0 = full eye offset (over-extends).
 * Tunable: COD1RELOADED_LEAN_HITBOX_FRAC. */
static float               g_lean_frac           = 0.5f;

/* The hook: after the engine finished the think (currentOrigin = ps.origin and
 * the entity is linked), shift the collision box by the lean and re-link. */
static void hook_ClientThink_real(void *ent, void *cmd)
{
    orig_ClientThink_real(ent, cmd);                    /* full think + link    */

    char *cl = *(char **)((char *)ent + E_CLIENT);      /* gentity->client      */
    if (!cl) return;                                    /* players only         */

    float leanf = *(float *)(cl + PS_LEANF);
    if (leanf == 0.0f) return;                          /* not leaning: no-op   */

    float  yaw = *(float *)(cl + PS_VIEWYAW);
    float *org = (float *)((char *)ent + E_ORIGIN);     /* r.currentOrigin      */

    /* Compute the FULL eye lean offset (into a zero vec), then apply g_lean_frac
     * of it. A fraction (~0.5) best centres the wide box over the tilted body
     * instead of over-shifting it past the head. */
    float off[3] = { 0.0f, 0.0f, 0.0f };
    p_AddLeanToPosition(off, yaw, leanf, 16.0f, 20.0f); /* off = full eye offset */
    org[0] += off[0] * g_lean_frac;
    org[1] += off[1] * g_lean_frac;
    org[2] += off[2] * g_lean_frac;
    p_trap_LinkEntity(ent);                             /* re-file leaned box   */
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
