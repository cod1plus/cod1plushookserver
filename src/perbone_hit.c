/*
 * perbone_hit.c - self-contained per-bone bullet hit refinement (anti "clip").
 *
 * FINE phase layered on top of lean_hitbox.c's AABB shift (the BROAD phase):
 *   - lean_hitbox.c shifts the player's single collision box to the leaned
 *     position so the engine box-trace REGISTERS a hit on a peeking player.
 *   - this module hooks trap_LocationalTrace; when the engine reports a player
 *     box-hit, it poses the victim's skeleton, reads every bone's WORLD position
 *     (index-based, no names), shifts them by the SAME lean offset (CoD1's pose
 *     has no lean), and tests the bullet segment against a sphere on each bone.
 *     Any bone within the sphere radius -> body hit (hitloc by bone height);
 *     no bone -> neutralize to a world miss (the box over-hang becomes a miss).
 *
 * Net: the wide box catches every legit shot; the per-bone pass rejects the
 * over-hang -> "shoot beside a leaning player = miss", like CoD2.
 *
 * RE-confirmed in-game: model 'player' = 110 bones, 3ds-Max Biped rig; pose
 * already bakes the yaw into local bone matrices, so world = local + currentOrigin
 * (no rotation). trap_DObjGetMatrixArray/NumBones take the gentity and deref
 * *(int*)ent internally.
 *
 * Head/neck are classified by BONE NAME (Bip01 Head / Bip01 Neck), not by height:
 * height alone put the neck/clavicle in the head zone (false headshots between the
 * ear and the shoulder) and broke in crouch/prone. Body zones stay height-based.
 *
 * MODES (env COD1RELOADED_PERBONE_HIT): unset/off = not installed; dump = probe
 * + log (no override); 1/on = full override.
 * Tuning: COD1RELOADED_PERBONE_RADIUS (body sphere, default 6.0),
 *         COD1RELOADED_PERBONE_HEADRAD (head sphere, default 6.0),
 *         COD1RELOADED_PERBONE_NECKRAD (neck sphere, default 5.0),
 *         COD1RELOADED_PERBONE_LEANFRAC (bone lean shift, default 0.5).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "hooks.h"

#define TAG "[perbone]"
#define GAME_SO_NAME "game.mp.i386.so"

/* ---- game.mp.i386.so RVAs (md5 343f99cd...) ---- */
#define RVA_LOCTRACE_STUB     0x6817d  /* trap_LocationalTrace, 6-arg cdecl, syscall 0x2e */
#define RVA_G_DObjCalcPose    0x6c7e4  /* void(ent) */
#define RVA_G_DObjWorldTag    0x6c973  /* int(ent, const char* bone, float out[16]) */
#define RVA_DObjNumBones      0x6937b  /* int(ent) -> bone count */
#define RVA_DObjMatrixArray   0x693e2  /* float*(ent) -> local matrix array, stride 0x40 */
#define RVA_AddLeanToPosition 0x71a7c  /* void(float* pos, float yaw, float leanf, float, float) */
#define RVA_g_entities        0x21d6c0 /* gentity_t array base (.bss) */

/* trap_LocationalTrace prologue: push ebp; mov ebp,esp; push ebx; sub $0x24,esp */
static const unsigned char LT_PROLOGUE[7] = { 0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x24 };
#define LT_PATCHLEN 7

/* ---- gentity_t / gclient offsets ---- */
#define E_CLIENT      0x15c
#define E_ORIGIN      0x138   /* r.currentOrigin vec3 */
#define GENTITY_SIZE  0x31c
#define PS_LEANF      0x40
#define PS_VIEWYAW    0xc4

#define ENTITYNUM_NONE 0x3ff
#define PB_MAX_CLIENTS 64
#define PB_MAX_BONES   160

/* ---- hitloc group ids ---- */
enum {
    HL_NONE = 0, HL_HELMET, HL_HEAD, HL_NECK, HL_TORSO_UPPER, HL_TORSO_LOWER,
    HL_R_ARM_U, HL_L_ARM_U, HL_R_ARM_L, HL_L_ARM_L, HL_R_HAND, HL_L_HAND,
    HL_R_LEG_U, HL_L_LEG_U, HL_R_LEG_L, HL_L_LEG_L, HL_R_FOOT, HL_L_FOOT, HL_GUN
};

