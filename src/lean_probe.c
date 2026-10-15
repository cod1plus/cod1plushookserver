/*
 * lean_probe.c - DIAGNOSTIC ONLY (COD1RELOADED_LEAN_PROBE=1): where a leaning player's hit
 * volume is, against his own eye, in every stance / side / movement - measured on the real
 * server with a test client that plays the keys a player would.
 *
 * The question it answers ("killed behind a wall"): which parts of a leaning player can be
 * hit while his eye - the point he sees AND shoots from - is still behind the corner. The
 * bullet trace poses the skeleton with the server's own code (pose_sync + the shared
 * lean_controllers), so hitmap_scan_shot() records exactly what a bullet can hit.
 *
 * How the test client is driven: ClientThink_real(ent, cmd) (game 0x39116, the same entry
 * lean_hitbox.c hooks) gets the usercmd before pmove. For the first live test client the
 * hook rewrites, every frame:
 *   - cmd->wbuttons (+5): 0x10 lean left / 0x20 lean right (PM_UpdateLean 0x25c8e..0x25cb0),
 *     0x80 crouch / 0x40 prone (the client builds them from cl_stance: CoDMP 0x40b9a0);
 *   - cmd->forwardmove / rightmove (+0x14 / +0x15), upmove 0; fire cleared.
 * A script of phases (stance x side x movement, PHASE_MS each) runs LOOPS times; SCAN_AT ms
 * into each phase (the lean is full after 350 ms, the legs have swung) one scan is written:
 * /tmp/hitmap_shot_<tag>_<cn>.txt (+ .bones, with /tmp/hitmap_bones.txt) and a state line
 * goes to stdout (lean, eFlags, movementDir, velocity, legs / torso / view yaw, origin).
 * Moving phases come in opposite pairs so the client stays near where it started.
 *
 * Never enable on a live server: one scan is ~8000 traces in one frame, and the hook takes
 * over a test client's controls.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include "lean_probe.h"
#include "hooks.h"
#include "hitmap_diag.h"
#include "netstat_sv.h"

#define TAG           "[lean_probe]"
#define GAME_SO_NAME  "game.mp.i386.so"

/* game.mp.i386.so md5 343f99cd67b79ac74aeaa5261f63c011 (same constants as hitmap_diag.c /
 * lean_hitbox.c / netstat_sv.c) */
#define RVA_CLIENTTHINK_REAL 0x39116
#define RVA_G_ENTITIES       0x21d6c0
#define GENTITY_SIZE         0x31c
#define RVA_CLIENTINFO       0x1586cc
#define CI_STRIDE            0x4b0
#define RVA_LEVEL_TIME       (0x18f220 + 0x1e8)
#define GE_ETYPE             0x04
#define GE_EFLAGS            0x08
#define GE_MOVEDIR           0x6c     /* es.angles2[1] = ps.movementDir */
#define GE_CLIENTNUM         0x90
#define GE_LEGSANIM          0xcc
#define GE_ORIGIN            0x138
#define GE_CLIENT            0x15c
#define GE_HEALTH            0x238
#define PS_VELOCITY          0x20
#define PS_LEANF             0x40
#define PS_VIEWYAW           0xc4
#define CI_LEGS_YAW          0x380
#define CI_TORSO_YAW         0x3b0
#define CMD_BUTTONS          4
#define CMD_WBUTTONS         5
#define CMD_FORWARD          0x14
#define CMD_RIGHT            0x15
#define CMD_UP               0x16
#define WB_LEAN_LEFT         0x10
#define WB_LEAN_RIGHT        0x20
#define WB_PRONE             0x40
#define WB_CROUCH            0x80

#define PHASE_MS  1600
#define SCAN_AT   1100
#define LOOPS     2

static const unsigned char CT_PROLOGUE[5] = { 0x55, 0x89, 0xe5, 0x56, 0x53 };

