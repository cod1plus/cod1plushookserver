/*
 * swing_sync.c - the server-side mirror of the client's swing_fix.cpp.
 *
 * WHY. The client draws every player with these four BG_PlayerAngles constants
 * changed (cgame_mp_x86.dll, swing_fix.cpp):
 *   1. legs swing tolerance      40.0 -> 0.0     (legs never sit in a 40 deg dead zone)
 *   2. lean swing speed          0.15 -> 1.0     (the "torso pitch" push: it is the lean channel)
 *   3. torso yaw swing speed     bg_swingSpeed -> 1.0   (torso site only, legs keep the cvar)
 *   4. torso yaw movement pull   0.3 -> 0.0      (torso no longer drifts toward the run direction)
 * The server's game.mp.i386.so carries the SAME function (shared bg_ source) and runs it
 * every frame to write ci->legs.yawAngle (+0x380), ci->torso.yawAngle (+0x3b0) and the
 * swung lean angle (+0x3b8) - the exact inputs BG_Player_DoControllersInternal reads
 * (0x1a435 / 0x1a441 / 0x1a477) to build the skeleton that a bullet's locational trace
 * walks. With vanilla constants server-side, a player who is not leaning is TESTED with
 * legs up to 40 deg behind his view, a torso pulled 30% toward his movement (27 deg on a
 * full strafe) and a torso/lean that lag his flick - while everyone SEES him locked to
 * the view. pose_sync.c's yaw forcing only engaged while leaning, and forced legs = view,
 * which is what the client draws only for a player standing still.
 *
 * WHAT. Patch the four server sites to the client's values. Same code and same constants
 * means the same result for the same inputs. The swing helper (game 0x19842) scales its
 * step by the frame time (`fild frametime; fmul scale; fmul speed`), so convergence is
 * the same in REAL time on a 250 fps client and a 40 fps server, not per frame - the
 * legs, left at the cvar speed on both sides, stay in step too.
 *
 * SITES (game.mp.i386.so md5 343f99cd67b79ac74aeaa5261f63c011). BG_PlayerAngles is
 * static - no symbol - and was located as the only writer of ci+0x380/+0x3b0 and the
 * only function passing bg_swingSpeed.value to the swing helper (five call sites,
 * torso first, then four legs sites, then the lean channel with its 45 deg clamp -
 * one-to-one with the five dvar readers swing_fix.cpp documents on the client):
 *   0x19c75  d9 83 a0 af fe ff         fld [ebx-0x15060]      .rodata 0x73d38 = 0.3f (sole reader)
 *   0x19cb3  8b 83 28 0d 00 00         mov eax,[ebx+0xd28]    GOT slot of bg_swingSpeed
 *   0x19cb9  8b 40 08                  mov eax,[eax+0x8]      .value
 *   0x19cbc  89 44 24 0c               mov [esp+0xc],eax      speed argument, TORSO call
 *   0x19e55  c7 44 24 04 00 00 20 42   mov [esp+0x4],40.0f    legs tolerance, "not yawing" site
 *   0x19f8b  c7 44 24 0c 9a 99 19 3e   mov [esp+0xc],0.15f    lean channel speed (clamp 45 follows)
 * Every site is compared byte-for-byte before anything is written: a different build
 * gets nothing and says so once. The game module is reloaded on every map change, so a
 * watcher re-checks and re-applies, exactly like pose_sync / antilag.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include "hooks.h"
#include "swing_sync.h"

#define TAG "[swing_sync]"
#define GAME_SO_NAME "game.mp.i386.so"

typedef struct {
    uintptr_t            rva;
    const unsigned char* orig;
    const unsigned char* patched;
    int                  len;
    const char*          what;
} site_t;

/* fld dword [ebx-0x15060]  ->  fldz ; nop x4 : torso target = view + moveYaw * 0 */
static const unsigned char MOVEFRAC_ORIG[6] = { 0xd9, 0x83, 0xa0, 0xaf, 0xfe, 0xff };
static const unsigned char MOVEFRAC_NEW[6]  = { 0xd9, 0xee, 0x90, 0x90, 0x90, 0x90 };

/* bg_swingSpeed.value -> [esp+0xc]   ->   mov dword [esp+0xc], 1.0f ; nop x5 */
static const unsigned char TORSO_ORIG[13] = { 0x8b, 0x83, 0x28, 0x0d, 0x00, 0x00,
                                              0x8b, 0x40, 0x08,
                                              0x89, 0x44, 0x24, 0x0c };
static const unsigned char TORSO_NEW[13]  = { 0xc7, 0x44, 0x24, 0x0c, 0x00, 0x00, 0x80, 0x3f,
                                              0x90, 0x90, 0x90, 0x90, 0x90 };

