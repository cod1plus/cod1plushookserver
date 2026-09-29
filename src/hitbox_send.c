/*
 * hitbox_send.c - the server's tested hit boxes, streamed to the client that looks at them.
 *
 * Why: "je vise l'epaule, ca traverse" cannot be settled from the server alone. The server
 * tests one oriented box per bone on the pose it computes (hitbox_fix.c); what the player
 * sees is what his client draws. Sending him the boxes exactly as the server tests them at
 * this instant lets his client overlay them on the model: a visible gap between the drawn
 * body and the boxes IS the bug, on screen, no interpretation needed.
 *
 * Per human client, 5 times a second: the other live player nearest to his view direction
 * (within 800 u and 30 degrees) is picked; for each bone of a fixed list the world matrix
 * comes from G_DObjGetWorldTagMatrix (the pose the hit test uses, pose_sync included) and
 * the box from the same data hitbox_fix.c patched into the engine (HB_SHIPPED + the
 * model's set), and it all goes out as server commands:
 *     hb <cn> <part> <nparts> <ox> <oy> <oz> <bone> <bone> ...
 *     bone = id,px,py,pz,r00,r01,r02,r10,r11,r12,r20,r21,r22,mnx,mny,mnz,mxx,mxy,mxz
 * (positions relative to the player origin, 1 decimal; axes = the bone's local axes in
 * world; box = mins/maxs in that frame). A command stays under 900 bytes.
 *
 * Gate: the SERVER cvar cod1x_drawhitbox (rcon set cod1x_drawhitbox 1), polled once a
 * second through the game's trap_Cvar_VariableIntegerValue. Costs nothing when 0.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <math.h>

#include "hitbox_send.h"
#include "hitbox_data.h"
#include "netstat_sv.h"

#define TAG "[hitbox_send]"
#define RVA_G_ENTITIES        0x21d6c0
#define GENTITY_SIZE          0x31c
#define RVA_LEVEL_TIME        (0x18f220 + 0x1e8)
#define RVA_TRAP_SENDCMD      0x067c61   /* trap_SendServerCommand(cn, 0, text) */
#define RVA_TRAP_CVAR_INT     0x067b0c   /* trap_Cvar_VariableIntegerValue(name) */
#define RVA_G_MODEL_NAME      0x06b5ae   /* G_ModelName(index) */
#define RVA_G_DOBJ_WORLD_TAG  0x06c973   /* G_DObjGetWorldTagMatrix(ent, name, float out[16]) */
#define GE_ETYPE     0x04
#define GE_EFLAGS    0x08
#define GE_APOS      0x3c      /* pitch, yaw */
#define GE_CLIENTNUM 0x90
#define GE_ORIGIN    0x138
#define GE_CLIENT    0x15c
#define GE_MODEL     0x179
#define GE_HEALTH    0x238
#define PERIOD_MS    200
#define MAX_CMD      900

typedef void        (*sendcmd_t)(int cn, int a, const char* text);
typedef int         (*cvar_int_t)(const char* name);
typedef const char* (*model_name_t)(int index);
typedef int         (*world_tag_t)(void* ent, const char* tag, float* out);

static const char* const BONES[] = {
    "head", "neck", "back_up", "back_mid", "back_low",
    "bip01 l clavicle", "bip01 r clavicle", "bip01 l upperarm", "bip01 r upperarm",
    "bip01 l forearm", "bip01 r forearm", "bip01 l thigh", "bip01 r thigh"
};
#define NBONES (sizeof(BONES) / sizeof(BONES[0]))

static int g_on = 0;
static int g_last_poll = 0;
static int g_last_send[64];
static int g_bone_idx[NBONES];      /* index of each BONES entry in HB_BONE_NAMES */
static int g_idx_ready = 0;

static int bone_index(const char* name) {
    unsigned i;
    for (i = 0; i < HB_NBONES; ++i) if (!strcasecmp(HB_BONE_NAMES[i], name)) return (int)i;
    return -1;
}

/* the box the server tests for bone `bi` of body model `set` (NULL = shipped) */
static void tested_box(const hb_set_t* set, int bi, float mins[3], float maxs[3]) {
    unsigned k;
    memcpy(mins, HB_SHIPPED[bi], 12);
    memcpy(maxs, HB_SHIPPED[bi] + 3, 12);
    if (!set) return;
    for (k = 0; k < set->nfix; ++k)
        if (set->fix[k].bone == bi) { memcpy(mins, set->fix[k].mins, 12); memcpy(maxs, set->fix[k].maxs, 12); return; }
}

static const hb_set_t* set_for_model(const char* name) {
    unsigned k;
    if (!name) return NULL;
    for (k = 0; k < HB_NUM_LOOKUP; ++k)
        if (!strcasecmp(HB_LOOKUP[k].xmodel, name)) return &HB_SETS[HB_LOOKUP[k].set];
    return &HB_UNIVERSAL;
}

