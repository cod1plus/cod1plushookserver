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
#include <math.h>
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

/* ---- shot audit (COD1RELOADED_SHOT_LOG=1): one line per bullet, misses included ----
 * PAM's games_mp.log only records HITS, so "it does not hit when I am clearly on him"
 * leaves no trace at all. This runs the game's own bullet trace (same function, same
 * arguments as Bullet_Fire_Extended 0x6e081..0x6e0b0: trap_LocationalTrace, mask
 * 0x2802031, the rifle or bullet priority map by *(weaponInfo+0x3c)+0x2c0) and, for every
 * other live player near the ray, prints where the ray passed relative to him:
 *   now  = his position when the server tests the bullet,
 *   @cmd = his position when the shooter's screen showed him (usercmd serverTime,
 *          interpolated from the same ring the antilag rewind uses).
 * A miss with a small "now" offset inside the body = a hit-volume / pose problem; a miss
 * with a small "@cmd" offset but a large "now" one = latency (the target moved between
 * what the shooter saw and what the server tests). Diagnostic only, ~1 extra trace/bullet. */
#define RVA_TRAP_LOC_TRACE  0x06817d
#define RVA_BULLET_PRIO     0x082e6c   /* .dynsym bulletPriorityMap */
#define RVA_RIFLE_PRIO      0x082e7f   /* .dynsym riflePriorityMap  */
#define E_ETYPE             0x04
#define E_EFLAGS            0x08
#define E_LEANF             0xd4
#define E_MINS              0x104
#define E_MAXS              0x110
#define SHOT_MASK           0x2802031
#define SHOT_NEAR_U         40.0f      /* only players whose axis is within this of the ray */

typedef void (*loctrace_t)(void* results, const float* start, const float* end,
                           int passEnt, int mask, const unsigned char* prio);
static int  g_shot_log = -1;
static long g_shot_n = 0;

static int shot_log_on(void) {
    if (g_shot_log < 0) {
        const char* e = getenv("COD1RELOADED_SHOT_LOG");
        g_shot_log = (e && e[0] == '1') ? 1 : 0;
        if (g_shot_log) { printf("%s SHOT AUDIT ON - one line per bullet (diagnostic)\n", TAG); fflush(stdout); }
    }
    return g_shot_log;
}

/* COD1RELOADED_HIT_DEBUG=1: tell the SHOOTER, in the bottom-left game-message area (the
 * one GSC iprintln writes to: server command `f "text"`, Scr_MakeGameMessage 0x40208),
 * which part each of his bullets hit, and which nearer limb it went through when the
 * priority table (bullet_prio.c) resolved it that way. trap_SendServerCommand(clientNum,
 * 0, text), game.mp.i386.so .dynsym 0x67c61 (ClientInactivityTimer 0x38dd6..0x38dfb). */
#define RVA_TRAP_SEND_SERVER_CMD 0x067c61
typedef void (*sendcmd_t)(int clientNum, int type, const char* text);
static int g_hit_debug = -1;
static const unsigned char NEAREST_PRIO[19] = { 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0 };

static int hit_debug_on(void) {
    if (g_hit_debug < 0) {
        const char* e = getenv("COD1RELOADED_HIT_DEBUG");
        g_hit_debug = (e && e[0] == '1') ? 1 : 0;
        if (g_hit_debug) { printf("%s HIT DEBUG ON - the shooter sees the part hit and the limb gone through\n", TAG); fflush(stdout); }
    }
    return g_hit_debug;
}

static const char* loc_name(int loc) {
    static const char* const n[19] = { "none", "helmet", "head", "neck", "torso_up", "torso_lo",
        "r_arm_up", "l_arm_up", "r_arm_lo", "l_arm_lo", "r_hand", "l_hand", "r_leg_up",
        "l_leg_up", "r_leg_lo", "l_leg_lo", "r_foot", "l_foot", "gun" };
    return (loc >= 0 && loc < 19) ? n[loc] : "?";
}

/* where the ray passes relative to a player standing at `org` facing `yaw`: in HIS frame
 * (x = to his right, y = in front of him, h = above his feet), at the ray's closest
 * approach to his vertical axis, plus the distance along the ray. Same axes as the
 * hitmap_diag front view (columns = his right), so a shot can be read off that map. */
static void ray_vs_axis(const float* s, const float* dir, float len, const float* org,
                        float yaw, float* x, float* y, float* h, float* along) {
    const float px = org[0] - s[0], py = org[1] - s[1];
    const float dd = dir[0] * dir[0] + dir[1] * dir[1];
    const float yr = yaw * 0.01745329252f;
    float t = (dd > 1e-6f) ? (px * dir[0] + py * dir[1]) / dd : 0.0f;
    if (t < 0) t = 0;
    if (t > len) t = len;
    {
        const float qx = s[0] + dir[0] * t - org[0], qy = s[1] + dir[1] * t - org[1];
        *x = qx * sinf(yr) - qy * cosf(yr);        /* right  = (sin yaw, -cos yaw) */
        *y = qx * cosf(yr) + qy * sinf(yr);        /* forward = (cos yaw, sin yaw) */
    }
    *h = s[2] + dir[2] * t - org[2];
    *along = t;
}