/* hitloc by local bone height — BODY zones only. Head + neck are name-based (see
 * perbone_test): classifying them by height mis-fires because the neck/clavicle
 * bones sit at head height -> "shoot between the ear and the shoulder = headshot",
 * and height-thresholds break entirely in crouch/prone (the head is no longer >=56).
 * So this NEVER returns HL_HEAD/HL_NECK anymore. */
static int hitloc_for_z(float localz)
{
    if (localz >= 44.0f) return HL_TORSO_UPPER;   /* shoulders / clavicle / upper chest */
    if (localz >= 28.0f) return HL_TORSO_LOWER;
    return HL_R_LEG_U;                             /* generic leg */
}

/* ---- trace_t (0x30) ---- */
typedef struct {
    float    fraction;     /* +0x00 */
    float    endpos[3];    /* +0x04 */
    float    normal[3];    /* +0x10 */
    int32_t  surfaceFlags; /* +0x1c */
    int32_t  _pad20, _pad24;
    uint16_t hitEntityNum; /* +0x28 */
    uint16_t _pad2a;
    uint16_t hitLocation;  /* +0x2c */
    uint16_t _pad2e;
} pb_trace_t;

typedef void   (*loctrace_t)(pb_trace_t*, const float*, const float*, int, int, int);
typedef void   (*calcpose_t)(void*);
typedef int    (*worldtag_t)(void*, const char*, float*);
typedef int    (*numbones_t)(void*);
typedef float* (*matarr_t)(void*);
typedef void   (*addlean_t)(float*, float, float, float, float);

/* ---- resolved at install ---- */
static uintptr_t  g_base = 0;
static hook_t     g_lt_hook;
static loctrace_t real_loctrace = NULL;
static calcpose_t p_CalcPose    = NULL;
static worldtag_t p_WorldTag    = NULL;
static numbones_t p_NumBones    = NULL;
static matarr_t   p_MatrixArray = NULL;
static addlean_t  p_AddLean     = NULL;

/* ---- config ---- */
static int   g_mode      = 0;     /* 0 off, 1 on, 2 dump */
static float g_radius    = 5.0f;  /* per-bone sphere radius (body) */
static float g_head_rad  = 4.5f;  /* head capsule radius (skull half-width) */
static float g_head_len  = 8.0f;  /* head capsule length, from the head bone up the skull */
static float g_neck_rad  = 3.0f;  /* neck sphere radius (the neck is thin) */
static float g_cap_scale = 1.0f;  /* limb capsule radius multiplier */
static float g_lean_frac = 0.5f;
static int    g_in_trace = 0;
static time_t g_last_dump = 0;   /* dump-mode throttle: re-dump the pose 1/2s */

