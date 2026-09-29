/*
 * bullet_prio.c - a limb never shields the body: the bullet goes through the arm, hand or
 * leg to what is behind it, for EVERY weapon.
 *
 * How the engine picks the part a bullet hit (cod_lnxded 0x80d2000, per-bone spheres on
 * the posed skeleton): it walks every bone the ray crosses and keeps the one with the
 * HIGHEST priority, even if it lies behind another one; at equal priority the nearest
 * wins (0x80d2313 / 0x80d2482). The priority comes from a 19-byte table the game passes to
 * trap_LocationalTrace (Bullet_Fire_Extended 0x6e081), one entry per hit location:
 *   0 none 1 helmet 2 head 3 neck 4 torso_up 5 torso_lo 6-9 arms 10-11 hands
 *   12-15 legs 16-17 feet 18 gun
 * and there are two tables in game.mp.i386.so (.dynsym, md5 343f99cd...):
 *   riflePriorityMap  0x82e7f: none 1, helmet/head/neck 9, torso_up 8, torso_lo 7, arms 6,
 *                              hands 5, legs 4, feet 3, gun 0   (bolt rifles, garand...)
 *   bulletPriorityMap 0x82e6c: none 1, everything 3, gun 0     (SMG, pistols, BAR, MG)
 * So a rifle bullet already goes through an arm into the head, but with an automatic or a
 * pistol every part is equal and the NEAREST one - the arm of a player aiming at you -
 * stops the bullet. That is the "I shot him in the arm and it did not go through" report.
 *
 * This rewrites the two tables in place at every module load (the .rodata comes back
 * vanilla with each map change, like the lean constant: pose_sync.c), server side only -
 * the client never decides a hit, so nothing to sync. COD1RELOADED_BULLET_PRIO:
 *   core  (default) both tables = body 4 (helmet, head, neck, torso up/lo), limbs 3.
 *         A limb is always beaten by any body part behind it; between two body parts the
 *         nearest wins, so a bullet entering the belly on its way up is a belly hit, not
 *         a head shot through the torso. Same rule for every weapon. NOTE for rifles: they
 *         used to take the head over a nearer torso; with `core` they take the nearest
 *         body part like everything else.
 *   rifle both tables = the rifle table (head over torso over limbs, for SMGs too).
 *   bullet-only  the bullet table = core, the rifle table left as shipped.
 *   off   nothing patched.
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
#include "bullet_prio.h"

#define TAG "[bullet_prio]"
#define GAME_SO_NAME "game.mp.i386.so"
#define RVA_BULLET_PRIO 0x082e6c
#define RVA_RIFLE_PRIO  0x082e7f
#define PRIO_LEN        19

static const unsigned char VANILLA_BULLET[PRIO_LEN] =
    { 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0 };
static const unsigned char VANILLA_RIFLE[PRIO_LEN] =
    { 1, 9, 9, 9, 8, 7, 6, 6, 6, 6, 5, 5, 4, 4, 4, 4, 3, 3, 0 };
static const unsigned char CORE[PRIO_LEN] =
    { 1, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0 };

static char      g_mode[16] = "core";
static uintptr_t g_base = 0;

static uintptr_t find_game_base(void) {
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

/* write `want` over the table at `va` if it currently holds the vanilla bytes (a
 * different build, or a table already patched to something else, is left alone) */
static int patch_table(uintptr_t va, const unsigned char* vanilla, const unsigned char* want,
                       const char* name) {
    unsigned char* p = (unsigned char*)va;
    if (memcmp(p, want, PRIO_LEN) == 0) return 1;              /* already there */
    if (memcmp(p, vanilla, PRIO_LEN) != 0) {
        printf("%s %s @0x%x is not the vanilla table - not patching\n", TAG, name, (unsigned)va);
        return 0;
    }
    if (hook_unprotect(va, PRIO_LEN) != 0) {
        printf("%s cannot unprotect %s\n", TAG, name);
        return 0;
    }
    memcpy(p, want, PRIO_LEN);
    printf("%s %s: limbs 3, body 4 -> %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
           TAG, name, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11],
           p[12], p[13], p[14], p[15], p[16], p[17], p[18]);
    return 1;
}

static void try_install(void) {
    uintptr_t base = find_game_base();
    const unsigned char* want_bullet;
    const unsigned char* want_rifle;
    if (!base) return;
    if (!strcasecmp(g_mode, "rifle"))            { want_bullet = VANILLA_RIFLE; want_rifle = VANILLA_RIFLE; }
    else if (!strcasecmp(g_mode, "bullet-only")) { want_bullet = CORE;          want_rifle = VANILLA_RIFLE; }
    else                                         { want_bullet = CORE;          want_rifle = CORE; }
    /* decide on the BYTES every tick: the module is reloaded at every map change */
    if (base == g_base &&
        memcmp((void*)(base + RVA_BULLET_PRIO), want_bullet, PRIO_LEN) == 0 &&
        memcmp((void*)(base + RVA_RIFLE_PRIO), want_rifle, PRIO_LEN) == 0)
        return;
    patch_table(base + RVA_BULLET_PRIO, VANILLA_BULLET, want_bullet, "bulletPriorityMap");
    patch_table(base + RVA_RIFLE_PRIO, VANILLA_RIFLE, want_rifle, "riflePriorityMap");
    g_base = base;
    printf("%s mode %s applied (game base 0x%08lx)\n", TAG, g_mode, (unsigned long)base);
    fflush(stdout);
}

static void* watcher(void* a) {
    (void)a;
    for (;;) { try_install(); usleep(400 * 1000); }
    return NULL;
}

void bullet_prio_init(void) {
    const char* e = getenv("COD1RELOADED_BULLET_PRIO");
    pthread_t tid;
    if (e && *e) {
        strncpy(g_mode, e, sizeof(g_mode) - 1);
        g_mode[sizeof(g_mode) - 1] = 0;
    }
    if (!strcasecmp(g_mode, "off") || !strcmp(g_mode, "0")) {
        printf("%s off - vanilla priority tables (a nearer limb stops SMG/pistol bullets)\n", TAG);
        return;
    }
    if (pthread_create(&tid, NULL, watcher, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started (mode %s)\n", TAG, g_mode);
    }
    fflush(stdout);
}
