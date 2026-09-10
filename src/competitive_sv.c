/*
 * competitive_sv.c - server-driven fair-play cvar enforcement (PunkBuster replacement).
 *
 * MODEL
 * -----
 * The admin edits ONE file on the server; every client running the mod aligns to it.
 * No values are hardcoded in the client.
 *
 *   competitive.cfg            (next to cod_lnxded, or $COD1RELOADED_COMPETITIVE_FILE)
 *   -----------------------------------------------------------------
 *   # "<cvar> <value>"      -> locked to exactly <value> (player can't change)
 *   # "<cvar> <min> <max>"  -> clamped to [min,max]      (player picks inside)
 *   com_maxfps    125 250
 *   snaps         40
 *   cl_maxpackets 125
 *   rate          25000
 *
 * The .so parses that into a compact spec string:
 *   "com_maxfps=125:250 snaps=40 cl_maxpackets=125 rate=25000"
 * and publishes it in the cvar "sv_competitive" with CVAR_SYSTEMINFO (0x08). CoD1
 * mirrors SYSTEMINFO cvars into every client (the same channel sv_pure/sv_cheats use -
 * CONFIRMED in CoDMP.exe: sv_cheats registered 0x48 = SYSTEMINFO|ROM, applied in
 * CL_SystemInfoChanged @0x45d6xx via Cvar_Set2). The client mod parses the spec and
 * locks/clamps the cvars. Empty file / absent -> empty spec -> clients stay vanilla.
 *
 * The file is re-read live: edit it and the new limits propagate within ~1s, no restart.
 *
 * THREADING: cvar traps touch the engine cvar table (not thread safe). The watcher
 * thread only reads the FILE and stages the spec; the actual trap calls happen from a
 * hook on G_RunFrame (@0x0505f5), i.e. on the game thread.
 *
 * Env: COD1RELOADED_COMPETITIVE=0 disables. COD1RELOADED_COMPETITIVE_FILE overrides path.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include "hooks.h"

#define TAG "[competitive]"
#define GAME_SO_NAME "game.mp.i386.so"
#define CVAR_SYSTEMINFO 0x08
/* The engine rejects any cvar value longer than MAX_CVAR_VALUE_STRING (256) with a
 * Com_Error that SHUTS THE SERVER DOWN on the next map change - hit live with a
 * 375-char spec. So the spec is split across several SYSTEMINFO cvars
 * (sv_competitive, sv_competitive2, ...), each kept well under the limit and cut
 * only at entry boundaries. The client concatenates them back before parsing. */
#define SPEC_MAX     1024   /* full spec, server side */
#define CHUNK_MAX     220   /* per-cvar payload, safely under 256 */
#define SPEC_CHUNKS     4   /* -> ~880 chars, roughly 45 cvars */

/* game.mp.i386.so RVAs (from .dynsym, md5 343f99cd...) */
#define RVA_G_RUNFRAME         0x0505f5
#define RVA_TRAP_CVAR_REGISTER 0x067a66   /* (vmCvar_t*, name, default, flags) syscall 4 */
#define RVA_TRAP_CVAR_SET      0x067ad7   /* (name, value)                     syscall 6 */

static const unsigned char RF_PROLOGUE[5] = { 0x55, 0x89, 0xe5, 0x56, 0x53 };
static unsigned char g_vmcvar[272];       /* vmCvar_t, confirmed 272 B */

typedef void (*trap_cvar_register_t)(void*, const char*, const char*, int);
typedef void (*trap_cvar_set_t)(const char*, const char*);
typedef void (*g_runframe_t)(int levelTime);

static hook_t       g_rf_hook;
static g_runframe_t orig_runframe = NULL;
static uintptr_t    g_base       = 0;
static int          g_enable     = 1;
static int          g_registered = 0;
/* g_useGear: the stock character scripts attach backpacks / bandoliers / ammo belts only
 * while this server cvar is non-zero (character/_utility.gsc::useOptionalModels()). That gear
 * is attached with ignoreCollision, so bullets never see it: it is a silhouette 8-17 units
 * wider than anything that can be hit, and players aim at it. 1.6X therefore forces the
 * cvar to 0 on every game-module load, from the game thread, so no config on any server
 * can drift back. COD1RELOADED_GEAR=1 leaves the cvar alone. The first map after a server
 * start still PRECACHES the gear (the scripts read the cvar in G_InitGame, before our first
 * G_RunFrame) but no player is ever spawned with it: attach() re-reads the cvar at spawn. */
