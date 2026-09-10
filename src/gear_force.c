/*
 * gear_force.c - g_useGear is 0 on this server. Always. Before the scripts can read it.
 *
 * WHY. The stock character scripts attach backpacks / bandoliers / ammo belts to every
 * player while the server cvar g_useGear is non-zero. That gear is attached with
 * ignoreCollision: the engine skips every bone of a masked model before the radius,
 * priority and geometry tests (cod_lnxded 0x80d22c9), so bullets never see it. It is a
 * silhouette 8-17 units wider than anything that can be hit, and players aim at it.
 *
 * WHERE. competitive_sv.c already forces the cvar from the game thread, but only from
 * the first G_RunFrame after a module load - and the scripts precache their gear inside
 * G_InitGame, which the engine calls BEFORE the first frame. On the first map after a
 * server start the archived config (seta g_useGear "1") therefore still wins the
 * precache. This file closes that window at the only point that is early enough: the
 * game module's entry. Sys_LoadDll does dlsym("dllEntry"), dlsym("vmMain"), then calls
 * dllEntry(syscalls) and only then vmMain(GAME_INIT). We interpose dlsym: when the engine
 * asks the game module for "vmMain", it gets our wrapper, which forces the cvar through
 * the game's own trap_Cvar_Set (dllEntry has run by then, so the syscall vector is live)
 * and then calls the real vmMain. Every load, every restart, whatever any config says.
 *
 * PORTABILITY. No GNU extension is used on purpose: RTLD_NEXT is spelled out, the real
 * dlsym is reached through dlvsym (a different symbol, so no recursion) declared by hand,
 * and "is this address inside game.mp.i386.so" is answered from /proc/self/maps like every
 * other module here does - not dladdr. Turning _GNU_SOURCE on for every translation unit
 * would re-enable the glibc 2.38 __isoc23_* redirect through atoi() and friends, and the
 * build guard would (rightly) reject the module. See glibc_compat.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#include "gear_force.h"

#define TAG "[gear]"
#define GAME_SO_NAME "game.mp.i386.so"
#define RVA_TRAP_CVAR_SET 0x067ad7   /* trap_Cvar_Set(name, value), same as competitive_sv.c */
#define GAME_INIT 2                  /* vmMain command -> G_InitGame. NOT 0 as in Quake 3: CoD1's
                                      * table (game .rodata 0x7aa24, 24 entries) has 0 = default,
                                      * 1 = api version (returns 5), 2 = G_InitGame @0x4d8e3. */

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void*)-1L)       /* glibc's value; the header only exposes it with _GNU_SOURCE */
#endif
extern void* dlvsym(void* handle, const char* symbol, const char* version);

typedef int (*vmmain_t)(int, int, int, int, int, int, int, int, int, int, int, int, int);
typedef void (*trap_cvar_set_t)(const char*, const char*);

static vmmain_t  g_real_vmmain = NULL;
static uintptr_t g_game_base   = 0;

/* -1 = not decided yet (getenv on first use: the constructor order between this file
 * and cod1plus.c does not matter then). */
static int g_force = -1;

int gear_force_enabled(void)
{
    if (g_force < 0) {
        const char* g = getenv("COD1RELOADED_GEAR");
        g_force = !(g && (*g == '1' || *g == 't' || *g == 'T' || *g == 'y' || *g == 'Y'));
    }
    return g_force;
}

/* Lowest start and highest end of the game module's mappings, 0/0 if not mapped. */
static void game_range(uintptr_t* lo, uintptr_t* hi)
{
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    *lo = 0; *hi = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long s, e;
        if (!strstr(line, GAME_SO_NAME)) continue;
        if (sscanf(line, "%lx-%lx", &s, &e) != 2) continue;
        if (!*lo || s < *lo) *lo = (uintptr_t)s;
        if (e > *hi) *hi = (uintptr_t)e;
    }
    fclose(f);
}

static int vmmain_wrapper(int cmd, int a0, int a1, int a2, int a3, int a4, int a5,
                          int a6, int a7, int a8, int a9, int a10, int a11)
{
    if (cmd == GAME_INIT && g_game_base && gear_force_enabled()) {
        trap_cvar_set_t set = (trap_cvar_set_t)(g_game_base + RVA_TRAP_CVAR_SET);
        set("g_useGear", "0");
        printf("%s g_useGear forced to 0 before G_InitGame (no attached gear on player "
               "models; COD1RELOADED_GEAR=1 to leave the cvar alone)\n", TAG);
        fflush(stdout);
    }
    return g_real_vmmain(cmd, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

void* dlsym(void* handle, const char* name)
{
    static void* (*real_dlsym)(void*, const char*) = NULL;
    void* r;

    if (!real_dlsym) {
        real_dlsym = (void* (*)(void*, const char*))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!real_dlsym)
            real_dlsym = (void* (*)(void*, const char*))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
        if (!real_dlsym) return NULL;     /* cannot happen on any glibc we run on */
    }
    r = real_dlsym(handle, name);

    if (r && name && strcmp(name, "vmMain") == 0 && gear_force_enabled()) {
        uintptr_t lo, hi;
        game_range(&lo, &hi);
        if (lo && (uintptr_t)r >= lo && (uintptr_t)r < hi) {
            g_real_vmmain = (vmmain_t)r;
            g_game_base   = lo;
            printf("%s vmMain of %s wrapped (base 0x%08lx): g_useGear will be 0 at every "
                   "G_InitGame\n", TAG, GAME_SO_NAME, (unsigned long)lo);
            fflush(stdout);
            return (void*)vmmain_wrapper;
        }
    }
    return r;
}