typedef struct { const char* name; signed char fwd, right; } move_t;
static const move_t MOVES[] = {
    { "still",   0,    0    },
    { "strafeL", 0,    -127 },
    { "strafeR", 0,    127  },
    { "fwdL",    127,  -127 },
    { "backR",   -127, 127  },
    { "fwdR",    127,  127  },
    { "backL",   -127, -127 },
    { "fwd",     127,  0    },
    { "back",    -127, 0    },
};
#define N_MOVES ((int)(sizeof(MOVES) / sizeof(MOVES[0])))

typedef struct { int stance, lean, move; } phase_t;   /* stance 0 stand 1 crouch; lean -1/0/+1 */
static phase_t g_ph[64];
static int     g_nph;

typedef void (*ClientThink_real_t)(void* ent, void* cmd);
static hook_t             g_hook;
static ClientThink_real_t orig_think;
static uintptr_t          g_base;

static int g_enabled = 0;
static volatile int g_cn  = -1;    /* the test client being driven */
static volatile int g_cur = -1;    /* current phase index, -1 = hands off */
static int g_t0, g_scanned = -1, g_done = 0;

static void build_script(void)
{
    int st, ln, mv;
    g_nph = 0;
    for (st = 0; st < 2; ++st) {
        g_ph[g_nph].stance = st; g_ph[g_nph].lean = 0; g_ph[g_nph].move = 0; ++g_nph;   /* baseline */
        for (ln = -1; ln <= 1; ln += 2)
            for (mv = 0; mv < N_MOVES; ++mv) {
                g_ph[g_nph].stance = st; g_ph[g_nph].lean = ln; g_ph[g_nph].move = mv; ++g_nph;
            }
    }
}

static void hook_ClientThink_real(void* ent, void* cmd)
{
    const int cn = *(const int*)ent;                 /* s.number */
    const int cur = g_cur;
    if (cmd && cur >= 0 && cn == g_cn) {
        unsigned char* c = (unsigned char*)cmd;
        const phase_t* p = &g_ph[cur];
        unsigned char wb = (unsigned char)(c[CMD_WBUTTONS] & 0x0f);
        if (p->lean < 0) wb |= WB_LEAN_LEFT;
        else if (p->lean > 0) wb |= WB_LEAN_RIGHT;
        if (p->stance == 1) wb |= WB_CROUCH;
        c[CMD_WBUTTONS] = wb;
        c[CMD_BUTTONS] &= (unsigned char)~1;      /* never fire */
        c[CMD_FORWARD] = (unsigned char)MOVES[p->move].fwd;
        c[CMD_RIGHT]   = (unsigned char)MOVES[p->move].right;
        c[CMD_UP]      = 0;
    }
    orig_think(ent, cmd);
}

static uintptr_t find_game_base(void)
{
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    uintptr_t base = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start;
        if (!strstr(line, GAME_SO_NAME)) continue;
        start = (uintptr_t)strtoul(line, NULL, 16);
        if (start && (base == 0 || start < base)) base = start;
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
    target = (unsigned char*)(base + RVA_CLIENTTHINK_REAL);
    if (base == g_base && target[0] == 0xE9) return;          /* ours, still live */
    if (memcmp(target, CT_PROLOGUE, sizeof(CT_PROLOGUE)) != 0) {
        if (!warned) {
            printf("%s ClientThink_real @0x%08lx is %02x %02x %02x (another hook? lean_hitbox must be off) - not installed\n",
                   TAG, (unsigned long)(uintptr_t)target, target[0], target[1], target[2]);
            fflush(stdout);
            warned = 1;
        }
        return;
    }
    if (hook_install(&g_hook, (uintptr_t)target, (uintptr_t)hook_ClientThink_real, 5) == 0) {
        orig_think = (ClientThink_real_t)g_hook.trampoline;
        g_base = base;
        warned = 0;
        printf("%s installed (game base 0x%08lx): %d phases x %d ms, scan at %d ms, %d loops\n",
               TAG, (unsigned long)base, g_nph, PHASE_MS, SCAN_AT, LOOPS);
        fflush(stdout);
    }
}

static void* watcher(void* arg)
{
    (void)arg;
    for (;;) { try_install(); usleep(400 * 1000); }
    return NULL;
}

