/*
 * anim_clamp.c - stop "Player animation index out of range" drops/kicks.
 *
 * CoD1's BG_GetAnimationForIndex(index) Com_Error's when `index >= animCount`
 * (a RUNTIME count, ~251, not a hard cap). On a heavily-modded server the
 * client and server can register a slightly different number of player
 * animations (custom map packs / model overrides), so the server references an
 * index the client (or itself) never registered -> the offending player is
 * dropped to the main menu. On a competitive platform this must NEVER happen.
 *
 * Fix: turn the out-of-range Com_Error into a CLAMP — set the index to 0 and
 * fall through, so BG_GetAnimationForIndex returns animation[0] (a valid default
 * pose) instead of erroring. Worst case = one entity shows a default anim for a
 * frame; nobody gets kicked. Mirror patch exists on the client (cgame).
 *
 * Target: game.mp.i386.so md5 343f99cd... (CoD1 v1.5 Linux, from .dynsym).
 *   BG_GetAnimationForIndex @ RVA 0x189f6. The bounds check:
 *     0x18a11  cmp eax,[edx+0xb800]      ; index vs animCount
 *     0x18a17  jb  0x18a2f               ; in-range -> OK
 *     0x18a19  lea eax,[ebx-0x151b8] ...
 *     0x18a2a  call Com_Error            ; <-- error path (22 bytes, 0x18a19..0x18a2f)
 *     0x18a2f  mov eax,[ebp+0xc]; imul 0x5c; add table  ; OK path re-reads [ebp+0xc]
 *   Patch @0x18a19: replace the 22-byte error block with
 *     mov dword [ebp+0xc], 0   (C7 45 0C 00 00 00 00, 7 bytes)  +  15x NOP
 *   so the OK path at 0x18a2f reads index=0. Verify-then-write; wrong build = no-op.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include "hooks.h"

#define TAG "[anim_clamp]"
#define GAME_SO_NAME "game.mp.i386.so"

#define RVA_ANIMCHECK  0x18a19   /* start of the out-of-range error block */
#define PATCH_LEN      22        /* 0x18a2f - 0x18a19 */

/* expected original bytes at RVA_ANIMCHECK (lea/mov/mov/call) — guards a wrong build */
static const unsigned char ORIG[PATCH_LEN] = {
    0x8d, 0x83, 0x48, 0xae, 0xfe, 0xff,       /* lea  eax,[ebx-0x151b8] */
    0x89, 0x44, 0x24, 0x04,                   /* mov  [esp+4],eax       */
    0xc7, 0x04, 0x24, 0x01, 0x00, 0x00, 0x00, /* mov  [esp],1           */
    0xe8, 0xc1, 0xc9, 0xff, 0xff              /* call Com_Error         */
};
/* replacement: mov dword [ebp+0xc],0  +  NOP padding = clamp index to 0 */
static const unsigned char PATCH[PATCH_LEN] = {
    0xc7, 0x45, 0x0c, 0x00, 0x00, 0x00, 0x00, /* mov dword [ebp+0xc],0  */
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, /* nop x15                */
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
};

static uintptr_t g_patched_base = 0;
static int       g_enable       = 1;

static uintptr_t find_game_base(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, GAME_SO_NAME)) continue;
        uintptr_t s = (uintptr_t)strtoul(line, NULL, 16);
        if (s && (base == 0 || s < base)) base = s;
    }
    fclose(f);
    return base;
}

static int poke(unsigned char *dst, const unsigned char *src, size_t n) {
    if (hook_unprotect((uintptr_t)dst, (int)n) != 0) return -1;  /* RWX the page(s) */
    memcpy(dst, src, n);
    return 0;
}

static void try_patch(void) {
    static int logged_bad = 0;
    uintptr_t base = find_game_base();
    if (!base) return;
    if (base == g_patched_base) return;                 /* already done this load */

    unsigned char *site = (unsigned char *)(base + RVA_ANIMCHECK);

    if (memcmp(site, PATCH, PATCH_LEN) == 0) {           /* already patched */
        g_patched_base = base;
        return;
    }
    if (memcmp(site, ORIG, PATCH_LEN) != 0) {            /* different build */
        if (!logged_bad) {
            logged_bad = 1;
            printf("%s site @0x%08lx unexpected (%02x %02x %02x ...) - different "
                   "game.mp.i386.so build? not patched\n",
                   TAG, (unsigned long)(uintptr_t)site, site[0], site[1], site[2]);
            fflush(stdout);
        }
        return;
    }
    if (poke(site, PATCH, PATCH_LEN) == 0) {
        g_patched_base = base;
        logged_bad = 0;
        printf("%s BG_GetAnimationForIndex out-of-range now clamps to 0 "
               "(no more animation-index drops) [game base 0x%08lx]\n",
               TAG, (unsigned long)base);
        fflush(stdout);
    } else {
        printf("%s mprotect/poke failed @0x%08lx\n",
               TAG, (unsigned long)(uintptr_t)site);
        fflush(stdout);
    }
}

static void *watcher(void *a) {
    (void)a;
    for (;;) { try_patch(); usleep(400 * 1000); }        /* re-patch after map reload */
    return NULL;
}

void anim_clamp_init(void) {
    const char *e = getenv("COD1RELOADED_ANIM_CLAMP");
    if (e && (*e == '0' || *e == 'f' || *e == 'F' || *e == 'n' || *e == 'N')) {
        g_enable = 0;
        printf("%s disabled (COD1RELOADED_ANIM_CLAMP=%s)\n", TAG, e);
        return;
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, watcher, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started (clamp animation index out-of-range)\n", TAG);
    } else {
        printf("%s failed to start watcher\n", TAG);
    }
}
