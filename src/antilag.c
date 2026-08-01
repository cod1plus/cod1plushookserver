/*
 * antilag.c - server-side lag compensation (LINUX port) for CoD1 1.5.
 *
 * CoD1 has NO native lag compensation: "antilag" appears nowhere in cod_lnxded
 * nor in game.mp.i386.so (verified: zero occurrences). PAM's
 * `ruleCvarDefault(arr, "g_antilag", 0)` is a CoD2 leftover that sets a cvar
 * nothing reads. THIS module implements the feature and reads that same cvar, so
 * `rcon g_antilag 1` toggles it live.
 *
 * Principle (Unlagged / CoD2 / CoD4x): the shooter's usercmd carries the
 * serverTime his SCREEN showed when he pulled the trigger. That value already
 * encodes RTT + client interpolation, so we never subtract ping by hand. At fire
 * time we move every OTHER live player back to where they were at that instant,
 * run the real bullet trace, then put them back.
 *
 * Target: game.mp.i386.so md5 343f99cd67b79ac74aeaa5261f63c011 (CoD1 v1.5 Linux).
 * That module is NOT stripped, so every address below came straight from its
 * .dynsym (exact, not pattern-matched). A prologue check still turns a mismatched
 * build into a no-op instead of a crash (same fail-safe as lean_hitbox).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include "hooks.h"

#define TAG "[antilag]"

#define GAME_SO_NAME "game.mp.i386.so"

/* ---- game.mp.i386.so RVAs (ALL from .dynsym of md5 343f99cd...) ---- */
#define RVA_BULLET_FIRE       0x06dffc  /* Bullet_Fire_Extended: 8 args, arg1=shooter, arg6=depth */
#define RVA_CLIENTENDFRAME    0x03ab2a  /* ClientEndFrame(gentity*): once per client per frame */
#define RVA_TRAP_LINKENTITY   0x068313
#define RVA_TRAP_UNLINKENTITY 0x068341
#define RVA_TRAP_CVAR_INT     0x067b0c  /* trap_Cvar_VariableIntegerValue(name) */
#define RVA_g_entities        0x21d6c0
#define RVA_level             0x18f220
#define LEVEL_TIME_OFF        0x1e8     /* level.time (int ms), written by G_RunFrame */

/* 5-byte stealable prologues (clean instruction boundaries) */
static const unsigned char BF_PROLOGUE[5] = { 0x55, 0x89, 0xe5, 0x56, 0x53 };
static const unsigned char CE_PROLOGUE[5] = { 0x55, 0x89, 0xe5, 0x57, 0x56 };

/* ---- gentity_t / gclient_t offsets (shared with lean_hitbox / perbone_hit) ---- */
#define GENTITY_SIZE   0x31c
#define E_ORIGIN       0x138   /* r.currentOrigin (vec3): what we save/rewind/restore */
#define E_CLIENT       0x15c   /* gentity->client (gclient_t*) */
#define E_HEALTH       0x238   /* int, alive gate */
#define PS_COMMANDTIME 0x00    /* ps.commandTime = the usercmd serverTime (Q3 layout) */

#define AL_MAX_CLIENTS 64
#define AL_HIST        128     /* sized by TIME: 128 * 25ms @40fps = 3.2s of margin */

typedef struct { float origin[3]; int time; } al_snap_t;

static al_snap_t g_hist[AL_MAX_CLIENTS][AL_HIST];
static int       g_head[AL_MAX_CLIENTS];
static int       g_count[AL_MAX_CLIENTS];

/* rewind bookkeeping for the shot in flight */
static float g_saved[AL_MAX_CLIENTS][3];
static int   g_moved[AL_MAX_CLIENTS];
static int   g_moved_count = 0;

typedef void (*trap_ent_t)(void* ent);
typedef int  (*trap_cvar_int_t)(const char* name);
typedef int  (*bullet_fire_t)(void*, int, int, int, int, int, void*, int);
typedef void (*clientendframe_t)(void* ent);

static hook_t           g_bf_hook, g_ce_hook;
static bullet_fire_t    orig_bullet_fire    = NULL;
static clientendframe_t orig_clientendframe = NULL;
static trap_ent_t       p_LinkEntity   = NULL;
static trap_ent_t       p_UnlinkEntity = NULL;
static trap_cvar_int_t  p_CvarInt      = NULL;
static uintptr_t        g_base = 0;