void lean_probe_init(void)
{
    const char* e = getenv("COD1RELOADED_LEAN_PROBE");
    pthread_t th;
    if (!e || atoi(e) <= 0) return;
    g_enabled = 1;
    build_script();
    printf("%s DIAGNOSTIC ON - drives the first test client, never on a live server\n", TAG);
    fflush(stdout);
    if (pthread_create(&th, NULL, watcher, NULL) == 0) pthread_detach(th);
}

static const char* stance_name(int s) { return s ? "crouch" : "stand"; }
static const char* lean_name(int l)   { return l < 0 ? "L" : l > 0 ? "R" : "none"; }

void lean_probe_frame(uintptr_t base)
{
    const char* ge;
    const char* ci;
    const char* ps;
    int now, el, ph, loop, i;
    if (!g_enabled || !base || g_done || base != g_base) return;
    now = *(const int*)(base + RVA_LEVEL_TIME);

    if (g_cn < 0) {
        for (i = 0; i < 64; ++i) {
            ge = (const char*)(base + RVA_G_ENTITIES) + (size_t)i * GENTITY_SIZE;
            if (*(const int*)(ge + GE_ETYPE) != 1 || *(const int*)(ge + GE_CLIENTNUM) != i) continue;
            if (*(const int*)(ge + GE_HEALTH) <= 0 || !netstat_is_bot(i)) continue;
            g_cn = i;
            g_t0 = now;
            g_scanned = -1;
            printf("%s driving test client %d from level.time %d\n", TAG, i, now);
            fflush(stdout);
            break;
        }
        if (g_cn < 0) return;
    }
    ge = (const char*)(base + RVA_G_ENTITIES) + (size_t)g_cn * GENTITY_SIZE;
    if (*(const int*)(ge + GE_ETYPE) != 1 || *(const int*)(ge + GE_HEALTH) <= 0 || now < g_t0) {
        printf("%s test client %d gone / dead / map restart - waiting for one\n", TAG, g_cn);
        fflush(stdout);
        g_cur = -1;
        g_cn = -1;
        return;
    }
    el = now - g_t0;
    ph = el / PHASE_MS;
    loop = ph / g_nph;
    if (loop >= LOOPS) {
        g_cur = -1;
        g_done = 1;
        printf("%s done: %d scans in /tmp/hitmap_shot_*_%d.txt\n", TAG, LOOPS * g_nph, g_cn);
        fflush(stdout);
        return;
    }
    g_cur = ph % g_nph;
    if (el % PHASE_MS >= SCAN_AT && g_scanned != ph) {
        const phase_t* p = &g_ph[g_cur];
        const float* vel;
        const float* o = (const float*)(ge + GE_ORIGIN);
        char tag[64];
        g_scanned = ph;
        ps = *(const char* const*)(ge + GE_CLIENT);
        ci = (const char*)(base + RVA_CLIENTINFO) + (size_t)g_cn * CI_STRIDE;
        if (!ps) return;
        vel = (const float*)(ps + PS_VELOCITY);
        snprintf(tag, sizeof(tag), "L%d_p%02d_%s_%s_%s", loop, g_cur, stance_name(p->stance),
                 lean_name(p->lean), MOVES[p->move].name);
        printf("%s %s: leanf %+.3f ef 0x%x legsAnim %d mdir %+.0f vel %+.0f %+.0f viewY %.1f legsY %.1f torsoY %.1f org %.1f %.1f %.1f t %d\n",
               TAG, tag, *(const float*)(ps + PS_LEANF), *(const int*)(ge + GE_EFLAGS),
               *(const int*)(ge + GE_LEGSANIM), *(const float*)(ge + GE_MOVEDIR), vel[0], vel[1],
               *(const float*)(ps + PS_VIEWYAW), *(const float*)(ci + CI_LEGS_YAW),
               *(const float*)(ci + CI_TORSO_YAW), o[0], o[1], o[2], now);
        fflush(stdout);
        hitmap_scan_shot(base, g_cn, tag);
    }
}
