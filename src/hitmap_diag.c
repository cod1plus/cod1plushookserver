/*
 * hitmap_diag.c - DIAGNOSTIC ONLY (COD1RELOADED_HITMAP=1): the real bullet hit volume of
 * a player, as the server tests it.
 *
 * Fires a grid of rays through each live player with the game's own bullet trace
 * (trap_LocationalTrace, game 0x6817d = syscall 0x2e, content mask 0x2802031 and the
 * bulletPriorityMap - exactly what Bullet_Fire_Extended passes at 0x6e081..0x6e0b0) and
 * prints which hit location each ray gets: a front view (rays along the player's view
 * direction) and a side view (rays along his right). The engine poses the skeleton for
 * every trace, with pose_sync's shared code, so this is the tested body, holes included.
 *
 * Runs on the game thread from competitive_sv's G_RunFrame hook, only when the player's
 * stance / lean bucket changed and at most once per 2 s per player. One map per state goes
 * to /tmp/hitmap_<cn>_<state>.txt (overwritten). Never enable on a live server: a scan is
 * ~8000 traces in one frame.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "hitmap_diag.h"

#define RVA_TRAP_LOC_TRACE  0x06817d
#define RVA_BULLET_PRIO     0x082e6c   /* .dynsym bulletPriorityMap (19 bytes) */
#define RVA_G_ENTITIES      0x21d6c0
#define GENTITY_SIZE        0x31c
#define RVA_CLIENTINFO      0x1586cc   /* bgs + 0x9b6ec, stride 0x4b0 */
#define CI_STRIDE           0x4b0
#define RVA_LEVEL_TIME      (0x18f220 + 0x1e8)
#define GE_ETYPE            0x04
#define GE_EFLAGS           0x08
#define GE_CLIENTNUM        0x90
#define GE_LEANF            0xd4       /* es mirror of ps.leanf */
#define GE_CUR_ORIGIN       0x138
#define GE_HEALTH           0x238
#define CI_VIEWYAW          0x3ec
#define BULLET_MASK         0x2802031
#define ENTITYNUM_NONE      1023
#define ENTITYNUM_WORLD     1022

typedef void (*loctrace_t)(void* results, const float* start, const float* end,
                           int passEnt, int mask, const unsigned char* prio);

/* ---- the skeleton dump next to each map (for the visible-mesh overlay) ----------
 * The DObj is [main model, attachments...] (G_DObjUpdate 0x6b71c): main model index =
 * ent byte +0x179, attachment model indices = bytes +0x2f4..+0x2f9, their tags = u16
 * script strings +0x2fa.., ignoreCollision = bit i of byte +0x17a (G_EntAttach 0x6b9b9).
 * Bones are read BY NAME from a list in /tmp/hitmap_bones.txt with
 * G_DObjGetWorldTagMatrix(ent, const char* name, float out[16]): the name is a C STRING
 * (G_InitTurrets 0x510c5 / G_GetActivateEnt 0x4e15b pass a literal, and print it with %s
 * when it is missing) and out is a 4x4 - rows 0..2 = the bone's axes in world, origin at
 * out[12..14] (G_GetActivateEnt reads it at out+0x30). Returns 0 for a bone the DObj does
 * not have. SL_FindLowercaseString (0 = never a string, so never a bone) skips the rest. */
#define RVA_G_MODEL_NAME         0x06b5ae
#define RVA_SL_CONVERT_TO_STRING 0x072cfe
#define RVA_SL_FIND_LOWER        0x072d9a
#define RVA_G_DOBJ_WORLD_TAG     0x06c973
#define GE_MODEL                 0x179
#define GE_ATTACH_IGNORECOLL     0x17a
#define GE_ATTACH_MODEL          0x2f4
#define GE_ATTACH_TAG            0x2fa
typedef const char* (*g_model_name_t)(int index);
typedef const char* (*sl_to_string_t)(unsigned int s);
typedef unsigned short (*sl_find_lower_t)(const char* s);
typedef int (*g_world_tag_t)(void* ent, const char* tag, float* out);

