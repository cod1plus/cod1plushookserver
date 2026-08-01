/*
 * pose_sync.c - make the SERVER's player pose match what the CLIENT draws.
 *
 * THE cod2x PRINCIPLE, ported properly. In CoD2 the animation controllers run on
 * both sides from one shared file, so the hit skeleton IS the drawn model and they
 * cannot disagree. CoD1 has the same bg code on both sides, but our client mod
 * changes two things the server does not know about:
 *
 *   1. swing_fix locks the drawn torso/legs yaw to the VIEW (tolerance 0, speed 1.0)
 *      and converges the lean channel in ~20ms; the server keeps vanilla speeds and
 *      lags behind by up to its dead zone.
 *   2. lean_fix adds a lateral body shift to tag_origin when leaning (the
 *      "helicopter" fix) - drawn only.
 *
 * Everything perbone_hit.c does about lean placement (mirrored shift, per-side
 * factors, the tilted-helmet corner axis, the dual-skull union) exists to COMPENSATE
 * for those two, from the outside, with numbers calibrated by shooting at a wall.
 * This module removes the cause instead: it reproduces both effects INSIDE the
 * server's own pose, so the tested skeleton is the drawn one by construction and the
 * compensations can be switched off.
 *
 * HOW
 *   - hook BG_Player_DoControllers (RVA 0x1ad71). Before the original runs, force
 *     the swing state the client would have (per stance - see below). After it
 *     returns, add the client's body shift to the controller tag_origin offset and
 *     re-publish it with G_DObjSetLocalTag.
 *   - the shift uses the game's OWN GetLeanFraction (PLT 0x149d0) on
 *     ci->playerAngles[2] and cod2x's K table - the same inputs the client uses, so
 *     no empirical per-side factors are needed.
 *
 * STANCE RULES (disassembly-proven, and the reason v1 of this idea misfired):
 *   - CROUCH/PRONE: the client zeroes ALL 24 lean controllers (cgame 0x496e) AND the
 *     server forces its lean swing target to 0 (game 0x19ef8) => already consistent,
 *     touch NOTHING. v1 forced yaw in every stance and rotated a hidden crouching
 *     player's skeleton out of his cover; that must never happen again.
 *   - STAND: torso+legs yaw = view (what the client draws), body shift applied.
 *   - DEAD: skip.
 *
 * Target: game.mp.i386.so md5 343f99cd67b79ac74aeaa5261f63c011. Every address below
 * is from its .dynsym or was disassembled from it. A prologue check makes a
 * mismatched build a no-op instead of a crash, like the other modules.
 *
 * OFF BY DEFAULT: COD1RELOADED_POSE_SYNC=1 to enable. With it enabled, set
 * COD1RELOADED_PERBONE_LEANFRAC=0 so perbone stops adding its own mirror on top.
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

#define TAG "[pose_sync]"

#define GAME_SO_NAME "game.mp.i386.so"

/* ---- game RVAs (dynsym + disassembly of md5 343f99cd...) ---- */
#define RVA_DOCONTROLLERS   0x01ad71  /* BG_Player_DoControllers(obj, partBits, ci, ft) */
#define RVA_SETLOCALTAG_PLT 0x014e80  /* G_DObjSetLocalTag(obj, partBits, tagName, off, ang) */
#define RVA_GETLEANFRAC_PLT 0x0149d0  /* GetLeanFraction(float) - cod2x's fLeanFrac */
#define RVA_g_entities      0x21d6c0
#define RVA_GOT_BGS         0x089ab4  /* GOT slot -> exported `bgs` */

#define BGS_CLIENTINFO      0x9b6ec   /* verified: + 64*0x4b0 == sizeof(bgs) */
#define CI_STRIDE           0x4b0

/* clientinfo fields (same bg struct as cgame) */
#define CI_LEGS_YAW         0x380
#define CI_TORSO_YAW        0x3b0
#define CI_LERP_LEAN        0x3b8
#define CI_PLAYERANGLES     0x3e0     /* [0]=pitch [1]=yaw(+4) [2]=roll(+8) */
#define CI_CONTROL_ORG_ANG  0x444     /* control.tag_origin_angles (vec3) */
#define CI_CONTROL_ORG_OFF  0x450     /* control.tag_origin_offset (vec3) */
#define CI_CLIENTNUM        0x4ac     /* NOTE: verify - used only for logging */

/* gentity / gclient */
#define GENTITY_SIZE        0x31c
#define E_CLIENT            0x15c
#define E_HEALTH            0x238
#define E_MINS              0x104
#define E_MAXS              0x110
#define PS_LEANF            0x40
#define PS_VIEWYAW          0xc4

