/*
 * hitbox_draw.c - draw the SERVER's tested hitbox as debug lines, so the desync between
 * "what the shooter sees" and "what the bullet tests" can be LOOKED AT instead of
 * inferred from numbers.
 *
 * WHY THIS EXISTS
 * Three days were spent reading miss-distances out of logs, with two false leads caused
 * by the measuring instrument perturbing the thing it measured (perbone's `dump` mode
 * poses the skeleton before the engine trace). The actual root cause was finally
 * identified from a player's sentence - "the upper outer corner is not hittable". A
 * picture would have shown that in ten seconds.
 *
 * WHAT IT DRAWS
 * The oriented box around `Bip01 Head`, built from the head bone's OWN axes, at the
 * position the SERVER poses it - i.e. the thing bullets are actually tested against.
 * The engine's real head volume is an oriented convex hull attached to that bone
 * (verified in cod_lnxded: per-part setup 0x80c9f14, leaf 0x80c53ee decompresses
 * int16-quantised planes and combines them with the posed bone matrix), so an oriented
 * box from the same matrix is the right approximation - NOT the vertical capsule
 * perbone_hit.c models, which is what made its crouch/lean calibration chase its tail.
 *
 * The client can already draw its own side with `r_xdebug` on a /devmap: those are the
 * XModel collision boxes on the DRAWN skeleton. Run both and the gap is visible.
 *
 * SAFETY
 *   - OFF unless COD1RELOADED_HITBOX_DRAW=1. When off, nothing is hooked and no code runs.
 *   - Hooks G_RunFrame (once per server frame), NOT the pose path: reading bone matrices
 *     from inside BG_Player_DoControllers would re-enter the very system that produces
 *     them.
 *   - G_DebugLine (0x048264) is a real function of the module, not a PLT stub: it sets up
 *     its own %ebx (`call <thunk>; add ebx,...`) before reaching its import. Calling a PLT
 *     stub from our module is what segfaulted the live server on 2026-08-11 - see
 *     pose_sync.c's RVA_GOT_SETLOCALTAG comment. This one is safe to call directly.
 *   - Install refuses unless the prologue bytes match, like every other module here.
 *
 * WHETHER IT REACHES CLIENTS IS UNPROVEN. On idTech3-derived engines the debug-line
 * buffer usually lives in the RENDERER, which a dedicated server does not have. If
 * nothing appears in-game, that is the answer, and the fallback is a server->client
 * command plus a client-side draw (the client mod already has a CG_Draw2D hook and a
 * documented render-bone hook). Test on a dev server first - never during a match.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>

#include "hooks.h"

#define TAG "[hitbox_draw]"
#define GAME_SO_NAME "game.mp.i386.so"

/* ---- game RVAs (md5 343f99cd67b79ac74aeaa5261f63c011) ---- */
#define RVA_G_RUNFRAME      0x0505f5  /* .dynsym, size 0x6a9 - once per server frame */
#define RVA_G_DEBUGLINE     0x048264  /* .dynsym, size 0x3f  - (start, end, color, depth) */
#define RVA_G_DOBJCALCPOSE  0x06c7e4  /* void(ent) */
#define RVA_G_DOBJWORLDTAG  0x06c973  /* int(ent, const char* bone, float out[16]) */
#define RVA_G_ENTITIES      0x21d6c0

#define GENTITY_SIZE   0x31c
#define E_ORIGIN       0x138
#define E_CLIENT       0x15c
#define E_HEALTH       0x238
#define MAX_DRAW_CLIENTS 64

/* `55 89 e5 56 53` then `sub esp,0x430` at +5: a 5-byte detour lands on a boundary. */
#define RF_PATCHLEN 5
static const unsigned char RF_PROLOGUE[RF_PATCHLEN] = { 0x55, 0x89, 0xe5, 0x56, 0x53 };

typedef void (*runframe_t)(int levelTime);
typedef void (*debugline_t)(const float* start, const float* end,
                            const float* color, int depthTest);
typedef void (*calcpose_t)(void* ent);
typedef int  (*worldtag_t)(void* ent, const char* bone, float* out16);

static hook_t      g_rf_hook;
static runframe_t  orig_runframe = NULL;
static debugline_t p_DebugLine   = NULL;
static calcpose_t  p_CalcPose    = NULL;
static worldtag_t  p_WorldTag    = NULL;
static uintptr_t   g_base   = 0;
static int         g_enable = 0;
static long        g_frames = 0;

/* Half-extents of the drawn head box, in units. The engine's real hull comes from the
 * model asset (quantised planes in xmodelparts/character_soviet_coat1) and is not a
 * number in any binary, so these are the best available estimate: ~5 x 5 laterally and
 * ~6 tall, centred a few units up the bone's local +Z because `Bip01 Head` sits at the
 * BASE of the skull (measured: impacts land 4-7u above the bone origin). */
static float g_hx = 5.0f, g_hy = 5.0f, g_hz = 6.0f, g_hup = 4.0f;

static void draw_line(const float* a, const float* b, const float* col)
{
    if (p_DebugLine) p_DebugLine(a, b, col, 0);
}

/* Wireframe of an oriented box: centre + three axis vectors already scaled to the
 * half-extents. 12 edges over the 8 corners. */