static int  g_force   = -1;  /* env override of the cvar: -1 = follow the cvar */
static int  g_in_fire = 0;   /* reentrancy guard (Bullet_Fire_Extended recurses) */
static long g_shots = 0, g_heals = 0;
static int  g_log_left = 20;

static void restore_others(void);
extern int g_bullet_in_flight;   /* defined below, read by perbone_hit.c */

static int   level_time(void) { return *(int*)(g_base + RVA_level + LEVEL_TIME_OFF); }
static char* ent_at(int i)    { return (char*)(g_base + RVA_g_entities) + (size_t)i * GENTITY_SIZE; }

/* live toggle: PAM already sets g_antilag, so `rcon g_antilag 1` drives us */
static int antilag_on(void) {
    if (g_force >= 0) return g_force;
    return (p_CvarInt && p_CvarInt("g_antilag") != 0);
}

/* server frame length; a shot inside the current frame needs no rewind */
static int frame_ms(void) {
    int fps = p_CvarInt ? p_CvarInt("sv_fps") : 20;
    if (fps < 1 || fps > 1000) fps = 20;
    return 1000 / fps;
}

/* ---- history ring ------------------------------------------------------ */
static void hist_push(int cn, const float* org, int t) {
    int h = (g_head[cn] + 1) % AL_HIST;
    g_hist[cn][h].origin[0] = org[0];
    g_hist[cn][h].origin[1] = org[1];
    g_hist[cn][h].origin[2] = org[2];
    g_hist[cn][h].time = t;
    g_head[cn] = h;
    if (g_count[cn] < AL_HIST) g_count[cn]++;
}

/* Position of client cn at `want` (ms), lerped between the two bracketing
 * snapshots. Returns 0 when we have nothing usable. */
static int hist_lookup(int cn, int want, float out[3]) {
    const int n = g_count[cn];
    if (n < 2) return 0;

    const int newest = g_head[cn];
    for (int k = 0; k < n - 1; ++k) {
        const int j  = (newest - k     + AL_HIST) % AL_HIST;   /* newer */
        const int jp = (newest - k - 1 + AL_HIST) % AL_HIST;   /* older */
        const al_snap_t* a = &g_hist[cn][jp];
        const al_snap_t* b = &g_hist[cn][j];
        if (a->time <= want && want <= b->time) {
            const int span = b->time - a->time;
            float f = (span > 0) ? (float)(want - a->time) / (float)span : 0.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            for (int c = 0; c < 3; ++c)
                out[c] = a->origin[c] + f * (b->origin[c] - a->origin[c]);
            return 1;
        }
    }
    /* older than everything kept: clamp to the oldest snapshot */
    {
        const int oldest = (newest - (n - 1) + AL_HIST) % AL_HIST;
        if (want < g_hist[cn][oldest].time) {
            out[0] = g_hist[cn][oldest].origin[0];
            out[1] = g_hist[cn][oldest].origin[1];
            out[2] = g_hist[cn][oldest].origin[2];
            return 1;
        }
    }
    return 0;   /* newer than newest: nothing to rewind to */
}

/* ---- capture: ClientEndFrame runs per client per frame, positions final ---- */
static void hook_ClientEndFrame(void* ent) {
    orig_clientendframe(ent);
    if (!g_base) return;

    /* SELF-HEAL: if a restore was ever skipped, undo it NOW, BEFORE capturing.
     * Otherwise the ring would record the rewound position and the corruption
     * would feed itself (real bug hit by the June Windows build: hitbox stuck). */
    if (!g_in_fire && g_moved_count > 0) {
        restore_others();
        g_heals++;
        printf("%s SELF-HEAL: forced restore (shots=%ld heals=%ld)\n", TAG, g_shots, g_heals);
        fflush(stdout);
    }

    /* Same self-heal for the bullet counter: a longjmp out of the fire chain
     * (G_Error deep in the damage code) would skip the decrement forever, and
     * perbone would then treat every sight probe as a bullet - the GRANT-storm
     * class. ClientEndFrame always runs outside any fire. */
    if (!g_in_fire && g_bullet_in_flight != 0) {
        printf("%s SELF-HEAL: bullet counter stuck at %d, reset\n", TAG, g_bullet_in_flight);
        fflush(stdout);
        g_bullet_in_flight = 0;
    }

    const size_t delta = (size_t)((char*)ent - (char*)(g_base + RVA_g_entities));
    if (delta % GENTITY_SIZE) return;
    const int cn = (int)(delta / GENTITY_SIZE);
    if (cn < 0 || cn >= AL_MAX_CLIENTS) return;
    if (!*(void**)((char*)ent + E_CLIENT)) return;          /* players only */
    hist_push(cn, (float*)((char*)ent + E_ORIGIN), level_time());
}