/* four extra traces 4 u around a missed bullet (right / left / up / down of the ray):
 * which of them hit that player says whether the miss was a 4 u edge case */
static void edge_probes(loctrace_t tr, const float* s, const float* dir, float len, int passEnt,
                        const unsigned char* prio, int target, char* out) {
    const float upw[3] = { 0, 0, 1 };
    float u[3], v[3], l;
    int k;
    static const char* const nm[4] = { "R", "L", "U", "D" };
    u[0] = dir[1] * upw[2] - dir[2] * upw[1];
    u[1] = dir[2] * upw[0] - dir[0] * upw[2];
    u[2] = dir[0] * upw[1] - dir[1] * upw[0];
    l = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (l < 1e-4f) { out[0] = 0; return; }
    u[0] /= l; u[1] /= l; u[2] /= l;
    v[0] = u[1] * dir[2] - u[2] * dir[1];
    v[1] = u[2] * dir[0] - u[0] * dir[2];
    v[2] = u[0] * dir[1] - u[1] * dir[0];
    out[0] = 0;
    for (k = 0; k < 4; ++k) {
        const float* a = (k < 2) ? u : v;
        const float sg = (k & 1) ? -4.0f : 4.0f;
        float st[3], en[3];
        unsigned char t[0x30];
        int i;
        for (i = 0; i < 3; ++i) { st[i] = s[i] + a[i] * sg; en[i] = st[i] + dir[i] * len; }
        memset(t, 0, sizeof(t));
        tr(t, st, en, passEnt, SHOT_MASK, prio);
        if (*(unsigned short*)(t + 0x28) == target) {
            strcat(out, out[0] ? "+" : "");
            strcat(out, nm[k]);
            strcat(out, ":");
            strcat(out, loc_name(*(unsigned short*)(t + 0x2c)));
        }
    }
    if (!out[0]) strcpy(out, "none");
}