/* mov dword [esp+0x4], 40.0f  ->  0.0f */
static const unsigned char LEGS_ORIG[8] = { 0xc7, 0x44, 0x24, 0x04, 0x00, 0x00, 0x20, 0x42 };
static const unsigned char LEGS_NEW[8]  = { 0xc7, 0x44, 0x24, 0x04, 0x00, 0x00, 0x00, 0x00 };

/* mov dword [esp+0xc], 0.15f  ->  1.0f */
static const unsigned char LEAN_ORIG[8] = { 0xc7, 0x44, 0x24, 0x0c, 0x9a, 0x99, 0x19, 0x3e };
static const unsigned char LEAN_NEW[8]  = { 0xc7, 0x44, 0x24, 0x0c, 0x00, 0x00, 0x80, 0x3f };

static const site_t SITES[] = {
    { 0x019c75, MOVEFRAC_ORIG, MOVEFRAC_NEW, 6,  "torso movement pull 0.3 -> 0" },
    { 0x019cb3, TORSO_ORIG,    TORSO_NEW,    13, "torso yaw swing speed -> 1.0" },
    { 0x019e55, LEGS_ORIG,     LEGS_NEW,     8,  "legs swing tolerance 40 -> 0" },
    { 0x019f8b, LEAN_ORIG,     LEAN_NEW,     8,  "lean swing speed 0.15 -> 1.0" },
};
#define NSITES ((int)(sizeof(SITES) / sizeof(SITES[0])))

static volatile int g_installed = 0;
static uintptr_t    g_base      = 0;

int swing_sync_installed(void) { return g_installed; }

static uintptr_t find_game_base(void)
{
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    uintptr_t base = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, GAME_SO_NAME)) continue;
        uintptr_t s = (uintptr_t)strtoul(line, NULL, 16);
        if (s && (base == 0 || s < base)) base = s;
    }
    fclose(f);
    return base;
}

static void try_install(void)
{
    static int logged_mismatch = 0;
    uintptr_t base = find_game_base();
    int i, need = 0;

    if (!base) {                      /* between maps: module unloaded */
        g_installed = 0;
        g_base = 0;
        return;
    }

    /* Each site must read either the vanilla bytes (fresh load) or ours (already done).
     * Anything else is a different build: touch nothing. */
    for (i = 0; i < NSITES; ++i) {
        const unsigned char* p = (const unsigned char*)(base + SITES[i].rva);
        if (memcmp(p, SITES[i].orig, SITES[i].len) == 0) {
            need = 1;
        } else if (memcmp(p, SITES[i].patched, SITES[i].len) != 0) {
            if (!logged_mismatch) {
                logged_mismatch = 1;
                printf("%s byte mismatch at game+0x%lx (%s): %02x %02x %02x %02x - "
                       "different game.mp.i386.so build? not installing\n",
                       TAG, (unsigned long)SITES[i].rva, SITES[i].what,
                       p[0], p[1], p[2], p[3]);
                fflush(stdout);
            }
            g_installed = 0;
            return;
        }
    }

    if (!need) {                      /* all four already ours (same load) */
        if (!g_installed) { g_installed = 1; g_base = base; }
        return;
    }

    for (i = 0; i < NSITES; ++i) {
        unsigned char* p = (unsigned char*)(base + SITES[i].rva);
        if (memcmp(p, SITES[i].orig, SITES[i].len) != 0) continue;   /* already ours */
        if (hook_unprotect((uintptr_t)p, SITES[i].len) != 0) {
            printf("%s cannot unprotect game+0x%lx - not installing\n",
                   TAG, (unsigned long)SITES[i].rva);
            fflush(stdout);
            g_installed = 0;
            return;
        }
        memcpy(p, SITES[i].patched, SITES[i].len);
    }

    g_base = base;
    g_installed = 1;
    logged_mismatch = 0;
    printf("%s installed (game base 0x%08lx): %d/%d sites - the server now swings "
           "legs/torso/lean exactly as the client draws them\n",
           TAG, (unsigned long)base, NSITES, NSITES);
    fflush(stdout);
}

static void* watcher_thread(void* arg)
{
    (void)arg;
    for (;;) { try_install(); usleep(400 * 1000); }
    return NULL;
}

void swing_sync_init(void)
{
    const char* e = getenv("COD1RELOADED_SWING_SYNC");
    pthread_t tid;

    if (e && *e && (!strcmp(e, "0") || !strcasecmp(e, "off") ||
                    !strcasecmp(e, "false") || !strcasecmp(e, "no"))) {
        printf("%s disabled by COD1RELOADED_SWING_SYNC - the server keeps the vanilla "
               "swing dead zones; pose_sync falls back to lean-only yaw forcing\n", TAG);
        fflush(stdout);
        return;
    }
    if (pthread_create(&tid, NULL, watcher_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started\n", TAG);
    } else {
        printf("%s failed to start watcher\n", TAG);
    }
    fflush(stdout);
}
