/* pure_extra.c - see pure_extra.h
 *
 * The HD texture pack is 5.6 GB of optional client-side pk3s (main/zcod*.pk3). On a
 * pure server a client may only read from the paks the server lists, and is dropped
 * (EXE_UNPURECLIENTDETECTED) when it answers with a pak the server does not know:
 *
 *   SV_SpawnServer   sv_paks     = FS_LoadedPakChecksums()   one checksum per loaded pak
 *                    sv_pakNames = FS_LoadedPakNames()       their names, same count
 *                    -> the client reads a file only from a pak whose checksum is in
 *                       sv_paks (CoDMP 0x42adf0); a pak it has and the server does not
 *                       list is ignored, a listed pak it does not have costs nothing
 *                       (only sv_referencedPaks are ever downloaded).
 *   SV_VerifyPaks_f  ("cp", 0x808c159) every pure checksum the client sends must be in
 *                    FS_LoadedPakPureChecksums(), else cl->pureAuthentic = 2 -> dropped.
 *
 * So far the only way to allow the pack was to copy it into every server's main/. But
 * what identifies a pak is not its bytes: FS_LoadZipFile (0x80633c6) checksums the CRC-32
 * of every non-empty file of the central directory,
 *
 *   checksum      = Com_BlockChecksum(crcs)
 *   pure checksum = Com_BlockChecksumKey(crcs, fs_checksumFeed)
 *
 * and pure_extra_data.h (scripts/gen_pure_extra.py) carries exactly those CRC lists, 10 kB
 * for the seven paks. The three list functions are extended with them: the paks are
 * listed as loaded, never as referenced - a client without the pack is asked nothing,
 * a client with it reads its textures from it and is recognised when it says so, and a
 * modified copy has another checksum and stays out like any unknown pak.
 * A pak the server does hold is skipped here (already in the engine's own lists).
 *
 * WHICH paks (the list is a decision, not the whole download):
 *   zcod1..zcod6   world textures, skins, gfx.
 *   zcod9_fix      ours: the pack's 17 textures saved with a DX10 DDS header, re-headered
 *                  as plain DXT5 - the engine reads DXT1/3/5 only (CoDMP 0x50d365) and the
 *                  originals shadow the vanilla .dds, so those decals were simply gone.
 *   NOT zcod7      env/ skies and water cube maps as DXT. The 1.1 cube map loader (CoDMP
 *                  0x50e950) takes whatever format the face file has, flips the faces in
 *                  place as 32-bit pixels and uploads them as RGBA: on DXT data that runs
 *                  8x past the file buffer and the game dies with "Hunk_FreeTempMemory:
 *                  bad magic" (mp_harbor, mp_carentan, mp_neuville, mp_chateau, mp_ship;
 *                  elsewhere the sky is rejected). Not listed = not read on our servers,
 *                  whatever the client build.
 */
#define _GNU_SOURCE
#include "pure_extra.h"
#include "pure_extra_data.h"
#include "hooks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "[pure_extra]"

/* cod_lnxded 1.1 - each of the three has a single caller and the same 6-byte prologue
 * (push ebp / mov ebp,esp / sub esp,0x18), returns its own static char[0x2000]. */
#define VA_FS_LOADED_PAK_CHECKSUMS       0x08075b76u   /* "%i " per pak   */
#define VA_FS_LOADED_PAK_NAMES           0x08075beeu   /* names, ' ' between */
#define VA_FS_LOADED_PAK_PURE_CHECKSUMS  0x08075c7au   /* "%i " per pak   */
#define VA_COM_BLOCKCHECKSUMKEY          0x0807f8adu   /* (buf, len, key) */
#define VA_FS_SEARCHPATHS                0x080e8c30u   /* searchpath_t* : { next, pack, .. } */
#define VA_FS_CHECKSUMFEED               0x0835b234u
#define VA_CMD_ARGC                      0x080600eau
#define VA_CMD_ARGV                      0x080600f4u
#define PACK_CHECKSUM                    0x304
#define LIST_BYTES                       0x2000
#define PATCH_LEN                        6

static const unsigned char PROLOGUE[PATCH_LEN] = { 0x55, 0x89, 0xe5, 0x83, 0xec, 0x18 };

typedef char *(*list_fn_t)(void);
typedef int (*blockchecksumkey_t)(const void *buf, int len, int key);

static hook_t g_h_sums, g_h_names, g_h_pure;
static int    g_announced_feed = 0x7fffffff;

/* the server holds this very pak: the engine lists it by itself */
static int server_has(int checksum) {
    const uint8_t *sp = *(const uint8_t *const *)VA_FS_SEARCHPATHS;
    for (int guard = 0; sp && guard < 4096; ++guard, sp = *(const uint8_t *const *)sp) {
        const uint8_t *pack = *(const uint8_t *const *)(sp + 4);
        if (pack && *(const int *)(pack + PACK_CHECKSUM) == checksum) return 1;
    }
    return 0;
}