static void shot_audit(void* shooter, int cn, const float* start, const float* end,
                       void* a7, int now, int want) {
    unsigned char t[0x30];
    float dir[3], len, tmp;
    int i, hit_ent, hit_loc, n;
    char line[900];
    const char* winfo = a7 ? *(const char**)((const char*)a7 + 0x3c) : NULL;
    const int rifle = winfo && *(const int*)(winfo + 0x2c0) != 0;
    const unsigned char* prio = (const unsigned char*)(g_base + (rifle ? RVA_RIFLE_PRIO : RVA_BULLET_PRIO));
    loctrace_t tr = (loctrace_t)(g_base + RVA_TRAP_LOC_TRACE);

    if (!start || !end) return;
    memset(t, 0, sizeof(t));
    tr(t, start, end, *(int*)shooter, SHOT_MASK, prio);
    hit_ent = *(unsigned short*)(t + 0x28);
    hit_loc = *(unsigned short*)(t + 0x2c);

    dir[0] = end[0] - start[0]; dir[1] = end[1] - start[1]; dir[2] = end[2] - start[2];
    len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1.0f) return;
    tmp = 1.0f / len; dir[0] *= tmp; dir[1] *= tmp; dir[2] *= tmp;

    /* hip or ADS: the shooter's EF_AIM (0x200). A hip-fired bolt rifle has a 4-degree cone
     * (kar98k hipSpreadStandMin), 6 u off the aim point at 85 u - not a hitbox question. */
    n = snprintf(line, sizeof(line), "%s shot#%ld cn=%d t=%d lag=%dms %s %s -> ", TAG, ++g_shot_n, cn,
                 now, now - want, rifle ? "rifle" : "bullet",
                 (*(const int*)((const char*)shooter + 0x8) & 0x200) ? "ads" : "hip");
    if (hit_ent < 64) {
        n += snprintf(line + n, sizeof(line) - n, "HIT cn=%d %s at %.0fu", hit_ent, loc_name(hit_loc),
                      *(float*)t * len);
    } else if (hit_ent == 1022) {
        n += snprintf(line + n, sizeof(line) - n, "world at %.0fu", *(float*)t * len);
    } else {
        n += snprintf(line + n, sizeof(line) - n, "nothing (ent %d frac %.2f)", hit_ent, *(float*)t);
    }

    /* hit debug: the same bullet against the NEAREST-part table says which limb, if any,
     * the priority table let it go through */
    if (hit_debug_on() && hit_ent < 64) {
        unsigned char tn[0x30];
        int near_loc;
        char msg[160];
        memset(tn, 0, sizeof(tn));
        tr(tn, start, end, *(int*)shooter, SHOT_MASK, NEAREST_PRIO);
        near_loc = (*(unsigned short*)(tn + 0x28) == hit_ent) ? *(unsigned short*)(tn + 0x2c) : hit_loc;
        if (near_loc != hit_loc)
            snprintf(msg, sizeof(msg), "f \"^3hit ^2%s ^3through ^7%s\"", loc_name(hit_loc), loc_name(near_loc));
        else
            snprintf(msg, sizeof(msg), "f \"^7hit %s\"", loc_name(hit_loc));
        ((sendcmd_t)(g_base + RVA_TRAP_SEND_SERVER_CMD))(cn, 0, msg);
    }

    /* The latency question, asked to the engine itself: with every other player moved
     * back to where he was at the shooter's usercmd time (the antilag rewind, then the
     * same trace, then restore), would this bullet have hit? */
    if (!g_in_fire && want < now) {
        g_in_fire = 1;
        rewind_others(cn, want);
        if (g_moved_count > 0) {
            unsigned char t2[0x30];
            int e2, l2;
            memset(t2, 0, sizeof(t2));
            tr(t2, start, end, *(int*)shooter, SHOT_MASK, prio);
            e2 = *(unsigned short*)(t2 + 0x28);
            l2 = *(unsigned short*)(t2 + 0x2c);
            if (e2 < 64)
                n += snprintf(line + n, sizeof(line) - n, " | REWOUND(%dms): HIT cn=%d %s", now - want, e2, loc_name(l2));
            else
                n += snprintf(line + n, sizeof(line) - n, " | REWOUND(%dms): miss", now - want);
        }
        restore_others();
        g_in_fire = 0;
    }

    for (i = 0; i < AL_MAX_CLIENTS && n < (int)sizeof(line) - 200; ++i) {
        const char* ge = ent_at(i);
        float x_now, y_now, h_now, along_now, x_cmd, y_cmd, h_cmd, along_cmd, rew[3], moved, yaw;
        const float* org;
        const float* mn;
        const float* mx;
        int ef;
        char probes[80];
        if (i == cn) continue;
        if (*(const int*)(ge + E_ETYPE) != 1 || !*(void**)(ge + E_CLIENT)) continue;
        if (*(const int*)(ge + E_HEALTH) <= 0) continue;
        org = (const float*)(ge + E_ORIGIN);
        yaw = *(const float*)(ge + 0x40);                    /* apos.trBase[1] = view yaw */
        ray_vs_axis(start, dir, len, org, yaw, &x_now, &y_now, &h_now, &along_now);
        if (!hist_lookup(i, want, rew)) { rew[0] = org[0]; rew[1] = org[1]; rew[2] = org[2]; }
        ray_vs_axis(start, dir, len, rew, yaw, &x_cmd, &y_cmd, &h_cmd, &along_cmd);
        if (sqrtf(x_now * x_now + y_now * y_now) > SHOT_NEAR_U &&
            sqrtf(x_cmd * x_cmd + y_cmd * y_cmd) > SHOT_NEAR_U) continue;
        moved = sqrtf((org[0] - rew[0]) * (org[0] - rew[0]) + (org[1] - rew[1]) * (org[1] - rew[1]) +
                      (org[2] - rew[2]) * (org[2] - rew[2]));
        mn = (const float*)(ge + E_MINS);
        mx = (const float*)(ge + E_MAXS);
        ef = *(const int*)(ge + E_EFLAGS);
        probes[0] = 0;
        if (hit_ent != i) edge_probes(tr, start, dir, len, *(int*)shooter, prio, i, probes);
        n += snprintf(line + n, sizeof(line) - n,
                      " | cn=%d %s%s lean%+.2f yaw %.0f dist %.0fu: now x%+.1f y%+.1f h%.1f | @cmd x%+.1f y%+.1f h%.1f | moved %.1fu%s%s",
                      i, (mx[2] - mn[2]) < 40 ? "prone" : (mx[2] - mn[2]) < 55 ? "crouch" : "stand",
                      (ef & 0x1) ? " dead" : "", *(const float*)(ge + E_LEANF), yaw, along_now,
                      x_now, y_now, h_now, x_cmd, y_cmd, h_cmd, moved,
                      probes[0] ? " | 4u probes: " : "", probes);
    }
    if (g_shot_log > 0) {
        printf("%s\n", line);
        fflush(stdout);
    }
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
    /* the audit sees every root shot, whether or not the rewind is on */
    if (depth == 0 && !g_in_fire && (shot_log_on() | hit_debug_on())) {
        const size_t d = (size_t)((char*)shooter - (char*)(g_base + RVA_g_entities));
        void* c = (d % GENTITY_SIZE == 0 && d / GENTITY_SIZE < AL_MAX_CLIENTS)
                ? *(void**)((char*)shooter + E_CLIENT) : NULL;
        if (c)
            shot_audit(shooter, (int)(d / GENTITY_SIZE), (const float*)a3, (const float*)a4, a7,
                       level_time(), *(int*)((char*)c + PS_COMMANDTIME));
    }

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