#define PS_MAX_CLIENTS      64

/* client body-shift table (cod2x animation.cpp:186-199, mirrored in lean_fix.cpp).
 * STAND only here: CoD1 zeroes the lean controllers in crouch/prone on BOTH sides. */
#define SHIFT_K_LEFT        5.0f
#define SHIFT_K_RIGHT       2.5f
#define SHIFT_RIGHT_SCALE   2.0f   /* = client body_shift_right_scale */

/* `55 | 89 e5 | 53 | 81 ec 94 00 00 00` - push ebp / mov ebp,esp / push ebx /
 * sub esp,0x94. Instruction boundaries are at 0,1,3,4,10, so the 5-byte JMP must
 * steal 10: a 6-byte steal lands INSIDE the 6-byte `sub esp,imm32` and the
 * trampoline then executes its tail as garbage (segfault on the first posed
 * player - hit live, 2026-07-31). */
#define DC_PATCHLEN 10
static const unsigned char DC_PROLOGUE[10] = {
    0x55, 0x89, 0xe5, 0x53, 0x81, 0xec, 0x94, 0x00, 0x00, 0x00
};

typedef void  (*docontrollers_t)(void* obj, int* partBits, void* ci, int frametime);
typedef void  (*setlocaltag_t)(void* obj, int* partBits, unsigned short tag,
                               const float* off, const float* ang);
typedef float (*getleanfrac_t)(float lean);

static hook_t          g_dc_hook;
static docontrollers_t orig_docontrollers = NULL;
static setlocaltag_t   p_SetLocalTag      = NULL;
static getleanfrac_t   p_GetLeanFrac      = NULL;
static uintptr_t       g_base   = 0;
static int             g_enable = 0;
static int             g_shift  = 1;   /* inject the body shift  */
static int             g_yaw    = 1;   /* force the swing yaw    */
static int             g_debug  = 0;
static long            g_hits   = 0;

/* scr_const_tag_origin: the interned "tag_origin" token the game passes to
 * SetLocalTag. Captured from the original call rather than guessed - see below. */
static unsigned short  g_tag_origin = 0;

/* Map a clientinfo pointer back to its client number (the array is contiguous). */
static int ci_index(void* ci)
{
    char* bgs = *(char**)(g_base + RVA_GOT_BGS);
    if (!bgs) return -1;
    size_t d = (size_t)((char*)ci - (bgs + BGS_CLIENTINFO));
    if (d % CI_STRIDE) return -1;
    d /= CI_STRIDE;
    return (d < PS_MAX_CLIENTS) ? (int)d : -1;
}

static void hook_docontrollers(void* obj, int* partBits, void* ci, int frametime)
{
    char* c = (char*)ci;
    int   n = (g_enable && g_base) ? ci_index(ci) : -1;

    /* --- gates: real player, alive, standing, leaning ------------------- */
    int   stand = 0;
    float leanf = 0.0f;
    char* ge    = NULL;
    void* cl    = NULL;
    if (n >= 0) {
        ge = (char*)(g_base + RVA_g_entities) + (size_t)n * GENTITY_SIZE;
        cl = *(void**)(ge + E_CLIENT);
        if (cl && *(int*)(ge + E_HEALTH) > 0) {
            float* mns = (float*)(ge + E_MINS);
            float* mxs = (float*)(ge + E_MAXS);
            stand = (mxs[2] - mns[2]) >= 55.0f;
            leanf = *(float*)((char*)cl + PS_LEANF);
        }
    }
    const int active = (n >= 0 && cl && stand && leanf != 0.0f);

    /* --- pre: force the swing state the client draws -------------------- */
    float saved_torso = 0.0f, saved_legs = 0.0f;
    if (active && g_yaw) {
        saved_torso = *(float*)(c + CI_TORSO_YAW);
        saved_legs  = *(float*)(c + CI_LEGS_YAW);
        const float vy = *(float*)((char*)cl + PS_VIEWYAW);
        *(float*)(c + CI_TORSO_YAW) = vy;
        *(float*)(c + CI_LEGS_YAW)  = vy;
    }

    orig_docontrollers(obj, partBits, ci, frametime);

    if (active && g_yaw) {
        *(float*)(c + CI_TORSO_YAW) = saved_torso;
        *(float*)(c + CI_LEGS_YAW)  = saved_legs;
    }

    /* --- post: add the client's body shift to the published tag_origin --- */
    if (active && g_shift && p_SetLocalTag && p_GetLeanFrac && g_tag_origin) {
        /* cod2x formula, with the game's own GetLeanFraction on the same input the
         * client uses (playerAngles[2]) - so no empirical per-side factor. */
        const float lf = p_GetLeanFrac(*(float*)(c + CI_PLAYERANGLES + 8));
        if (lf != 0.0f) {
            float  off[3], ang[3];
            float* coff = (float*)(c + CI_CONTROL_ORG_OFF);
            float* cang = (float*)(c + CI_CONTROL_ORG_ANG);
            off[0] = coff[0]; off[1] = coff[1]; off[2] = coff[2];
            ang[0] = cang[0]; ang[1] = cang[1]; ang[2] = cang[2];
            const float K = (lf <= 0.0f) ? SHIFT_K_LEFT
                                         : SHIFT_K_RIGHT * SHIFT_RIGHT_SCALE;
            off[1] += -lf * K;                 /* exactly cod2x animation.cpp:193-200 */
            p_SetLocalTag(obj, partBits, g_tag_origin, off, ang);
            if (g_debug && (++g_hits % 200) == 1) {
                printf("%s cn=%d leanf=%.2f lf=%.3f shift=%.2f (yaw forced=%d)\n",
                       TAG, n, leanf, lf, -lf * K, g_yaw);
                fflush(stdout);
            }
        }
    }
}