static void dump_skeleton(uintptr_t base, const char* ge, const char* map_path)
{
    g_model_name_t  model_name = (g_model_name_t)(base + RVA_G_MODEL_NAME);
    sl_to_string_t  to_str     = (sl_to_string_t)(base + RVA_SL_CONVERT_TO_STRING);
    sl_find_lower_t find_lower = (sl_find_lower_t)(base + RVA_SL_FIND_LOWER);
    g_world_tag_t   world_tag  = (g_world_tag_t)(base + RVA_G_DOBJ_WORLD_TAG);
    char path[160], name[128];
    FILE *in, *out;
    int i, n = 0;
    const int mi = *(const unsigned char*)(ge + GE_MODEL);

    snprintf(path, sizeof(path), "%s.bones", map_path);
    out = fopen(path, "w");
    if (!out) return;
    if (mi) fprintf(out, "MODEL 0 %s - 0\n", model_name(mi));
    for (i = 0; i < 6; ++i) {
        const int ai = *(const unsigned char*)(ge + GE_ATTACH_MODEL + i);
        const unsigned short tag = *(const unsigned short*)(ge + GE_ATTACH_TAG + 2 * i);
        const char* ts;
        if (!ai) continue;
        ts = tag ? to_str(tag) : "";
        fprintf(out, "MODEL %d %s %s %d\n", i + 1, model_name(ai), (ts && ts[0]) ? ts : "-",
                (*(const unsigned char*)(ge + GE_ATTACH_IGNORECOLL) >> i) & 1);
    }
    in = fopen("/tmp/hitmap_bones.txt", "r");
    if (in) {
        while (fgets(name, sizeof(name), in)) {
            float m[16];
            size_t l = strlen(name);
            while (l && (name[l - 1] == '\n' || name[l - 1] == '\r')) name[--l] = 0;
            if (!l || !find_lower(name)) continue;
            memset(m, 0, sizeof(m));
            if (!world_tag((void*)ge, name, m)) continue;
            fprintf(out, "BONE %s|%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.3f %.3f\n",
                    name, m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10],
                    m[12], m[13], m[14]);
            ++n;
        }
        fclose(in);
    }
    fclose(out);
    printf("[hitmap] %s: %d bones\n", path, n);
    fflush(stdout);
}

static int  g_on = -1;
static int  g_last_state[64];
static int  g_last_time[64];

/* 0 none 1 helmet 2 head 3 neck 4/5 torso up/lo 6-9 arms 10-11 hands 12-15 legs
 * 16-17 feet 18 gun */
static char loc_char(int loc)
{
    static const char c[19] = { 'o', 'C', 'H', 'N', 'T', 't', 'A', 'A', 'a', 'a',
                                'M', 'M', 'L', 'L', 'l', 'l', 'P', 'P', 'G' };
    return (loc >= 0 && loc < 19) ? c[loc] : '?';
}