/* ---- rewind / restore -------------------------------------------------- */
static void rewind_others(int shooter_cn, int want) {
    for (int c = 0; c < AL_MAX_CLIENTS; ++c) {
        g_moved[c] = 0;
        if (c == shooter_cn) continue;                      /* never rewind the shooter */
        char* e = ent_at(c);
        if (!*(void**)(e + E_CLIENT))   continue;           /* not a player */
        if (*(int*)(e + E_HEALTH) <= 0) continue;           /* dead */
        float pos[3];
        if (!hist_lookup(c, want, pos)) continue;
        float* org = (float*)(e + E_ORIGIN);
        g_saved[c][0] = org[0]; g_saved[c][1] = org[1]; g_saved[c][2] = org[2];
        p_UnlinkEntity(e);
        org[0] = pos[0]; org[1] = pos[1]; org[2] = pos[2];
        p_LinkEntity(e);                                    /* refile the collision box */
        g_moved[c] = 1;
        g_moved_count++;
    }
}

static void restore_others(void) {
    for (int c = 0; c < AL_MAX_CLIENTS; ++c) {
        if (!g_moved[c]) continue;
        char*  e   = ent_at(c);
        float* org = (float*)(e + E_ORIGIN);
        p_UnlinkEntity(e);
        org[0] = g_saved[c][0]; org[1] = g_saved[c][1]; org[2] = g_saved[c][2];
        p_LinkEntity(e);
        g_moved[c] = 0;
    }
    g_moved_count = 0;
}

/* ---- the fire hook: wrap the bullet trace ------------------------------ */

/* Non-zero exactly while a real bullet (Bullet_Fire_Extended, penetration
 * included) is being traced. perbone_hit reads this to act ONLY on bullets:
 * the game also calls trap_LocationalTrace every frame for sight/crosshair
 * probes, and rewriting those flooded hits onto leaning players ("GRANT" storm,
 * hundreds/s with nobody firing). Counter, not flag: the fire fn recurses. */
int g_bullet_in_flight = 0;

static int hook_bullet_fire_inner(void* shooter, int a2, int a3, int a4, int a5,
                                  int depth, void* a7, int a8);

static int hook_bullet_fire(void* shooter, int a2, int a3, int a4, int a5,
                            int depth, void* a7, int a8) {
    g_bullet_in_flight++;
    int r = hook_bullet_fire_inner(shooter, a2, a3, a4, a5, depth, a7, a8);
    g_bullet_in_flight--;
    return r;
}

static int hook_bullet_fire_inner(void* shooter, int a2, int a3, int a4, int a5,
                                  int depth, void* a7, int a8) {
    /* depth > 0 = penetration recursion: the world is already rewound */
    if (g_in_fire || depth != 0 || !antilag_on())
        return orig_bullet_fire(shooter, a2, a3, a4, a5, depth, a7, a8);

    const size_t delta = (size_t)((char*)shooter - (char*)(g_base + RVA_g_entities));
    if (delta % GENTITY_SIZE || delta / GENTITY_SIZE >= AL_MAX_CLIENTS)
        return orig_bullet_fire(shooter, a2, a3, a4, a5, depth, a7, a8);
    const int cn = (int)(delta / GENTITY_SIZE);

    void* cl = *(void**)((char*)shooter + E_CLIENT);
    if (!cl)
        return orig_bullet_fire(shooter, a2, a3, a4, a5, depth, a7, a8);

    const int now  = level_time();
    int       want = *(int*)((char*)cl + PS_COMMANDTIME);   /* shooter's usercmd time */

    /* no-rewind-if-recent: the shot already happened in the current frame */
    if (now - want <= frame_ms())
        return orig_bullet_fire(shooter, a2, a3, a4, a5, depth, a7, a8);
    /* hard clamp: never rewind more than 1s, never into the future */
    if (want < now - 1000) want = now - 1000;
    if (want > now)        want = now;

    g_in_fire = 1;
    rewind_others(cn, want);
    const int r = orig_bullet_fire(shooter, a2, a3, a4, a5, depth, a7, a8);
    restore_others();
    g_in_fire = 0;

    g_shots++;
    if (g_log_left > 0) {
        g_log_left--;
        printf("%s shot cn=%d rewind %dms (level=%d cmd=%d)\n",
               TAG, cn, now - want, now, want);
        fflush(stdout);
    }
    return r;
}