static void draw_obb(const float* c, const float* ax, const float* ay, const float* az,
                     const float* col)
{
    float p[8][3];
    int i;
    for (i = 0; i < 8; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f;
        const float sy = (i & 2) ? 1.0f : -1.0f;
        const float sz = (i & 4) ? 1.0f : -1.0f;
        p[i][0] = c[0] + ax[0]*sx + ay[0]*sy + az[0]*sz;
        p[i][1] = c[1] + ax[1]*sx + ay[1]*sy + az[1]*sz;
        p[i][2] = c[2] + ax[2]*sx + ay[2]*sy + az[2]*sz;
    }
    /* edges: pairs of corner indices differing by exactly one bit */
    for (i = 0; i < 8; ++i) {
        if (!(i & 1)) draw_line(p[i], p[i | 1], col);
        if (!(i & 2)) draw_line(p[i], p[i | 2], col);
        if (!(i & 4)) draw_line(p[i], p[i | 4], col);
    }
}

static void draw_one(char* ge)
{
    float m[16];
    float cx[3], cy[3], cz[3], centre[3];
    /* red = the server's tested pose, to contrast with the client's own r_xdebug boxes */
    static const float RED[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    int k;

    if (!p_WorldTag(ge, "Bip01 Head", m)) return;

    /* Row-major 4x4 as the rest of this codebase reads it: translation at [12..14],
     * axes in the first three rows. Scale each axis to its half-extent. */
    for (k = 0; k < 3; ++k) {
        cx[k] = m[0 + k] * g_hx;
        cy[k] = m[4 + k] * g_hy;
        cz[k] = m[8 + k] * g_hz;
    }
    /* lift the centre up the bone's own +Z: the bone sits at the skull base */
    for (k = 0; k < 3; ++k)
        centre[k] = m[12 + k] + m[8 + k] * g_hup;

    draw_obb(centre, cx, cy, cz, RED);
}

static void hook_runframe(int levelTime)
{
    runframe_t orig = orig_runframe;
    if (!orig) orig = (runframe_t)g_rf_hook.trampoline;
    if (orig) orig(levelTime);

    if (!g_enable || !g_base || !p_WorldTag || !p_DebugLine) return;

    ++g_frames;
    for (int i = 0; i < MAX_DRAW_CLIENTS; ++i) {
        char* ge = (char*)(g_base + RVA_G_ENTITIES) + (size_t)i * GENTITY_SIZE;
        if (!*(void**)(ge + E_CLIENT))      continue;
        if (*(int*)(ge + E_HEALTH) <= 0)    continue;
        if (p_CalcPose) p_CalcPose(ge);     /* refresh the pose we are about to read */
        draw_one(ge);
    }
}

/* ============================ install ============================ */
static void try_install(void)
{
    uintptr_t base = 0;
    FILE* f;
    char line[512];

    if (orig_runframe) return;

    f = fopen("/proc/self/maps", "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, GAME_SO_NAME)) {
            unsigned long lo = strtoul(line, NULL, 16);
            if (!base || lo < base) base = (uintptr_t)lo;
        }
    }
    fclose(f);
    if (!base) return;

    if (memcmp((const void*)(base + RVA_G_RUNFRAME), RF_PROLOGUE, RF_PATCHLEN) != 0) {
        static int logged = 0;
        if (!logged) {
            logged = 1;
            printf("%s prologue mismatch at G_RunFrame - not installing\n", TAG);
            fflush(stdout);
        }
        return;
    }

    g_base      = base;
    p_DebugLine = (debugline_t)(base + RVA_G_DEBUGLINE);
    p_CalcPose  = (calcpose_t) (base + RVA_G_DOBJCALCPOSE);
    p_WorldTag  = (worldtag_t) (base + RVA_G_DOBJWORLDTAG);

    if (hook_install(&g_rf_hook, base + RVA_G_RUNFRAME,
                     (uintptr_t)hook_runframe, RF_PATCHLEN) == 0) {
        orig_runframe = (runframe_t)g_rf_hook.trampoline;
        printf("%s installed - drawing the SERVER pose head box in RED "
               "(half-extents %.1f/%.1f/%.1f, up %.1f, game base 0x%08lx) "
               "[build " __DATE__ " " __TIME__ "]\n",
               TAG, g_hx, g_hy, g_hz, g_hup, (unsigned long)base);
        printf("%s if nothing appears in-game the engine does not forward debug lines "
               "from a dedicated server; use r_xdebug client-side meanwhile\n", TAG);
        fflush(stdout);
    } else {
        printf("%s hook_install failed\n", TAG);
        fflush(stdout);
    }
}

static void* watcher_thread(void* arg)
{
    (void)arg;
    for (;;) { try_install(); usleep(400 * 1000); }
    return NULL;
}

void hitbox_draw_init(void)
{
    const char* e = getenv("COD1RELOADED_HITBOX_DRAW");
    const char* q;
    pthread_t tid;

    if (!e || !*e || *e == '0' || *e == 'o' || *e == 'f' || *e == 'n') return;
    g_enable = 1;

    if ((q = getenv("COD1RELOADED_HITBOX_DRAW_HX")) && *q) g_hx  = (float)atof(q);
    if ((q = getenv("COD1RELOADED_HITBOX_DRAW_HY")) && *q) g_hy  = (float)atof(q);
    if ((q = getenv("COD1RELOADED_HITBOX_DRAW_HZ")) && *q) g_hz  = (float)atof(q);
    if ((q = getenv("COD1RELOADED_HITBOX_DRAW_UP")) && *q) g_hup = (float)atof(q);

    if (pthread_create(&tid, NULL, watcher_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started (DEV ONLY - never enable during a match)\n", TAG);
        fflush(stdout);
    }
}