static void scan(uintptr_t base, int cn, const char* ge, const char* ci, int state)
{
    loctrace_t tr = (loctrace_t)(base + RVA_TRAP_LOC_TRACE);
    const unsigned char* prio = (const unsigned char*)(base + RVA_BULLET_PRIO);
    const float* o = (const float*)(ge + GE_CUR_ORIGIN);
    const float yaw = *(const float*)(ci + CI_VIEWYAW) * 0.01745329252f;
    const float f[3] = { cosf(yaw), sinf(yaw), 0 }, r[3] = { sinf(yaw), -cosf(yaw), 0 };
    int count[19] = { 0 }, view, x, z, i;
    char path[128], row[80];
    FILE* out;
    float leanf;
    int ef;

    memcpy(&leanf, ge + GE_LEANF, 4);
    memcpy(&ef, ge + GE_EFLAGS, 4);
    snprintf(path, sizeof(path), "/tmp/hitmap_%d_%s_lean%+d.txt", cn,
             (ef & 0x40) ? "prone" : (ef & 0x20) ? "crouch" : "stand", state % 10 - 3);
    out = fopen(path, "w");
    if (!out) return;
    fprintf(out, "cn %d  eFlags 0x%x  leanf %+.3f  yaw %.1f  origin %.1f %.1f %.1f  level.time %d\n"
                 "C helmet H head N neck T torso_up t torso_lo A arm_up a arm_lo M hand L leg_up "
                 "l leg_lo P foot G gun o none(on player) # world . miss   1 char = 1 unit\n",
            cn, ef, leanf, yaw / 0.01745329252f, o[0], o[1], o[2],
            *(const int*)(base + RVA_LEVEL_TIME));

    for (view = 0; view < 2; ++view) {
        /* view 0: seen from the FRONT (rays along -forward), columns = his right (+) ..
         * view 1: seen from his RIGHT side (rays along -right), columns = his forward */
        const float* d  = view ? r : f;       /* ray comes from +d */
        const float* ax = view ? f : r;       /* column axis */
        fprintf(out, "\n%s view (column = %s, left edge -32, right edge +32, center |)\n",
                view ? "SIDE (from his right)" : "FRONT", view ? "his forward" : "his right");
        for (z = 80; z >= -2; --z) {
            int n = 0;
            for (x = -32; x <= 32; ++x) {
                unsigned char t[64];
                float s[3], e[3];
                int ent, loc;
                for (i = 0; i < 3; ++i) {
                    const float p = o[i] + ax[i] * (float)x + (i == 2 ? (float)z : 0.0f);
                    s[i] = p + d[i] * 45.0f;     /* 45: clears a wall next to him, still */
                    e[i] = p - d[i] * 45.0f;     /* outside any stance (prone head ~30) */
                }
                memset(t, 0, sizeof(t));
                tr(t, s, e, ENTITYNUM_NONE, BULLET_MASK, prio);
                ent = *(unsigned short*)(t + 0x28);
                loc = *(unsigned short*)(t + 0x2c);
                if (ent == cn) {
                    row[n++] = loc_char(loc);
                    if (loc >= 0 && loc < 19) count[loc]++;
                } else if (*(float*)t < 1.0f) {
                    row[n++] = '#';
                } else {
                    row[n++] = (x == 0) ? '|' : '.';
                }
            }
            row[n] = 0;
            fprintf(out, "z%3d %s\n", z, row);
        }
    }
    fprintf(out, "\ncells: helmet %d head %d neck %d torso_up %d torso_lo %d arms %d hands %d legs %d feet %d gun %d none %d\n",
            count[1], count[2], count[3], count[4], count[5], count[6] + count[7] + count[8] + count[9],
            count[10] + count[11], count[12] + count[13] + count[14] + count[15], count[16] + count[17],
            count[18], count[0]);
    fclose(out);
    printf("[hitmap] %s written\n", path);
    fflush(stdout);
    dump_skeleton(base, ge, path);
}

void hitmap_frame(uintptr_t base)
{
    int i;
    if (g_on < 0) {
        /* 1 = one map per stance/lean state; N >= 2 = rescan every N seconds (the first
         * scan after a spawn catches the spawn animation, so a periodic one is needed
         * to see the settled idle pose) */
        const char* e = getenv("COD1RELOADED_HITMAP");
        g_on = e ? atoi(e) : 0;
        if (g_on < 0) g_on = 0;
        if (g_on) { printf("[hitmap] DIAGNOSTIC ON (%s) - never on a live server\n",
                           g_on >= 2 ? "periodic" : "per state"); fflush(stdout); }
    }
    if (!g_on || !base) return;
    for (i = 0; i < 64; ++i) {
        const char* ge = (const char*)(base + RVA_G_ENTITIES) + (size_t)i * GENTITY_SIZE;
        const char* ci = (const char*)(base + RVA_CLIENTINFO) + (size_t)i * CI_STRIDE;
        const int now = *(const int*)(base + RVA_LEVEL_TIME);
        float leanf;
        int ef, state;
        if (*(const int*)(ge + GE_ETYPE) != 1 || *(const int*)(ge + GE_CLIENTNUM) != i) continue;
        if (*(const int*)(ge + GE_HEALTH) <= 0) continue;
        memcpy(&leanf, ge + GE_LEANF, 4);
        memcpy(&ef, ge + GE_EFLAGS, 4);
        /* state = stance * 10 + lean bucket (-3..+3 over leanf -0.5..0.5) + 3 */
        state = ((ef & 0x40) ? 2 : (ef & 0x20) ? 1 : 0) * 10 + (int)lrintf(leanf * 6.0f) + 3;
        if (g_on >= 2) {
            if (g_last_time[i] && now - g_last_time[i] < g_on * 1000 && now >= g_last_time[i]) continue;
        } else {
            if (state == g_last_state[i] && g_last_time[i]) continue;          /* scanned */
            if (now - g_last_time[i] < 1500 && now >= g_last_time[i]) continue;  /* cooldown */
        }
        g_last_state[i] = state;
        g_last_time[i] = now;
        scan(base, i, ge, ci, state);
    }
}