static void append(char *list, const char *txt) {
    const size_t have = strlen(list), add = strlen(txt);
    if (have + add + 1 <= LIST_BYTES) memcpy(list + have, txt, add + 1);
}

static char *hook_loaded_pak_checksums(void) {
    char *list = ((list_fn_t)g_h_sums.trampoline)();
    char tok[32];
    for (int i = 0; i < PX_NUM_PAKS; ++i) {
        if (server_has(PX_PAKS[i].checksum)) continue;
        snprintf(tok, sizeof(tok), "%i ", PX_PAKS[i].checksum);
        append(list, tok);
    }
    return list;
}

static char *hook_loaded_pak_names(void) {
    char *list = ((list_fn_t)g_h_names.trampoline)();
    for (int i = 0; i < PX_NUM_PAKS; ++i) {
        if (server_has(PX_PAKS[i].checksum)) continue;
        if (list[0]) append(list, " ");
        append(list, PX_PAKS[i].name);
    }
    return list;
}

static char *hook_loaded_pak_pure_checksums(void) {
    char *list = ((list_fn_t)g_h_pure.trampoline)();
    const int feed = *(const int *)VA_FS_CHECKSUMFEED;
    /* The only caller is SV_VerifyPaks_f, and it tokenizes this list AFTER the call: the
     * command arguments are still the client's "cp" answer. Counting our paks in it costs
     * nothing and tells in the log that a client did load the pack. */
    const int argc = ((int (*)(void))VA_CMD_ARGC)();
    char tok[32];
    int n = 0, held = 0;
    for (int i = 0; i < PX_NUM_PAKS; ++i) {
        if (server_has(PX_PAKS[i].checksum)) continue;
        const int pure = ((blockchecksumkey_t)VA_COM_BLOCKCHECKSUMKEY)(PX_PAKS[i].crc, 4 * PX_PAKS[i].ncrc, feed);
        snprintf(tok, sizeof(tok), "%i ", pure);
        append(list, tok);
        ++n;
        for (int a = 1; a < argc && a < 1024; ++a) {
            const char *arg = ((const char *(*)(int))VA_CMD_ARGV)(a);
            if (arg && arg[0] != '@' && atoi(arg) == pure) { ++held; break; }
        }
    }
    if (held)
        printf("%s a client answered with %d of the %d pak(s) (it has the HD pack): accepted\n", TAG, held, n);
    if (feed != g_announced_feed) {            /* once per map: the feed changes at every spawn */
        g_announced_feed = feed;
        printf("%s %d client-side pak(s) accepted without the files (%s..%s), checksum feed %d\n",
               TAG, n, PX_PAKS[0].name, PX_PAKS[PX_NUM_PAKS - 1].name, feed);
    }
    return list;
}

void pure_extra_init(void) {
    const char *e = getenv("COD1RELOADED_PURE_EXTRA");
    if (e && e[0] == '0') {
        printf("%s disabled (COD1RELOADED_PURE_EXTRA=0)\n", TAG);
        return;
    }
    const uintptr_t va[3] = { VA_FS_LOADED_PAK_CHECKSUMS, VA_FS_LOADED_PAK_NAMES, VA_FS_LOADED_PAK_PURE_CHECKSUMS };
    for (int i = 0; i < 3; ++i)
        if (memcmp((const void *)va[i], PROLOGUE, PATCH_LEN) != 0) {
            printf("%s unexpected code at 0x%08lx (not cod_lnxded 1.1?) - not installed\n", TAG, (unsigned long)va[i]);
            return;
        }
    /* all three or none: a pak listed in sv_paks that the verify list does not know
     * would get every client holding it dropped. The verify list goes first. */
    if (hook_install(&g_h_pure, va[2], (uintptr_t)hook_loaded_pak_pure_checksums, PATCH_LEN) != 0) {
        printf("%s hook_install failed - not installed\n", TAG);
        return;
    }
    if (hook_install(&g_h_sums, va[0], (uintptr_t)hook_loaded_pak_checksums, PATCH_LEN) != 0 ||
        hook_install(&g_h_names, va[1], (uintptr_t)hook_loaded_pak_names, PATCH_LEN) != 0) {
        hook_remove(&g_h_sums);
        hook_remove(&g_h_names);
        hook_remove(&g_h_pure);
        printf("%s hook_install failed - not installed\n", TAG);
        return;
    }
    printf("%s installed: %d pak(s) allowed on pure clients without being on the server\n", TAG, PX_NUM_PAKS);
}