/* ============================ install ============================ */
static void try_install(void)
{
    if (orig_docontrollers) return;

    uintptr_t base = 0;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, GAME_SO_NAME)) {
            unsigned long lo = strtoul(line, NULL, 16);
            if (!base || lo < base) base = (uintptr_t)lo;
        }
    }
    fclose(f);
    if (!base) return;

    if (memcmp((void*)(base + RVA_DOCONTROLLERS), DC_PROLOGUE, sizeof(DC_PROLOGUE)) != 0) {
        static int logged = 0;
        if (!logged) {
            logged = 1;
            printf("%s prologue mismatch at BG_Player_DoControllers - not installing\n", TAG);
            fflush(stdout);
        }
        return;
    }

    g_base        = base;
    p_SetLocalTag = (setlocaltag_t)(base + RVA_SETLOCALTAG_PLT);
    p_GetLeanFrac = (getleanfrac_t)(base + RVA_GETLEANFRAC_PLT);

    /* The tag token is an immediate in the original's tail: `mov esp+8, <tok>` right
     * before the SetLocalTag call. Read it from the code instead of hardcoding a
     * value that would silently rot if the build changed. */
    {
        const unsigned char* p = (const unsigned char*)(base + RVA_DOCONTROLLERS);
        for (int i = 0; i < 0x160 - 5; ++i) {
            /* lea eax,[ebx-0x14fd8] pattern precedes the tag arg in this build; the
             * tag itself is loaded as a 32-bit immediate into esp+8. */
            if (p[i] == 0xc7 && p[i+1] == 0x44 && p[i+2] == 0x24 && p[i+3] == 0x08) {
                unsigned int imm = *(const unsigned int*)(p + i + 4);
                if (imm && imm < 0x10000) { g_tag_origin = (unsigned short)imm; break; }
            }
        }
    }

    if (hook_install(&g_dc_hook, base + RVA_DOCONTROLLERS,
                     (uintptr_t)hook_docontrollers, DC_PATCHLEN) == 0) {
        orig_docontrollers = (docontrollers_t)g_dc_hook.trampoline;
        printf("%s installed (yaw=%d shift=%d tag_origin=%u, game base 0x%08lx)\n",
               TAG, g_yaw, g_shift, (unsigned)g_tag_origin, (unsigned long)base);
        if (!g_tag_origin)
            printf("%s WARNING: tag token not found - shift injection disabled\n", TAG);
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

void pose_sync_init(void)
{
    const char* e = getenv("COD1RELOADED_POSE_SYNC");
    if (!e || *e == '0' || *e == 'o' || *e == 'f' || *e == 'n') return;
    g_enable = 1;

    const char* y = getenv("COD1RELOADED_POSE_SYNC_YAW");
    if (y && *y) g_yaw = (*y != '0');
    const char* s = getenv("COD1RELOADED_POSE_SYNC_SHIFT");
    if (s && *s) g_shift = (*s != '0');
    const char* d = getenv("COD1RELOADED_POSE_SYNC_DEBUG");
    if (d && *d && *d != '0') g_debug = 1;

    pthread_t tid;
    if (pthread_create(&tid, NULL, watcher_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started (yaw=%d shift=%d)\n", TAG, g_yaw, g_shift);
        fflush(stdout);
    }
}