/* ---- install ----------------------------------------------------------- */
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

    unsigned char* bf = (unsigned char*)(base + RVA_BULLET_FIRE);
    unsigned char* ce = (unsigned char*)(base + RVA_CLIENTENDFRAME);

    if (base == g_base && bf[0] == 0xE9) return;             /* already live */

    if (memcmp(bf, BF_PROLOGUE, 5) != 0 || memcmp(ce, CE_PROLOGUE, 5) != 0) {
        if (bf[0] != 0xE9 && !logged) {
            logged = 1;
            printf("%s prologue mismatch (bf %02x %02x %02x / ce %02x %02x %02x) "
                   "- different game.mp.i386.so build? antilag disabled\n",
                   TAG, bf[0], bf[1], bf[2], ce[0], ce[1], ce[2]);
            fflush(stdout);
        }
        return;
    }

    p_LinkEntity   = (trap_ent_t)(base + RVA_TRAP_LINKENTITY);
    p_UnlinkEntity = (trap_ent_t)(base + RVA_TRAP_UNLINKENTITY);
    p_CvarInt      = (trap_cvar_int_t)(base + RVA_TRAP_CVAR_INT);

    memset(g_hist,  0, sizeof(g_hist));
    memset(g_head,  0, sizeof(g_head));
    memset(g_count, 0, sizeof(g_count));
    memset(g_moved, 0, sizeof(g_moved));
    g_moved_count = 0;

    int ok = 1;
    ok &= (hook_install(&g_ce_hook, (uintptr_t)ce, (uintptr_t)hook_ClientEndFrame, 5) == 0);
    if (ok) orig_clientendframe = (clientendframe_t)g_ce_hook.trampoline;
    ok &= (hook_install(&g_bf_hook, (uintptr_t)bf, (uintptr_t)hook_bullet_fire, 5) == 0);
    if (ok) orig_bullet_fire = (bullet_fire_t)g_bf_hook.trampoline;

    if (ok) {
        g_base = base;
        logged = 0;
        printf("%s installed (game base 0x%08lx) - set `g_antilag 1` to compensate%s\n",
               TAG, (unsigned long)base, g_force >= 0 ? " (env FORCE overrides the cvar)" : "");
        fflush(stdout);
    } else {
        printf("%s hook_install failed - antilag disabled\n", TAG);
        fflush(stdout);
    }
}

static void* watcher(void* a) {
    (void)a;
    for (;;) { try_install(); usleep(400 * 1000); }
    return NULL;
}

void antilag_init(void) {
    /* Hooks are installed BY DEFAULT (like lean_hitbox) so that `g_antilag 1` in
     * the console/rcon is enough to switch compensation on, with no restart and
     * no env var. While g_antilag is 0 the fire hook is a straight passthrough;
     * the history ring keeps filling, so enabling mid-match works instantly.
     * COD1RELOADED_ANTILAG=0 is the kill switch (do not install at all). */
    const char* e = getenv("COD1RELOADED_ANTILAG");
    if (e && (*e == '0' || *e == 'f' || *e == 'F' || *e == 'n' || *e == 'N')) {
        printf("%s disabled (COD1RELOADED_ANTILAG=%s)\n", TAG, e);
        return;
    }
    /* FORCE=1 ignores the cvar and always compensates; FORCE=0 never does */
    const char* f = getenv("COD1RELOADED_ANTILAG_FORCE");
    if (f && *f) g_force = atoi(f) ? 1 : 0;

    pthread_t tid;
    if (pthread_create(&tid, NULL, watcher, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started\n", TAG);
    } else {
        printf("%s failed to start watcher\n", TAG);
    }
}