static int          g_gear_force = 1;
static int          g_gear_done  = 0;

/* staged spec (written by watcher, read+published by the game thread) */
static char         g_spec_pending[SPEC_MAX] = "";
static char         g_spec_live[SPEC_MAX]    = "";
static volatile int g_dirty = 0;

static const char* cfg_path(void) {
    const char* e = getenv("COD1RELOADED_COMPETITIVE_FILE");
    return (e && e[0]) ? e : "competitive.cfg";
}

/* Parse competitive.cfg into `out`. Lines: "cvar value" or "cvar min max". '#'/';'
 * comments and blank lines ignored. Returns the spec (may be empty). */
static void build_spec(char* out, size_t outsz) {
    out[0] = 0;
    FILE* f = fopen(cfg_path(), "r");
    if (!f) return;
    char line[256];
    size_t len = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[64], a[32], b[32];
        int n = sscanf(line, " %63s %31s %31s", name, a, b);
        if (n < 2 || name[0] == '#' || name[0] == ';' || name[0] == '/') continue;
        char tok[160];
        int tn = (n >= 3)
            ? snprintf(tok, sizeof(tok), "%s%s=%s:%s", len ? " " : "", name, a, b)
            : snprintf(tok, sizeof(tok), "%s%s=%s",    len ? " " : "", name, a);
        if (tn <= 0 || len + (size_t)tn >= outsz) break;
        memcpy(out + len, tok, (size_t)tn);
        len += (size_t)tn;
        out[len] = 0;
    }
    fclose(f);
}

/* Name of chunk i: "sv_competitive", "sv_competitive2", ... */
static void chunk_name(int i, char* out, size_t n) {
    if (i == 0) snprintf(out, n, "sv_competitive");
    else        snprintf(out, n, "sv_competitive%d", i + 1);
}

/* Copy at most CHUNK_MAX chars of `src` into `dst`, cutting on a space so an entry
 * is never split in half. Returns how many chars were consumed. */
static size_t chunk_take(const char* src, char* dst, size_t dstsz) {
    size_t len = strlen(src);
    if (len > CHUNK_MAX) {
        len = CHUNK_MAX;
        while (len > 0 && src[len] != ' ') len--;   /* back up to a boundary */
        if (len == 0) len = CHUNK_MAX;              /* single huge entry: hard cut */
    }
    if (len >= dstsz) len = dstsz - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
    while (src[len] == ' ') len++;                  /* swallow the separator */
    return len;
}

/* GAME thread: ensure the cvars exist as SYSTEMINFO, then push the staged spec. */
static void publish_if_dirty(void) {
    if (!g_base) return;

    if (!g_registered) {
        g_registered = 1;
        trap_cvar_register_t reg =
            (trap_cvar_register_t)(g_base + RVA_TRAP_CVAR_REGISTER);
        for (int i = 0; i < SPEC_CHUNKS; i++) {
            char nm[32];
            chunk_name(i, nm, sizeof(nm));
            reg(g_vmcvar, nm, "", CVAR_SYSTEMINFO);   /* create + flag */
        }
        g_dirty = 1;                                  /* force first push */
        printf("%s sv_competitive registered as SYSTEMINFO in %d parts "
               "(broadcast to clients)\n", TAG, SPEC_CHUNKS);
        fflush(stdout);
    }

    if (g_dirty) {
        char spec[SPEC_MAX];
        memcpy(spec, g_spec_pending, sizeof(spec));   /* snapshot the staged spec */
        spec[SPEC_MAX - 1] = 0;
        g_dirty = 0;

        trap_cvar_set_t set = (trap_cvar_set_t)(g_base + RVA_TRAP_CVAR_SET);
        const char* p = spec;
        int used = 0;
        for (int i = 0; i < SPEC_CHUNKS; i++) {
            char nm[32], part[CHUNK_MAX + 1];
            chunk_name(i, nm, sizeof(nm));
            if (*p) {
                p += chunk_take(p, part, sizeof(part));
                used++;
            } else {
                part[0] = 0;                          /* clear the unused tail */
            }
            set(nm, part);
        }
        if (*p) {
            printf("%s WARNING: competitive.cfg is too long, the tail was dropped "
                   "(max ~%d chars): \"%s\"\n", TAG, CHUNK_MAX * SPEC_CHUNKS, p);
        }
        printf("%s published spec to clients in %d part(s): \"%s\"\n",
               TAG, used, spec[0] ? spec : "(empty)");
        fflush(stdout);
    }
}