/* ============================== math ============================== */
static inline float dot3(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* Squared distance from point p to segment a->b; out_t = fraction along a->b. */
static float pt_seg_dist2(const float* p, const float* a, const float* b, float* out_t)
{
    float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
    float ap[3] = { p[0]-a[0], p[1]-a[1], p[2]-a[2] };
    float t = dot3(ap, ab) / (dot3(ab, ab) + 1e-9f);
    t = clampf(t, 0.0f, 1.0f);
    float c[3] = { a[0]+ab[0]*t, a[1]+ab[1]*t, a[2]+ab[2]*t };
    float d[3] = { p[0]-c[0], p[1]-c[1], p[2]-c[2] };
    if (out_t) *out_t = t;
    return dot3(d, d);
}

/* Squared distance between segment p1->q1 (bullet) and p2->q2 (capsule axis).
 * out_s = fraction along the BULLET; cp = closest point on the bullet. */
static float seg_seg_dist2(const float* p1, const float* q1,
                           const float* p2, const float* q2,
                           float* out_s, float* cp)
{
    float d1[3] = { q1[0]-p1[0], q1[1]-p1[1], q1[2]-p1[2] };
    float d2[3] = { q2[0]-p2[0], q2[1]-p2[1], q2[2]-p2[2] };
    float r[3]  = { p1[0]-p2[0], p1[1]-p2[1], p1[2]-p2[2] };
    float a = dot3(d1,d1), e = dot3(d2,d2), f = dot3(d2,r);
    const float EPS = 1e-6f;
    float s, t;

    if (a <= EPS && e <= EPS)      { s = 0.0f; t = 0.0f; }
    else if (a <= EPS)             { s = 0.0f; t = clampf(f/e, 0.0f, 1.0f); }
    else {
        float c = dot3(d1, r);
        if (e <= EPS) { t = 0.0f; s = clampf(-c/a, 0.0f, 1.0f); }
        else {
            float b = dot3(d1, d2);
            float denom = a*e - b*b;
            s = (denom > EPS) ? clampf((b*f - c*e)/denom, 0.0f, 1.0f) : 0.0f;
            t = (b*s + f) / e;
            if      (t < 0.0f) { t = 0.0f; s = clampf(-c/a,      0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = clampf((b - c)/a, 0.0f, 1.0f); }
        }
    }
    float c1[3] = { p1[0]+d1[0]*s, p1[1]+d1[1]*s, p1[2]+d1[2]*s };
    float c2[3] = { p2[0]+d2[0]*t, p2[1]+d2[1]*t, p2[2]+d2[2]*t };
    float dv[3] = { c1[0]-c2[0], c1[1]-c2[1], c1[2]-c2[2] };
    if (out_s) *out_s = s;
    if (cp) { cp[0]=c1[0]; cp[1]=c1[1]; cp[2]=c1[2]; }
    return dot3(dv, dv);
}

/* World position of a named bone (out[12..14]) + lean. 0 if the name is absent. */
static int bone_world_by_name(void* ent, const char* name, const float lean[3], float p[3])
{
    float m[16];
    if (!p_WorldTag(ent, name, m)) return 0;
    p[0] = m[12] + lean[0];
    p[1] = m[13] + lean[1];
    p[2] = m[14] + lean[2];
    return 1;
}

/* Limb capsules (Biped names) — fill the gaps that sphere-per-bone leaves in
 * thin limbs. Missing names are skipped (G_DObjGetWorldTagMatrix returns 0). */
typedef struct { const char* a; const char* b; float radius; int hitloc; } limbcap_t;
static const limbcap_t LIMB_CAPS[] = {
    { "Bip01 L UpperArm", "Bip01 L Forearm", 4.5f, HL_L_ARM_U },
    { "Bip01 L Forearm",  "Bip01 L Hand",    4.0f, HL_L_ARM_L },
    { "Bip01 R UpperArm", "Bip01 R Forearm", 4.5f, HL_R_ARM_U },
    { "Bip01 R Forearm",  "Bip01 R Hand",    4.0f, HL_R_ARM_L },
    { "Bip01 L Thigh",    "Bip01 L Calf",    6.0f, HL_L_LEG_U },
    { "Bip01 L Calf",     "Bip01 L Foot",    5.0f, HL_L_LEG_L },
    { "Bip01 L Foot",     "Bip01 L Toe0",    4.5f, HL_L_FOOT },  /* foot bone = ankle; toe is ~12u forward */
    { "Bip01 R Thigh",    "Bip01 R Calf",    6.0f, HL_R_LEG_U },
    { "Bip01 R Calf",     "Bip01 R Foot",    5.0f, HL_R_LEG_L },
    { "Bip01 R Foot",     "Bip01 R Toe0",    4.5f, HL_R_FOOT },
};
#define NUM_LIMB_CAPS ((int)(sizeof(LIMB_CAPS)/sizeof(LIMB_CAPS[0])))

/* Lean offset to add to bone positions (same math the eye uses, * g_lean_frac). */
static void compute_lean(void* cl, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    float leanf = *(float*)((char*)cl + PS_LEANF);
    if (leanf == 0.0f) return;
    float yaw = *(float*)((char*)cl + PS_VIEWYAW);
    float off[3] = { 0.0f, 0.0f, 0.0f };
    p_AddLean(off, yaw, leanf, 16.0f, 20.0f);
    out[0] = off[0] * g_lean_frac;
    out[1] = off[1] * g_lean_frac;
    out[2] = off[2] * g_lean_frac;
}

/* Pose the victim, test the bullet segment vs a sphere on every bone.
 * Returns the hitloc of the NEAREST bone hit (by fraction along the bullet),
 * or HL_NONE for no bone. Fills hit_pos with the bone world position. */
static int perbone_test(void* ent, void* cl, const float* start, const float* end,
                        float hit_pos[3])
{
    p_CalcPose(ent);
    int    n   = p_NumBones(ent);
    float* arr = p_MatrixArray(ent);
    if (!arr || n <= 0) return HL_NONE;
    if (n > PB_MAX_BONES) n = PB_MAX_BONES;

    float* org = (float*)((char*)ent + E_ORIGIN);
    float  lean[3];
    compute_lean(cl, lean);

    int   best_hl = HL_NONE;
    float best_t  = 2.0f;

    /* BODY + LIMBS first; HEAD + NECK are tested LAST and WIN (see end of fn). */
    for (int i = 0; i < n; ++i) {
        const float* m = arr + (long)i * 16;          /* 0x40 = 16 floats */
        float w[3] = { m[12] + org[0] + lean[0],
                       m[13] + org[1] + lean[1],
                       m[14] + org[2] + lean[2] };
        int   hl  = hitloc_for_z(m[14]);              /* body zone only (never head/neck) */
        float t;
        if (pt_seg_dist2(w, start, end, &t) <= g_radius*g_radius && t < best_t) {
            best_t  = t;
            best_hl = hl;
            hit_pos[0] = w[0]; hit_pos[1] = w[1]; hit_pos[2] = w[2];
        }
    }

    /* limb capsules (continuous coverage on thin arms/legs where spheres gap) */
    for (int i = 0; i < NUM_LIMB_CAPS; ++i) {
        float A[3], B[3];
        if (!bone_world_by_name(ent, LIMB_CAPS[i].a, lean, A)) continue;
        if (!bone_world_by_name(ent, LIMB_CAPS[i].b, lean, B)) continue;
        float rr = LIMB_CAPS[i].radius * g_cap_scale;
        float t, cp[3];
        if (seg_seg_dist2(start, end, A, B, &t, cp) <= rr*rr && t < best_t) {
            best_t  = t;
            best_hl = LIMB_CAPS[i].hitloc;
            hit_pos[0] = cp[0]; hit_pos[1] = cp[1]; hit_pos[2] = cp[2];
        }
    }

    /* HEAD + NECK LAST — they WIN over any body/limb hit. Their volumes are TIGHT
     * (head = skull-axis CAPSULE r=g_head_rad neck->head + g_head_len; neck = thin
     * sphere r=g_neck_rad), so a bullet inside them genuinely hit the head/neck.
     * BUG FIXED 2026-07-20: testing these BEFORE the body loop let a grazed
     * shoulder/clavicle bone (UpperArm z52-54, classed TORSO_UPPER) at a smaller t
     * OVERRIDE a real head hit -> SMG/pistol headshots (wider cone / off-axis /
     * hip) registered as TORSO_UPPER while precise rifle-ADS shots were fine. The
     * eye-origin bullet ray makes a false head hit on a body shot unrealistic
     * (a chest shot's ray points away from the head), so head/neck win outright. */
    {
        float Np[3];
        const int have_neck = bone_world_by_name(ent, "Bip01 Neck", lean, Np);
        if (have_neck) {
            float t;
            if (pt_seg_dist2(Np, start, end, &t) <= g_neck_rad*g_neck_rad) {
                best_hl = HL_NECK;
                hit_pos[0]=Np[0]; hit_pos[1]=Np[1]; hit_pos[2]=Np[2];
            }
        }
        float Hp[3];
        if (bone_world_by_name(ent, "Bip01 Head", lean, Hp)) {
            float up[3] = { 0.0f, 0.0f, 1.0f };          /* fallback: world up */
            if (have_neck) {
                up[0]=Hp[0]-Np[0]; up[1]=Hp[1]-Np[1]; up[2]=Hp[2]-Np[2];
                float L = sqrtf(dot3(up, up));
                if (L > 1e-3f) { up[0]/=L; up[1]/=L; up[2]/=L; }
                else { up[0]=0.0f; up[1]=0.0f; up[2]=1.0f; }
            }
            float crown[3] = { Hp[0]+up[0]*g_head_len,
                               Hp[1]+up[1]*g_head_len,
                               Hp[2]+up[2]*g_head_len };
            float t, cp[3];
            if (seg_seg_dist2(start, end, Hp, crown, &t, cp) <= g_head_rad*g_head_rad) {
                best_hl = HL_HEAD;
                hit_pos[0]=cp[0]; hit_pos[1]=cp[1]; hit_pos[2]=cp[2];
            }
        }
    }
    return best_hl;
}

/* ===== one-time probe (dump mode): validate the index read + log bones ===== */
static void dump_bones(void* ent)
{
    /* Re-fire once every 2s so the tester can strike a pose (stand, crouch,
     * lean+aim) and get a FRESH probe for each. The NAME OK block + the header
     * entOrigin give named LOCAL coords per pose (local = world - entOrigin). */
    time_t now = time(NULL);
    if (now - g_last_dump < 2) return;
    g_last_dump = now;
    if (p_CalcPose) p_CalcPose(ent);

    int    n   = p_NumBones   ? p_NumBones(ent)   : -1;
    float* arr = p_MatrixArray ? p_MatrixArray(ent) : 0;
    float* org = (float*)((char*)ent + E_ORIGIN);
    printf("%s ==== INDEX probe: NumBones=%d arr=%p entOrigin=(%.0f %.0f %.0f) ====\n",
           TAG, n, (void*)arr, org[0], org[1], org[2]);
    if (arr && n > 0) {
        if (n > 48) n = 48;
        for (int i = 0; i < n; ++i) {
            const float* m = arr + (long)i * 16;
            printf("%s   bone[%2d] local=(%.1f %.1f %.1f) world=(%.0f %.0f %.0f)\n",
                   TAG, i, m[12], m[13], m[14],
                   org[0]+m[12], org[1]+m[13], org[2]+m[14]);
        }
    }
    /* confirm the limb-capsule bone names resolve on this model */
    static const char* NAMES[] = {
        "Bip01 Head", "Bip01 Neck", "Bip01 Spine1", "Bip01 Pelvis", "pelvis",
        "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand",
        "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand",
        "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot", "Bip01 L Toe0",
        "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot", "Bip01 R Toe0"
    };
    for (int i = 0; i < (int)(sizeof(NAMES)/sizeof(NAMES[0])); ++i) {
        float m[16];
        if (p_WorldTag(ent, NAMES[i], m))
            printf("%s   NAME OK: '%s' world=(%.0f %.0f %.0f)\n",
                   TAG, NAMES[i], m[12], m[13], m[14]);
    }
    printf("%s ==== end probe ====\n", TAG);
    fflush(stdout);
}

/* ============================== the hook ============================== */
static void Hook_LocationalTrace(pb_trace_t* tr, const float* start, const float* end,
                                 int passEnt, int mask, int sight)
{
    real_loctrace(tr, start, end, passEnt, mask, sight);   /* BROAD: engine box trace */

    if (g_in_trace) return;

    uint16_t ent = tr->hitEntityNum;
    if (ent >= PB_MAX_CLIENTS) return;                     /* world / non-player */

    char* gent = (char*)(g_base + RVA_g_entities) + (size_t)ent * GENTITY_SIZE;
    void* cl   = *(void**)(gent + E_CLIENT);
    if (!cl) return;

    g_in_trace = 1;
    float hp[3] = { end[0], end[1], end[2] };
    int   hl;
    if (g_mode == 2) { dump_bones(gent); hl = perbone_test(gent, cl, start, end, hp); }
    else             { hl = perbone_test(gent, cl, start, end, hp); }
    g_in_trace = 0;

    if (g_mode == 2) {                                     /* dump: don't override */
        printf("%s hit ent=%d -> hl=%d (dump)\n", TAG, ent, hl);
        fflush(stdout);
        return;
    }

    if (hl != HL_NONE) {                                   /* ACCEPT: bone hit */
        tr->hitLocation = (uint16_t)hl;
        tr->endpos[0] = hp[0]; tr->endpos[1] = hp[1]; tr->endpos[2] = hp[2];
        return;
    }

    /* REJECT: box hit, no bone -> world miss (g_entities[1023].takedamage==0). */
    tr->hitEntityNum = ENTITYNUM_NONE;
    tr->hitLocation  = HL_NONE;
    tr->fraction     = 1.0f;
    tr->endpos[0] = end[0]; tr->endpos[1] = end[1]; tr->endpos[2] = end[2];
    tr->surfaceFlags = 0;
}

/* ============================== install ============================== */
static uintptr_t find_game_base(void)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, GAME_SO_NAME)) continue;
        uintptr_t start = (uintptr_t)strtoul(line, NULL, 16);
        if (start && (base == 0 || start < base)) base = start;
    }
    fclose(f);
    return base;
}