/* the live player entity nearest to the viewer's line of sight, or -1 */
static int pick_target(uintptr_t base, int viewer) {
    const char* ge = (const char*)(base + RVA_G_ENTITIES) + (size_t)viewer * GENTITY_SIZE;
    const float* o = (const float*)(ge + GE_ORIGIN);
    const int ef = *(const int*)(ge + GE_EFLAGS);
    const float pitch = *(const float*)(ge + GE_APOS) * 0.01745329252f;
    const float yaw   = *(const float*)(ge + GE_APOS + 4) * 0.01745329252f;
    const float d[3]  = { cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), -sinf(pitch) };
    const float eye[3] = { o[0], o[1], o[2] + ((ef & 0x40) ? 11.0f : (ef & 0x20) ? 40.0f : 60.0f) };
    int best = -1, i;
    float best_ang = 0.5f;   /* ~30 degrees */
    for (i = 0; i < 64; ++i) {
        const char* te = (const char*)(base + RVA_G_ENTITIES) + (size_t)i * GENTITY_SIZE;
        const float* to;
        float v[3], along, lat2, ang;
        if (i == viewer) continue;
        if (*(const int*)(te + GE_ETYPE) != 1 || !*(void* const*)(te + GE_CLIENT)) continue;
        if (*(const int*)(te + GE_HEALTH) <= 0) continue;
        to = (const float*)(te + GE_ORIGIN);
        v[0] = to[0] - eye[0]; v[1] = to[1] - eye[1]; v[2] = to[2] + 40.0f - eye[2];
        along = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
        if (along < 20.0f || along > 800.0f) continue;
        lat2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - along * along;
        ang = sqrtf(lat2 > 0 ? lat2 : 0) / along;
        if (ang < best_ang) { best_ang = ang; best = i; }
    }
    return best;
}

static void send_target(uintptr_t base, int viewer, int target) {
    sendcmd_t    send  = (sendcmd_t)(base + RVA_TRAP_SENDCMD);
    model_name_t mname = (model_name_t)(base + RVA_G_MODEL_NAME);
    world_tag_t  wtag  = (world_tag_t)(base + RVA_G_DOBJ_WORLD_TAG);
    char* te = (char*)(base + RVA_G_ENTITIES) + (size_t)target * GENTITY_SIZE;
    const float* o = (const float*)(te + GE_ORIGIN);
    const unsigned char mi = *(const unsigned char*)(te + GE_MODEL);
    const char* model = mi ? mname(mi) : NULL;
    const hb_set_t* set = set_for_model(model);
    char  parts[4][MAX_CMD + 64];
    int   np = 0, n = 0;
    unsigned b;
    int   head_len;
    char  head[64];
    /* fill part bodies first, then prefix each with its header */
    char  body[4][MAX_CMD];
    int   blen[4] = { 0, 0, 0, 0 };
    for (b = 0; b < NBONES; ++b) {
        float m[16], mins[3], maxs[3];
        char tok[200];
        int  tl;
        if (g_bone_idx[b] < 0) continue;
        if (!wtag(te, BONES[b], m)) continue;
        tested_box(set, g_bone_idx[b], mins, maxs);
        tl = snprintf(tok, sizeof(tok),
                      " %d,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",
                      g_bone_idx[b], m[12] - o[0], m[13] - o[1], m[14] - o[2],
                      m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10],
                      mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2]);
        if (np == 0 || blen[np - 1] + tl > MAX_CMD - 80) {
            if (np == 4) break;
            blen[np] = 0; body[np][0] = 0; np++;
        }
        memcpy(body[np - 1] + blen[np - 1], tok, (size_t)tl + 1);
        blen[np - 1] += tl;
        n++;
    }
    if (!n) return;
    for (b = 0; b < (unsigned)np; ++b) {
        head_len = snprintf(head, sizeof(head), "hb %d %u %d %.1f %.1f %.1f", target, b, np, o[0], o[1], o[2]);
        memcpy(parts[b], head, (size_t)head_len);
        memcpy(parts[b] + head_len, body[b], (size_t)blen[b] + 1);
        send(viewer, 0, parts[b]);
    }
}

void hitbox_send_frame(uintptr_t base) {
    int now, i;
    if (!base) return;
    now = *(const int*)(base + RVA_LEVEL_TIME);
    if (now - g_last_poll > 1000 || now < g_last_poll) {
        const int was = g_on;
        g_last_poll = now;
        g_on = ((cvar_int_t)(base + RVA_TRAP_CVAR_INT))("cod1x_drawhitbox") != 0;
        if (g_on != was) {
            printf("%s cod1x_drawhitbox %d - %s\n", TAG, g_on,
                   g_on ? "streaming the tested boxes to each player (5 Hz, nearest target)" : "off");
            fflush(stdout);
        }
        if (g_on && !g_idx_ready) {
            unsigned b;
            for (b = 0; b < NBONES; ++b) g_bone_idx[b] = bone_index(BONES[b]);
            g_idx_ready = 1;
        }
    }
    if (!g_on) return;
    for (i = 0; i < 64; ++i) {
        const char* ge = (const char*)(base + RVA_G_ENTITIES) + (size_t)i * GENTITY_SIZE;
        int target;
        if (*(const int*)(ge + GE_ETYPE) != 1 || *(const int*)(ge + GE_CLIENTNUM) != i) continue;
        if (!*(void* const*)(ge + GE_CLIENT) || netstat_is_bot(i)) continue;
        if (now - g_last_send[i] < PERIOD_MS && now >= g_last_send[i]) continue;
        g_last_send[i] = now;
        target = pick_target(base, i);
        if (target >= 0) send_target(base, i, target);
    }
}