static void force_gear_off(void) {
    if (!g_gear_force || g_gear_done || !g_base) return;
    g_gear_done = 1;
    trap_cvar_set_t set = (trap_cvar_set_t)(g_base + RVA_TRAP_CVAR_SET);
    set("g_useGear", "0");
    printf("%s g_useGear forced to 0: no attached gear on player models "
           "(COD1RELOADED_GEAR=1 to leave the cvar alone)\n", TAG);
    fflush(stdout);
}

static void hook_G_RunFrame(int levelTime) {
    force_gear_off();
    publish_if_dirty();
    orig_runframe(levelTime);
}

static uintptr_t find_game_base(void) {
    FILE* f = fopen("/proc/self/maps", "r");
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

static void try_install(void) {
    static int logged = 0;
    uintptr_t base = find_game_base();
    if (!base) return;

    unsigned char* rf = (unsigned char*)(base + RVA_G_RUNFRAME);
    if (base == g_base && rf[0] == 0xE9) return;

    if (memcmp(rf, RF_PROLOGUE, 5) != 0) {
        if (rf[0] != 0xE9 && !logged) {
            logged = 1;
            printf("%s G_RunFrame prologue mismatch (%02x %02x %02x) - different "
                   "game.mp.i386.so build? not installed\n", TAG, rf[0], rf[1], rf[2]);
            fflush(stdout);
        }
        return;
    }

    memset(g_vmcvar, 0, sizeof(g_vmcvar));
    g_registered = 0;                 /* re-register after a map/module reload */
    g_gear_done  = 0;                 /* the reloaded module has the archived value again */
    g_dirty = 1;

    if (hook_install(&g_rf_hook, (uintptr_t)rf, (uintptr_t)hook_G_RunFrame, 5) == 0) {
        orig_runframe = (g_runframe_t)g_rf_hook.trampoline;
        g_base = base;
        logged = 0;
        printf("%s installed (game base 0x%08lx, file '%s')\n",
               TAG, (unsigned long)base, cfg_path());
        fflush(stdout);
    } else {
        printf("%s hook_install failed\n", TAG);
        fflush(stdout);
    }
}

static void* watcher(void* a) {
    (void)a;
    for (;;) {
        try_install();
        if (g_enable) {
            char spec[SPEC_MAX];
            build_spec(spec, sizeof(spec));            /* read the file */
            if (strcmp(spec, g_spec_live) != 0) {      /* changed -> stage it */
                memcpy(g_spec_live, spec, sizeof(g_spec_live));
                memcpy(g_spec_pending, spec, sizeof(g_spec_pending));
                g_dirty = 1;                           /* game thread will push it */
            }
        }
        usleep(1000 * 1000);                           /* re-check the file every 1s */
    }
    return NULL;
}

void competitive_sv_init(void) {
    {
        const char* g = getenv("COD1RELOADED_GEAR");
        if (g && (*g == '1' || *g == 't' || *g == 'T' || *g == 'y' || *g == 'Y')) {
            g_gear_force = 0;
            printf("%s g_useGear left to the server config (COD1RELOADED_GEAR=%s)\n", TAG, g);
        }
    }
    const char* e = getenv("COD1RELOADED_COMPETITIVE");
    if (e && (*e == '0' || *e == 'f' || *e == 'F' || *e == 'n' || *e == 'N')) {
        g_enable = 0;
        printf("%s disabled (COD1RELOADED_COMPETITIVE=%s)\n", TAG, e);
        return;
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, watcher, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started (file '%s' -> clients)\n", TAG, cfg_path());
    } else {
        printf("%s failed to start watcher\n", TAG);
    }
}