static void resolve_fns(uintptr_t base)
{
    p_CalcPose    = (calcpose_t)(base + RVA_G_DObjCalcPose);
    p_WorldTag    = (worldtag_t)(base + RVA_G_DObjWorldTag);
    p_NumBones    = (numbones_t)(base + RVA_DObjNumBones);
    p_MatrixArray = (matarr_t)  (base + RVA_DObjMatrixArray);
    p_AddLean     = (addlean_t) (base + RVA_AddLeanToPosition);
    (void)p_WorldTag;
}

static void try_install(void)
{
    static int logged = 0;
    uintptr_t base = find_game_base();
    if (!base) return;

    unsigned char* target = (unsigned char*)(base + RVA_LOCTRACE_STUB);
    if (base == g_base && target[0] == 0xE9) return;
    if (memcmp(target, LT_PROLOGUE, sizeof(LT_PROLOGUE)) != 0) {
        if (target[0] != 0xE9 && !logged) {
            printf("%s trap_LocationalTrace prologue mismatch @0x%08lx "
                   "(got %02x %02x %02x) - wrong game build? perbone disabled\n",
                   TAG, (unsigned long)(uintptr_t)target, target[0], target[1], target[2]);
            fflush(stdout);
            logged = 1;
        }
        return;
    }

    resolve_fns(base);
    if (hook_install(&g_lt_hook, (uintptr_t)target,
                     (uintptr_t)Hook_LocationalTrace, LT_PATCHLEN) == 0) {
        real_loctrace = (loctrace_t)g_lt_hook.trampoline;
        g_base   = base;
        g_last_dump = 0;
        logged   = 0;
        printf("%s installed (mode=%s, body=%.1f head=%.1f/len%.1f neck=%.1f leanfrac=%.2f, "
               "game base 0x%08lx) [head capsule + foot->toe]\n",
               TAG, g_mode == 2 ? "dump" : "on", g_radius, g_head_rad, g_head_len,
               g_neck_rad, g_lean_frac, (unsigned long)base);
        fflush(stdout);
    } else {
        printf("%s hook_install failed\n", TAG);
        fflush(stdout);
    }
}

