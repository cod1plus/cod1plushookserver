/*
 * use_pickup.c - a weapon lying on the ground gives its ammo on the use key only, never by
 * walking over it.
 *
 * What the engine does (game.mp.i386.so md5 343f99cd..., .dynsym names):
 *   - every frame G_TouchTriggers (0x386cf) calls ent->touch(item, player, 1) for each item
 *     the player stands on (BG_PlayerTouchesItem, 0x388f6 -> 0x389d6);
 *   - the use key, Cmd_Activate_f (0x4562b), calls the SAME pointer with touched = 0 on the
 *     item G_GetActivateEnt picked (0x45827 -> 0x4584c);
 *   - for every item that pointer is Touch_Item_Auto (0x4b520, set by LaunchItem 0x4bef6):
 *     it sets the byte Touch_Item tests (+0x176), then Touch_Item(item, player, touched);
 *   - Touch_Item asks BG_CanItemBeGrabbed(&item->s, ps, touched) (0x1b68f). For a weapon
 *     (item->giType 1): a weapon the player CARRIES -> yes while BG_GetMaxPickupableAmmo > 0,
 *     touched or not; a weapon he does not carry -> no when touched, yes on use. When the
 *     answer is no and touched is set, Touch_Item returns at once (0x4b5d9), no message.
 * So walking over a weapon only ever does one thing: it takes the ammo of a weapon the
 * player already carries, with the pickup event (and its sound) on the player. Picking up
 * or swapping a weapon of another type was already a use-key action.
 *
 * Fix: Touch_Item_Auto returns at once when touched != 0 and the item is a weapon. The use
 * key (touched 0) goes through untouched, with the engine's own rules: the cursor hint, the
 * ammo taken from a weapon of the same type, the swap / empty-slot pickup of another type,
 * the PAM "trigger" notify. Health packs and every other item keep the touch pickup. Cmd_Give_f
 * calls Touch_Item directly, so `give` is unchanged. Same behaviour for every client (1.5 too).
 *
 * CoD2 1.3 (and cod2x, which does not change it) has the same BG_CanItemBeGrabbed rule:
 * this is not a port, it is the touched path of CoD1's own code switched off for weapons.
 *
 * NOTE client side: with cg_predictItems 1 (the default) the client's cgame predicts the
 * pickup on its own (CG_TouchItem, cgame_mp_x86 0x3002a710, same BG_CanItemBeGrabbed(.., 1)):
 * it queues a predicted EV_ITEM_PICKUP (0x92) and hides the item (eFlags 0x100) for the
 * player standing on it, though the server gives him nothing. Only his own client does it;
 * the others get nothing from the server. Not fixed here (cgame, read from the binary only).
 *
 * COD1RELOADED_USE_PICKUP=0 keeps the engine's walk-over ammo pickup.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include "hooks.h"
#include "use_pickup.h"

#define TAG "[use_pickup]"
#define GAME_SO_NAME "game.mp.i386.so"

#define RVA_TOUCH_ITEM_AUTO 0x04b520
#define GE_ITEM             0x2a0      /* gentity_t.item (gitem_t*), LaunchItem 0x4bdd4 */
#define GI_TYPE             0x20       /* gitem_t.giType, BG_CanItemBeGrabbed 0x1b71a */
#define IT_WEAPON           1

/* push ebp; mov ebp,esp; push ebx; sub esp,0x14 - the 7 bytes the detour replaces (the
 * 5-byte jump must not split the sub) */
static const unsigned char TIA_PROLOGUE[7] = { 0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x14 };

typedef void (*touch_t)(void* ent, void* other, int touched);

static hook_t    g_hook;
static touch_t   orig_touch_item_auto;
static uintptr_t g_base;

static void hook_Touch_Item_Auto(void* ent, void* other, int touched)
{
    const char* item = *(const char* const*)((const char*)ent + GE_ITEM);
    if (touched && item && *(const int*)(item + GI_TYPE) == IT_WEAPON)
        return;
    orig_touch_item_auto(ent, other, touched);
}

static uintptr_t find_game_base(void)
{
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    uintptr_t base = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s;
        if (!strstr(line, GAME_SO_NAME)) continue;
        s = (uintptr_t)strtoul(line, NULL, 16);
        if (s && (base == 0 || s < base)) base = s;
    }
    fclose(f);
    return base;
}

static void try_install(void)
{
    static int warned = 0;
    const uintptr_t base = find_game_base();
    unsigned char* target;
    if (!base) return;
    target = (unsigned char*)(base + RVA_TOUCH_ITEM_AUTO);
    /* a map change reloads the module (often at the same address) with the vanilla bytes */
    if (base == g_base && target[0] == 0xE9) return;
    if (memcmp(target, TIA_PROLOGUE, sizeof(TIA_PROLOGUE)) != 0) {
        if (!warned) {
            printf("%s Touch_Item_Auto @0x%08lx is %02x %02x %02x - different game.mp.i386.so build? not installed\n",
                   TAG, (unsigned long)(uintptr_t)target, target[0], target[1], target[2]);
            fflush(stdout);
            warned = 1;
        }
        return;
    }
    if (hook_install(&g_hook, (uintptr_t)target, (uintptr_t)hook_Touch_Item_Auto,
                     (int)sizeof(TIA_PROLOGUE)) == 0) {
        orig_touch_item_auto = (touch_t)g_hook.trampoline;
        g_base = base;
        warned = 0;
        printf("%s installed (game base 0x%08lx): weapons on the ground give ammo on the use key only\n",
               TAG, (unsigned long)base);
    } else {
        printf("%s hook_install failed - walk-over ammo pickup unchanged\n", TAG);
    }
    fflush(stdout);
}

static void* watcher(void* a)
{
    (void)a;
    for (;;) { try_install(); usleep(400 * 1000); }
    return NULL;
}

void use_pickup_init(void)
{
    const char* e = getenv("COD1RELOADED_USE_PICKUP");
    pthread_t tid;
    if (e && (*e == '0' || *e == 'f' || *e == 'F' || *e == 'n' || *e == 'N')) {
        printf("%s off (COD1RELOADED_USE_PICKUP=%s) - walking over a weapon you carry takes its ammo\n", TAG, e);
        fflush(stdout);
        return;
    }
    if (pthread_create(&tid, NULL, watcher, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started\n", TAG);
    } else {
        printf("%s failed to start watcher\n", TAG);
    }
    fflush(stdout);
}