static void* watcher_thread(void* arg)
{
    (void)arg;
    for (;;) { try_install(); usleep(400 * 1000); }
    return NULL;
}

void perbone_hit_init(void)
{
    const char* e = getenv("COD1RELOADED_PERBONE_HIT");
    if (!e || *e == '0' || *e == 'o' || *e == 'f' || *e == 'n') return;
    if (strcmp(e, "dump") == 0 || *e == 'd') g_mode = 2; else g_mode = 1;

    const char* rd = getenv("COD1RELOADED_PERBONE_RADIUS");
    if (rd && *rd) { float v = (float)atof(rd); if (v > 0.0f && v <= 40.0f) g_radius = v; }
    const char* hr = getenv("COD1RELOADED_PERBONE_HEADRAD");
    if (hr && *hr) { float v = (float)atof(hr); if (v > 0.0f && v <= 20.0f) g_head_rad = v; }
    const char* nr = getenv("COD1RELOADED_PERBONE_NECKRAD");
    if (nr && *nr) { float v = (float)atof(nr); if (v > 0.0f && v <= 20.0f) g_neck_rad = v; }
    const char* hl = getenv("COD1RELOADED_PERBONE_HEADLEN");
    if (hl && *hl) { float v = (float)atof(hl); if (v > 0.0f && v <= 30.0f) g_head_len = v; }
    const char* lf = getenv("COD1RELOADED_PERBONE_LEANFRAC");
    if (lf && *lf) { float v = (float)atof(lf); if (v >= 0.0f && v <= 2.0f) g_lean_frac = v; }
    const char* ls = getenv("COD1RELOADED_PERBONE_LIMBSCALE");
    if (ls && *ls) { float v = (float)atof(ls); if (v > 0.0f && v <= 4.0f) g_cap_scale = v; }

    pthread_t tid;
    if (pthread_create(&tid, NULL, watcher_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started (mode=%s)\n", TAG, g_mode == 2 ? "dump" : "on");
    } else {
        printf("%s failed to start watcher\n", TAG);
    }
    fflush(stdout);
}
