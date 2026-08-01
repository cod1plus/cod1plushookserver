/*
 * perbone_hit.c - self-contained per-bone bullet hit refinement (anti "clip").
 *
 * FINE phase layered on top of lean_hitbox.c's AABB shift (the BROAD phase):
 *   - lean_hitbox.c shifts the player's single collision box to the leaned
 *     position so the engine box-trace REGISTERS a hit on a peeking player.
 *   - this module hooks trap_LocationalTrace; when the engine reports a player
 *     box-hit, it poses the victim's skeleton, reads every bone's WORLD position
 *     (index-based, no names), shifts each by the lean SCALED BY ITS HEIGHT (the body
 *     pivots about the feet, so the skull gets the full ~20u and the feet none), and
 *     tests the bullet segment against a sphere on each bone.
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
 *         COD1RELOADED_PERBONE_LEANFRAC (lean multiplier, default 1.0 - the
 *           per-bone height scaling shapes it; lowering this re-breaks headshots),
 *         COD1RELOADED_PERBONE_DEBUG=1 (log the real head miss distance per shot
 *           on a leaning victim - use this instead of guessing at the geometry).
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
/* MUST MATCH mss32/lean_fix.cpp body_shift_* and lean_hitbox.c LEAN_BODY_SHIFT_*. */
#define PB_BODY_SHIFT_SCALE        1.0f
#define PB_BODY_SHIFT_RIGHT_SCALE  2.0f   /* = client body_shift_right_scale (validated) */

#define E_MINS        0x104   /* r.mins vec3 (disasm-confirmed, see lean_hitbox.c) */
#define E_MAXS        0x110   /* r.maxs vec3 */
#define E_CLIENT      0x15c
#define E_ORIGIN      0x138   /* r.currentOrigin vec3 */
#define GENTITY_SIZE  0x31c
#define E_HEALTH      0x238   /* int; the dead keep a client ptr + stale leanf */
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
/* ==== VALIDATED IN GAME 2026-07-31 - do not change without re-running the probe ====
 * The whole head model, as accepted by the players after a full calibration session.
 * Probe: victim holds a lean, immobile; aim at the LEAN-SIDE EDGE of the drawn helmet;
 * read miss0 (bullet distance to the posed skull axis). Covered span = shift +/- rad.
 *
 * DO NOT touch g_head_len to reach the top of the skull: the capsule axis TILTS with
 * the lean, so lengthening it pushes the whole volume sideways and destroys the
 * lateral calibration (tried live, "full bugge"). Use g_head_rad_crouch / g_head_rad
 * for coverage - a radius grows the volume without moving the axis. */
static float g_head_rad  = 5.0f;  /* stand: validated ("binary" hits vs misses at 5.0) */
static float g_head_len  = 8.0f;  /* engine-shaped; see the warning above */
static float g_neck_rad  = 3.0f;  /* neck sphere radius (the neck is thin) */
static float g_cap_scale = 1.0f;  /* limb capsule radius multiplier */
/* Multiplier on the mirrored body-shift offset. DEFAULT 1: compute_lean now
 * reproduces the client's drawn tag_origin shift EXACTLY (stand-only, per-side lf
 * scaling - see the audited comment there), so mirroring it in full is correct.
 * The old default-0 rationale (bracket log favoring miss0) was measured during
 * the window when the client shift was itself removed - superseded. */
static float g_lean_frac = 1.0f;
/* Drawn-vs-posed head offset in UNITS at a half lean, per stance and side - the whole
 * body-shift model, with no K table or factor in between. Measured by aiming at the
 * LEAN-SIDE EDGE of the drawn helmet and reading miss0 (bullet distance to the posed
 * skull axis): stand-left edge 9.5 with a 5.0 radius => the drawn centre sits ~4.5
 * past the posed skull; crouch-left edge 10.5 => ~5.5. The right-side values are
 * provisional until the same probe is run there.
 * Env: COD1RELOADED_PERBONE_SHIFT_SL / _SR / _CL / _CR. */
static float g_shift_sl = 4.7f;   /* stand,  lean left  - VALIDATED */
static float g_shift_sr = 2.0f;   /* stand,  lean right - provisional, never probed */
static float g_shift_cl = 5.7f;   /* crouch, lean left  - VALIDATED */
static float g_shift_cr = 2.5f;   /* crouch, lean right - provisional, never probed */
/* Separate head radius for CROUCH. All of the night's calibration was done standing;
 * crouched, aimed shots at the drawn helmet's edge measured 6.4-7.0 from the skull
 * axis against a 4.5 capsule ("impossible de hit son cote gauche de la tete") while
 * centre shots landed - i.e. the capsule is correctly PLACED but too narrow for the
 * crouch pose. 0 = use the standing radius. */
/* 1 = the body shift fades to zero at ground level (a leaning body pivots on its
 * feet). Head-height bones keep the full calibrated value. */
static int   g_shift_taper = 1;
static float g_head_rad_crouch = 6.0f;   /* VALIDATED: crouch needs the extra width to
                                          * reach the top of the drawn helmet. Stand is
                                          * untouched by this (box height gate). */
/* Second, lean-offset head axis for the tilted-helmet corner. It was a COMPENSATION
 * for the server skull sitting off the drawn one; pose_sync now places the skull
 * correctly, so it only adds a generous lobe on the lean side (measured: granted a
 * headshot at 5.6 when the capsule radius is 5.0). DEFAULT OFF; re-enable with
 * COD1RELOADED_PERBONE_CORNER=1 only if pose_sync is disabled. */
static int   g_corner  = 0;
/* Fraction of the ENGINE eye lean (G_AddLean, ~20u) to add on top of the pose.
 * DEFAULT 0, and that is not a disable switch - it is the correct value.
 * G_DObjCalcPose already bends the skeleton sideways when the player leans
 * (measured live: the head bone sits 9 to 14 units off-centre at leanf 0.5 with
 * nothing added, where a neutral pose has it at 0). The 16/20 constants are the
 * offset of the EYE and of the damage points, not of the model. Adding them put
 * the head at ~29 instead of ~14 and every head shot missed by 12 to 13 units -
 * that was the "impossible to shoot a leaning head" bug. Raise only if a dump
 * ever proves the server pose does NOT bend. */
static float g_eye_lean  = 0.0f;
static int    g_debug    = 0;   /* COD1RELOADED_PERBONE_DEBUG=1 -> log the head miss distance */
static int    g_strict   = 0;   /* 1 = box hit with no bone is ALWAYS discarded */
static int    g_swing_sync = 1; /* v2: force server swing yaw to view for STANDING
                                 * leaners, save/restore around each trace (v1 wrote
                                 * persistently+blanket -> neck deformation + rotated
                                 * a hidden croucher out of cover; both fixed by
                                 * stand-only + restore). SWINGSYNC=0 disables. */
/* Margin band for the no-bone case on a LEANING victim (the widened box holds both his
 * real bent body AND the empty upright silhouette). Bullet within this distance of some
 * bone surface = plausibly the body (capsule gap, pose error) -> keep as generic damage.
 * Farther than this from EVERY bone = the vacated silhouette -> delete. Binary versions
 * of this both failed live: always-delete ate legitimate bullets ("3 chargeurs vides"),
 * always-keep resurrected the standing ghost ("sa hitbox agit comme s'il etait stand"). */
static float g_reject_margin = 3.0f;   /* VALIDATED: 5.0 granted hits in open air next
                                        * to a leaner, 1.5 deleted legitimate grazes. */
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

/* Scale the lean offset by how high the bone sits.
 *
 * THE BODY PIVOTS ABOUT THE FEET: the head swings the full ~20 units sideways, the
 * pelvis about half, the feet not at all. Adding one uniform offset to every bone (what
 * this used to do) cannot be right for any of them, and with the old 0.5 factor the head
 * sphere sat 10 units out while the real head was at 20 - so a bullet aimed at the head
 * you can see missed the sphere and no headshot registered. That is the reported "can't
 * shoot the head when he leans".
 *
 * ref_z is the topmost bone (the skull), so the head always receives exactly the full
 * offset and the scale adapts by itself to standing, crouched and prone. That matters:
 * G_AddLean's 20-unit lateral constant does NOT vary with stance (verified in the
 * disassembly), only the eye height does, so normalising by a fixed eye height would
 * under-shift a crouched head. */
static void lean_at_height(const float lean[3], float local_z, float ref_z, float out[3])
{
    /* TAPER BY HEIGHT. A leaning body pivots about the feet: the head swings the
     * measured amount, the legs barely move. Applying the shift uniformly put the leg
     * spheres several units out in open air on the lean side - "je tire a cote de ses
     * jambes et ca le touche quand meme". ref_z is passed as the HEAD's local height,
     * so a bone at head height receives exactly the calibrated shift (the validated
     * numbers are unchanged) and a bone at the ground receives none.
     * COD1RELOADED_PERBONE_SHIFT_TAPER=0 restores the old uniform behaviour. */
    if (!g_shift_taper) {
        out[0] = lean[0]; out[1] = lean[1]; out[2] = lean[2];
        return;
    }
    float f = (ref_z > 1.0f) ? (local_z / ref_z) : 1.0f;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    out[0] = lean[0] * f;
    out[1] = lean[1] * f;
    out[2] = lean[2] * f;
}

/* World position of a named bone (out[12..14]) + height-scaled lean. 0 if absent. */
static int bone_world_by_name(void* ent, const char* name, const float lean[3],
                              float org_z, float ref_z, float p[3])
{
    float m[16];
    if (!p_WorldTag(ent, name, m)) return 0;
    float sc[3];
    lean_at_height(lean, m[14] - org_z, ref_z, sc);   /* m is world -> local height */
    p[0] = m[12] + sc[0];
    p[1] = m[13] + sc[1];
    p[2] = m[14] + sc[2];
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

/* What must be ADDED to the posed skeleton so the bones land where the client draws them.
 *
 * The pose itself already carries the lean: G_DObjCalcPose bends the skeleton, so the
 * bone matrices come back with the torso and head displaced sideways - and the bracket
 * measurement showed that is ALREADY where the client draws them (adding the client's
 * tag_origin body shift made every aimed shot pass farther, hence g_lean_frac 0).
 *
 * The engine eye lean (16/20) is deliberately NOT added either: see g_eye_lean. */
static void compute_lean(void* ent, void* cl, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    float leanf = *(float*)((char*)cl + PS_LEANF);
    if (leanf == 0.0f) return;
    float yaw = *(float*)((char*)cl + PS_VIEWYAW);

    /* DIRECTION READ FROM THE POSE ITSELF, not from p_AddLean. Measured live: with
     * the p_AddLean direction the mirror pushed the capsule the WRONG WAY on a left
     * lean (aimed helmet shots went from 3.3u off to unhittable) - its sign
     * convention is not the client body_shift's, which the client source itself
     * flagged as unverified. The posed skeleton already leans, so the vector from
     * the entity origin to the posed head IS the lean direction, per side, with no
     * convention to guess. */
    float off[3] = { 0.0f, 0.0f, 0.0f };
    p_AddLean(off, yaw, leanf, 16.0f, 20.0f);
    float mag = sqrtf(off[0]*off[0] + off[1]*off[1]);

    float ux = 0.0f, uy = 0.0f;
    {
        float hm[16];
        float* org = (float*)((char*)ent + E_ORIGIN);
        if (p_WorldTag(ent, "Bip01 Head", hm)) {
            float dx = hm[12] - org[0], dy = hm[13] - org[1];
            float dl = sqrtf(dx*dx + dy*dy);
            if (dl > 0.5f) { ux = dx/dl; uy = dy/dl; }
        }
        if (ux == 0.0f && uy == 0.0f) {          /* pose unavailable: fall back */
            if (mag < 0.01f) return;
            ux = off[0]/mag; uy = off[1]/mag;
        }
    }

    /* MIRROR OF THE CLIENT'S DRAWN BODY SHIFT (cod2x table, animation.cpp:186-199).
     * Magnitude = K x factor x |leanf| along the pose's own lean direction; the
     * factors are env-tunable and were measured by aiming at the drawn helmet and
     * reading miss0/miss/miss2 (see g_shift_l / g_shift_r).
     *
     * An earlier audit concluded the client zeroes its lean controllers when
     * crouched and the crouch rows were deleted; live measurement contradicted it
     * (crouched left-lean helmet shots sat 6.4-7.0 off the posed skull, matching
     * crouch-left K 12.5 x .5 = 6.25), so the stance-aware table is back. */
    float* mins = (float*)((char*)ent + E_MINS);
    float* maxs = (float*)((char*)ent + E_MAXS);
    float  body = 0.0f;
    {
        /* DIRECT UNITS - one number per stance and side: how far the DRAWN head sits
         * past the posed skull, in units, at a half lean. No K table, no factor, no
         * scale: every earlier formula routed through cod2x's K rows times a factor
         * times a multiplier, so no adjustment was predictable and each fix broke
         * another stance. Measure with the bracket columns, set the number, done.
         * Scales linearly with |leanf| (0.5 = the usual held lean). */
        const int is_crouch = (maxs[2] - mins[2]) < 55.0f;
        const int is_left   = (leanf < 0.0f);
        const float u = is_crouch ? (is_left ? g_shift_cl : g_shift_cr)
                                  : (is_left ? g_shift_sl : g_shift_sr);
        body = u * (fabsf(leanf) / 0.5f);
    }

    const float total = body + g_eye_lean * mag;

    out[0] = ux * total * g_lean_frac;
    out[1] = uy * total * g_lean_frac;
    out[2] = 0.0f;
}

/* Pose the victim, test the bullet segment vs a sphere on every bone.
 * Returns the hitloc of the NEAREST bone hit (by fraction along the bullet),
 * or HL_NONE for no bone. Fills hit_pos with the bone world position. */
static int perbone_test(void* ent, void* cl, const float* start, const float* end,
                        float hit_pos[3], float* min_clear)
{
    *min_clear = 1e9f;   /* distance from the bullet to the nearest bone SURFACE */
    p_CalcPose(ent);
    int    n   = p_NumBones(ent);
    float* arr = p_MatrixArray(ent);
    if (!arr || n <= 0) return HL_NONE;
    if (n > PB_MAX_BONES) n = PB_MAX_BONES;

    float* org = (float*)((char*)ent + E_ORIGIN);
    float  lean[3];
    compute_lean(ent, cl, lean);

    /* Height reference for the lean scaling: the topmost bone, i.e. the skull. Taken
     * from the posed skeleton so it follows the stance with no hardcoded eye height. */
    float ref_z = 0.0f;
    for (int i = 0; i < n; ++i) {
        float z = arr[(long)i * 16 + 14];
        if (z > ref_z) ref_z = z;
    }
    /* Reference height for the shift taper = the HEAD bone, not the topmost bone, so
     * a head-height bone gets exactly the calibrated shift (helmet bones above it are
     * clamped to 1.0) while legs fade to zero. */
    {
        float hm[16];
        float* org0 = (float*)((char*)ent + E_ORIGIN);
        if (p_WorldTag(ent, "Bip01 Head", hm)) {
            float hz = hm[14] - org0[2];
            if (hz > 1.0f) ref_z = hz;
        }
    }

    int   best_hl = HL_NONE;
    float best_t  = 2.0f;

    /* BODY + LIMBS first; HEAD + NECK are tested LAST and WIN (see end of fn).
     * Only the CHARACTER rig (~80 bones, character_soviet_coat3): the tail of the
     * matrix array is the carried WEAPON's bones, which sit at chest/shoulder
     * height and were classified TORSO by hitloc_for_z - shooting the air where
     * the gun points registered as body damage. */
    int nbody = n > 80 ? 80 : n;
    for (int i = 0; i < nbody; ++i) {
        const float* m = arr + (long)i * 16;          /* 0x40 = 16 floats */
        float sc[3];
        lean_at_height(lean, m[14], ref_z, sc);       /* m[14] already local */
        float w[3] = { m[12] + org[0] + sc[0],
                       m[13] + org[1] + sc[1],
                       m[14] + org[2] + sc[2] };
        /* HEAD/HELMET TERRITORY IS OFF-LIMITS to the generic loop. The skull, face
         * and helmet bones live above local z~56; sphere-testing them classified
         * hits as TORSO (hitloc_for_z caps at torso_upper) and created a diffuse
         * hittable volume above the shoulders/head - measured live: granted "torso"
         * impacts at z 62-68, above the drawn helmet. Only the NAMED skull capsule
         * (tight, r=g_head_rad) and neck sphere may claim that region. */
        if (m[14] >= 56.0f) continue;
        int   hl  = hitloc_for_z(m[14]);              /* body zone only (never head/neck) */
        /* Shoulder band (clavicles, z>=48): full-radius spheres bulged into the
         * visible head/shoulder GAP. 3.5 still protruded ~1u ("hit epaule alors
         * que je suis pas dessus" on gap probes that correctly missed the head);
         * 3.0 hugs the drawn deltoid. The arm capsule (trimmed) covers the outer
         * shoulder mass below. */
        float rr = (m[14] >= 48.0f) ? g_radius - 2.0f : g_radius;
        float t;
        float d2 = pt_seg_dist2(w, start, end, &t);
        {   float c = sqrtf(d2) - rr;
            if (c < *min_clear) *min_clear = c; }
        if (d2 <= rr*rr && t < best_t) {
            best_t  = t;
            best_hl = hl;
            /* impact point ON the bullet path (the bone centre is up to g_radius off
             * the ray, which desyncs endpos from the fraction we later write) */
            hit_pos[0] = start[0] + t*(end[0]-start[0]);
            hit_pos[1] = start[1] + t*(end[1]-start[1]);
            hit_pos[2] = start[2] + t*(end[2]-start[2]);
        }
    }

    /* limb capsules (continuous coverage on thin arms/legs where spheres gap) */
    for (int i = 0; i < NUM_LIMB_CAPS; ++i) {
        float A[3], B[3];
        if (!bone_world_by_name(ent, LIMB_CAPS[i].a, lean, org[2], ref_z, A)) continue;
        if (!bone_world_by_name(ent, LIMB_CAPS[i].b, lean, org[2], ref_z, B)) continue;
        /* UPPER-ARM capsules: start 30% below the shoulder joint. At full radius the
         * capsule top reached z~58 and filled the visible shoulder/head gap ("je vise
         * entre l'epaule et la tete, colle au corps, ca touche"). The joint itself
         * stays covered by the slim z>=48 band sphere (r3.5) of the body loop. */
        if (LIMB_CAPS[i].hitloc == HL_R_ARM_U || LIMB_CAPS[i].hitloc == HL_L_ARM_U) {
            A[0] += 0.30f * (B[0] - A[0]);
            A[1] += 0.30f * (B[1] - A[1]);
            A[2] += 0.30f * (B[2] - A[2]);
        }
        float rr = LIMB_CAPS[i].radius * g_cap_scale;
        float t, cp[3];
        float d2 = seg_seg_dist2(start, end, A, B, &t, cp);
        {   float c = sqrtf(d2) - rr;
            if (c < *min_clear) *min_clear = c; }
        if (d2 <= rr*rr && t < best_t) {
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
        const int have_neck = bone_world_by_name(ent, "Bip01 Neck", lean, org[2], ref_z, Np);
        if (have_neck) {
            float t;
            float nd2 = pt_seg_dist2(Np, start, end, &t);
            {   float c = sqrtf(nd2) - g_neck_rad;
                if (c < *min_clear) *min_clear = c; }
            if (nd2 <= g_neck_rad*g_neck_rad) {
                best_hl = HL_NECK;
                /* impact point ON the bullet path, not the bone centre */
                hit_pos[0] = start[0] + t*(end[0]-start[0]);
                hit_pos[1] = start[1] + t*(end[1]-start[1]);
                hit_pos[2] = start[2] + t*(end[2]-start[2]);
            }
        }
        float Hp[3];
        if (bone_world_by_name(ent, "Bip01 Head", lean, org[2], ref_z, Hp)) {
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
            float d2 = seg_seg_dist2(start, end, Hp, crown, &t, cp);
            /* NOTE - two attempts at removing the capsule's lower hemisphere (the
             * "skirt" that reaches a radius below the head bone, into the visible
             * head/shoulder gap) were both WORSE and are recorded here so they are
             * not retried blind:
             *   1. raising the axis base by one radius -> the volume becomes a cone
             *      tip AT head height; aimed helmet shots missed by 7u.
             *   2. rejecting impacts below the head bone -> `cp` is the closest point
             *      on the BULLET, not on the skull axis, so a level shot from eye
             *      height sits ~1u above the threshold and any downhill/crouched
             *      shooter is rejected: heads became unhittable on BOTH sides.
             * A correct version must use the closest point on the AXIS (c2 in
             * seg_seg_dist2, currently not returned) or taper the radius along it.
             * Until then the skirt stays: a slightly generous gap is far cheaper
             * than unhittable heads. */
            /* crouch gets its own radius: the standing calibration does not carry
             * over to the crouch pose (see g_head_rad_crouch). */
            float hr = g_head_rad;
            if (g_head_rad_crouch > 0.0f) {
                float* mns2 = (float*)((char*)ent + E_MINS);
                float* mxs2 = (float*)((char*)ent + E_MAXS);
                if (mxs2[2] - mns2[2] < 55.0f) hr = g_head_rad_crouch;
            }
            {   float c = sqrtf(d2) - hr;
                if (c < *min_clear) *min_clear = c; }
            if (d2 <= hr*hr) {
                best_hl = HL_HEAD;
                hit_pos[0]=cp[0]; hit_pos[1]=cp[1]; hit_pos[2]=cp[2];
            }

            /* Diagnostic for the "cannot hit a leaning head" report. Three fixes reasoned
             * from a model of the geometry did not move it, so log the real numbers: how
             * far the bullet actually passed from the skull axis, where the head was
             * placed, and what the shot was classified as.
             * COD1RELOADED_PERBONE_DEBUG=1, one line per second, leaning victims only. */
            if (g_debug) {
                float leanf = *(float*)((char*)cl + PS_LEANF);
                if (leanf != 0.0f) {
                    static time_t last_dbg = 0;
                    time_t now = time(NULL);
                    if (now != last_dbg) {
                        last_dbg = now;
                        /* BRACKET THE SHIFT. A single miss distance cannot separate a
                         * misplaced head from a shooter who was simply off target, and
                         * the tester said as much ("jsuis meme pas sur toi defois"). So
                         * measure the SAME shot against the head placed three ways: no
                         * shift at all, the current shift, and twice it. Over a handful
                         * of shots the systematically smallest column is the right
                         * magnitude, and aim error averages out of the comparison
                         * because all three see the identical bullet. */
                        float tt, cp2[3], A[3], B[3];
                        A[0]=Hp[0]-lean[0]; A[1]=Hp[1]-lean[1]; A[2]=Hp[2]-lean[2];
                        B[0]=crown[0]-lean[0]; B[1]=crown[1]-lean[1]; B[2]=crown[2]-lean[2];
                        float m0 = sqrtf(seg_seg_dist2(start, end, A, B, &tt, cp2));
                        A[0]=Hp[0]+lean[0]; A[1]=Hp[1]+lean[1]; A[2]=Hp[2]+lean[2];
                        B[0]=crown[0]+lean[0]; B[1]=crown[1]+lean[1]; B[2]=crown[2]+lean[2];
                        float m2 = sqrtf(seg_seg_dist2(start, end, A, B, &tt, cp2));
                        printf("%s DBG leanf=%.2f leanoff=(%.1f %.1f) ref_z=%.1f "
                               "head=(%.1f %.1f %.1f) org=(%.1f %.1f %.1f) "
                               "miss0=%.1f miss=%.1f miss2=%.1f rad=%.1f -> hitloc=%d\n",
                               TAG, leanf, lean[0], lean[1], ref_z,
                               Hp[0], Hp[1], Hp[2], org[0], org[1], org[2],
                               m0, sqrtf(d2), m2, g_head_rad, best_hl);
                        fflush(stdout);
                    }
                }
            }

            /* HELMET OUTER CORNER. When leaning, the drawn helmet TILTS with the
             * skull; a capsule around the bone axis leaves the outer-top CORNER
             * (the side the player leans toward) uncovered - reported three times
             * across different radii/mirrors as "his lean-side of the head is
             * unhittable", i.e. structural, not tuning. Second parallel axis,
             * offset 2.5u along the lean direction, hugs that outer half. */
            if (g_corner && best_hl != HL_HEAD) {
                float lf2 = *(float*)((char*)cl + PS_LEANF);
                if (lf2 != 0.0f) {
                    float od[3] = { 0.0f, 0.0f, 0.0f };
                    p_AddLean(od, *(float*)((char*)cl + PS_VIEWYAW), lf2, 16.0f, 20.0f);
                    float mg2 = sqrtf(od[0]*od[0] + od[1]*od[1]);
                    if (mg2 > 0.01f) {
                        const float ox = od[0]/mg2*2.5f, oy = od[1]/mg2*2.5f;
                        /* UPPER HALF only: the tilted-helmet corner lives at crown
                         * height; starting this axis at the head bone reached down
                         * into the head/shoulder gap on the lean side and granted
                         * gap hits ("je tire entre epaule et tete, ca hit"). */
                        float A2[3] = { (Hp[0]+crown[0])*0.5f + ox,
                                        (Hp[1]+crown[1])*0.5f + oy,
                                        (Hp[2]+crown[2])*0.5f };
                        float B2[3] = { crown[0]+ox, crown[1]+oy, crown[2] };
                        float t2, cpo[3];
                        float d2o = seg_seg_dist2(start, end, A2, B2, &t2, cpo);
                        {   float c = sqrtf(d2o) - g_head_rad;
                            if (c < *min_clear) *min_clear = c; }
                        if (d2o <= g_head_rad*g_head_rad) {
                            best_hl = HL_HEAD;
                            hit_pos[0]=cpo[0]; hit_pos[1]=cpo[1]; hit_pos[2]=cpo[2];
                        }
                    }
                }
            }

            /* UNION with the UNSHIFTED head. Measured (bracket log): aimed shots
             * cluster bimodally - half pass 0-2u from the RAW posed head, half
             * ~1u from the head shifted ~10u out - because the client's drawn
             * torso tracks the view exactly (swing_fix: tolerance 0, speed 1.0)
             * while the server's swing keeps the vanilla dead zone, so the drawn
             * head sits either ON the pose or up to ~10u around the lean arc.
             * One offset can never cover both modes; testing BOTH candidate
             * skulls does, exactly. The real fix (patch the server's swing
             * constants to match the client, cod2x-style) is the next milestone. */
            if (best_hl != HL_HEAD &&
                lean[0]*lean[0] + lean[1]*lean[1] > 0.01f) {
                float zero[3] = { 0.0f, 0.0f, 0.0f };
                float Hp0[3], Np0[3];
                const int have_n0 =
                    bone_world_by_name(ent, "Bip01 Neck", zero, org[2], ref_z, Np0);
                if (bone_world_by_name(ent, "Bip01 Head", zero, org[2], ref_z, Hp0)) {
                    float up0[3] = { 0.0f, 0.0f, 1.0f };
                    if (have_n0) {
                        up0[0]=Hp0[0]-Np0[0]; up0[1]=Hp0[1]-Np0[1]; up0[2]=Hp0[2]-Np0[2];
                        float L = sqrtf(dot3(up0, up0));
                        if (L > 1e-3f) { up0[0]/=L; up0[1]/=L; up0[2]/=L; }
                        else { up0[0]=0.0f; up0[1]=0.0f; up0[2]=1.0f; }
                    }
                    float crown0[3] = { Hp0[0]+up0[0]*g_head_len,
                                        Hp0[1]+up0[1]*g_head_len,
                                        Hp0[2]+up0[2]*g_head_len };
                    /* TIGHTER radius on the raw candidate: truly aimed shots at it
                     * measured 0.4-2.1u; at the full 6.0 it was promoting TORSO
                     * grazes (bullet 4.6-5.5 from the raw skull while inside the
                     * chest) to headshots. */
                    float r0 = g_head_rad - 2.0f;
                    if (r0 < 3.0f) r0 = 3.0f;
                    float t0, cp0[3];
                    float d20 = seg_seg_dist2(start, end, Hp0, crown0, &t0, cp0);
                    {   float c = sqrtf(d20) - r0;
                        if (c < *min_clear) *min_clear = c; }
                    if (d20 <= r0*r0) {
                        best_hl = HL_HEAD;
                        hit_pos[0]=cp0[0]; hit_pos[1]=cp0[1]; hit_pos[2]=cp0[2];
                    }
                }
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

/* ==================== swing sync v2 (save/restore) ==================== */
/* The modded client draws the torso/legs LOCKED to the view (swing_fix tol 0,
 * speed 1.0); the server's swing state lags in its vanilla 40-deg dead zone, so a
 * leaning victim's drawn head sat 0-10u around the lean arc from the posed one
 * (the measured bimodal miss clusters). Force the server's swing yaw to the view
 * JUST for the pose used by this trace, then RESTORE - v1 wrote persistently and
 * blanket, which leaked into the networked pose (neck deformation) and rotated a
 * hidden crouch-leaning victim's skeleton out of cover. v2: STAND only (crouch
 * controllers are zeroed on the client, so crouch is already consistent), and
 * nothing survives the hook. clientinfo = *(GOT bgs @+0x89ab4) + 0x9b6ec +
 * cn*0x4b0 (0x9b6ec + 64*0x4b0 == sizeof(bgs) exactly); torso.yawAngle +0x3b0,
 * legs.yawAngle +0x380 - same bg struct offsets as the client. */
#define RVA_GOT_BGS       0x89ab4
#define BGS_CLIENTINFO    0x9b6ec
#define CI_STRIDE         0x4b0
#define CI_LEGS_YAW       0x380
#define CI_TORSO_YAW      0x3b0

typedef struct { int used; float torso, legs; } sync_save_t;
static sync_save_t g_sync_saved[PB_MAX_CLIENTS];

static char* ci_of(int i)
{
    char* bgs = *(char**)(g_base + RVA_GOT_BGS);
    if (!bgs) return NULL;
    return bgs + BGS_CLIENTINFO + (size_t)i * CI_STRIDE;
}

/* Fresh-pose every leaning player (the engine never poses in normal play - the
 * per-frame G_DObjCalcPose is gated behind the debug dvar g_debugLocDamage,
 * je @0x3b2b5), with the swing yaw forced to the view for STANDING leaners. */
static void sync_force_and_pose(void)
{
    for (int i = 0; i < PB_MAX_CLIENTS; ++i) {
        char* ge = (char*)(g_base + RVA_g_entities) + (size_t)i * GENTITY_SIZE;
        void* c  = *(void**)(ge + E_CLIENT);
        if (!c) continue;
        if (*(int*)(ge + E_HEALTH) <= 0) continue;       /* never pose corpses */
        if (*(float*)((char*)c + PS_LEANF) == 0.0f) continue;
        if (g_swing_sync) {
            float* mns = (float*)(ge + E_MINS);
            float* mxs = (float*)(ge + E_MAXS);
            if (mxs[2] - mns[2] >= 55.0f) {              /* STAND only */
                char* ci = ci_of(i);
                if (ci && !g_sync_saved[i].used) {
                    g_sync_saved[i].torso = *(float*)(ci + CI_TORSO_YAW);
                    g_sync_saved[i].legs  = *(float*)(ci + CI_LEGS_YAW);
                    g_sync_saved[i].used  = 1;
                    float vy = *(float*)((char*)c + PS_VIEWYAW);
                    *(float*)(ci + CI_TORSO_YAW) = vy;
                    *(float*)(ci + CI_LEGS_YAW)  = vy;
                }
            }
        }
        p_CalcPose(ge);
    }
}

static void sync_restore(void)
{
    for (int i = 0; i < PB_MAX_CLIENTS; ++i) {
        if (!g_sync_saved[i].used) continue;
        g_sync_saved[i].used = 0;
        char* ci = ci_of(i);
        if (!ci) continue;
        *(float*)(ci + CI_TORSO_YAW) = g_sync_saved[i].torso;
        *(float*)(ci + CI_LEGS_YAW)  = g_sync_saved[i].legs;
    }
}

/* ============================== the hook ============================== */
static void hook_lt_body(pb_trace_t* tr, const float* start, const float* end,
                                 int passEnt, int mask, int sight)
{
    /* Only act on REAL bullets. The game calls trap_LocationalTrace every frame
     * for sight/crosshair probes too; treating those as shots produced a storm of
     * granted hits with nobody firing. The flag is maintained by antilag.c's
     * always-installed Bullet_Fire hook. */
    extern int g_bullet_in_flight;
    const int is_bullet = (g_bullet_in_flight > 0);

    if (!g_in_trace && is_bullet)
        sync_force_and_pose();

    real_loctrace(tr, start, end, passEnt, mask, sight);   /* engine per-part trace */

    if (g_in_trace) return;
    if (!is_bullet) return;   /* sight/crosshair probes: engine answer untouched */
    if (g_mode == 3) {
        /* pose-only: the engine's answer IS the answer. With DEBUG, expose it - all
         * night we only ever logged perbone's OWN verdicts, never the engine's. This
         * line is the ground truth: what the per-part clipper says about a bullet at a
         * leaning player, with the skeleton freshly posed. */
        if (g_debug) {
            /* Log EVERY shot that passes near a leaning player - including total
             * misses. The first instrument only printed when the trace HIT the
             * player, which made the interesting shots (aimed at the drawn head,
             * engine says world-miss) invisible. Distance is bullet-ray to the
             * freshly-posed head bone; ehit=1023 means the engine hit nothing. */
            static time_t last_eng = 0;
            time_t now = time(NULL);
            if (now != last_eng) {
                float best = 1e9f, bh[3] = {0,0,0}, blf = 0.0f, bor[2] = {0,0};
                int   bent = -1;
                for (int i = 0; i < PB_MAX_CLIENTS; ++i) {
                    char* ge = (char*)(g_base + RVA_g_entities) + (size_t)i * GENTITY_SIZE;
                    void* c  = *(void**)(ge + E_CLIENT);
                    if (!c) continue;
                    float lf = *(float*)((char*)c + PS_LEANF);
                    if (lf == 0.0f) continue;
                    float hm[16];
                    if (!p_WorldTag(ge, "Bip01 Head", hm)) continue;
                    float t;
                    float d = sqrtf(pt_seg_dist2(&hm[12], start, end, &t));
                    if (d < best) {
                        best = d; bent = i; blf = lf;
                        bh[0]=hm[12]; bh[1]=hm[13]; bh[2]=hm[14];
                        float* o = (float*)(ge + E_ORIGIN);
                        bor[0]=o[0]; bor[1]=o[1];
                    }
                }
                if (bent >= 0 && best < 40.0f) {
                    last_eng = now;
                    printf("%s ENG leanf=%.2f target=%d ehit=%d hitloc=%d "
                           "endpos=(%.1f %.1f %.1f) head=(%.1f %.1f %.1f) "
                           "org=(%.1f %.1f) headmiss=%.1f\n",
                           TAG, blf, bent, (int)tr->hitEntityNum, (int)tr->hitLocation,
                           tr->endpos[0], tr->endpos[1], tr->endpos[2],
                           bh[0], bh[1], bh[2], bor[0], bor[1], best);
                    fflush(stdout);
                }
            }
        }
        return;
    }

    /* Point traces (start==end) are existence/visibility probes, not bullets - there is
     * no ray to refine against and the capsule math degenerates. Leave them alone. */
    {
        float dx = end[0]-start[0], dy = end[1]-start[1], dz = end[2]-start[2];
        if (dx*dx + dy*dy + dz*dz < 1.0f) return;
    }

    uint16_t ent = tr->hitEntityNum;
    if (ent >= PB_MAX_CLIENTS) {
        /* The engine MISSED every player. Measured live (ENG lines): bullets pass
         * 1-2u from a leaning player's posed SKULL and sail through to the wall
         * behind (ehit=1022 = ENTITYNUM_WORLD) - the engine's locational path
         * simply drops leaning players most of the time. That is the 20-year
         * "see-but-can't-hit" lean, finally measured. GRANT the hit ourselves:
         * test the bullet - only up to the engine's own stop point, so walls and
         * cover still protect - against the posed skeleton of every LEANING
         * player, and rewrite the trace to the nearest bone hit. Non-leaning
         * players are untouched: the engine registers them fine. */
        if (g_mode == 2) return;                           /* dump: observe only */
        float seg_end[3] = { tr->endpos[0], tr->endpos[1], tr->endpos[2] };
        float bestf = 2.0f, besthp[3] = {0,0,0};
        int   besti = -1, besthl = HL_NONE;
        g_in_trace = 1;
        for (int i = 0; i < PB_MAX_CLIENTS; ++i) {
            /* NEVER the shooter: the muzzle sits beside his own posed skull when
             * he leans, so without this his own head is the nearest "hit" at
             * fraction 0 - a leaning shooter instakilled HIMSELF (the frac=0.00
             * GRANT storm). The engine excludes him via passEnt; so must we. */
            if (i == passEnt) continue;
            char* ge = (char*)(g_base + RVA_g_entities) + (size_t)i * GENTITY_SIZE;
            void* c  = *(void**)(ge + E_CLIENT);
            if (!c) continue;
            if (*(int*)(ge + E_HEALTH) <= 0) continue;   /* the dead grant nothing */
            if (*(float*)((char*)c + PS_LEANF) == 0.0f) continue;
            /* PRONE: skip - all bones lie within ~6u of the ground, so any round
             * skimming the body would "graze" the head capsule and everything
             * became a headshot. The grant exists for the stand/crouch lean peek;
             * prone stays with the engine. */
            {
                float* mns = (float*)(ge + E_MINS);
                float* mxs = (float*)(ge + E_MAXS);
                if (mxs[2] - mns[2] < 30.0f) continue;
            }
            float hp2[3] = { seg_end[0], seg_end[1], seg_end[2] };
            float clr = 1e9f;
            int hl2 = perbone_test(ge, c, start, seg_end, hp2, &clr);
            if (hl2 == HL_NONE) continue;
            /* order and report by fraction along the ORIGINAL ray */
            float sd[3] = { end[0]-start[0], end[1]-start[1], end[2]-start[2] };
            float L2 = sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2];
            float f = (L2 > 1.0f)
                ? ((hp2[0]-start[0])*sd[0] + (hp2[1]-start[1])*sd[1]
                 + (hp2[2]-start[2])*sd[2]) / L2 : 0.0f;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            if (f < bestf) {
                bestf = f; besti = i; besthl = hl2;
                besthp[0]=hp2[0]; besthp[1]=hp2[1]; besthp[2]=hp2[2];
            }
        }
        g_in_trace = 0;
        if (besti >= 0) {
            tr->hitEntityNum = (uint16_t)besti;
            tr->hitLocation  = (uint16_t)besthl;
            tr->fraction     = bestf;
            tr->endpos[0]=besthp[0]; tr->endpos[1]=besthp[1]; tr->endpos[2]=besthp[2];
            if (g_debug) {
                printf("%s GRANT ent=%d hitloc=%d frac=%.2f endpos=(%.1f %.1f %.1f) "
                       "[engine said world-miss]\n",
                       TAG, besti, besthl, bestf, besthp[0], besthp[1], besthp[2]);
                fflush(stdout);
            }
        }
        return;
    }

    char* gent = (char*)(g_base + RVA_g_entities) + (size_t)ent * GENTITY_SIZE;
    void* cl   = *(void**)(gent + E_CLIENT);
    if (!cl) return;

    /* NON-LEANING victim: the engine's per-part verdict is ground truth - it can
     * emit HELMET(1) and GUN(18), which our capsule model cannot, so overriding it
     * promoted helmet hits to full headshot damage and gun blocks to body damage.
     * Refinement exists for LEANERS only (audit HIGH-2). */
    if (*(float*)((char*)cl + PS_LEANF) == 0.0f) return;

    g_in_trace = 1;
    float hp[3] = { end[0], end[1], end[2] };
    float clear = 1e9f;
    int   hl;
    if (g_mode == 2) { dump_bones(gent); hl = perbone_test(gent, cl, start, end, hp, &clear); }
    else             { hl = perbone_test(gent, cl, start, end, hp, &clear); }
    g_in_trace = 0;

    if (g_mode == 2) {                                     /* dump: don't override */
        printf("%s hit ent=%d -> hl=%d (dump)\n", TAG, ent, hl);
        fflush(stdout);
        return;
    }

    if (hl != HL_NONE) {                                   /* ACCEPT: bone hit */
        tr->hitLocation = (uint16_t)hl;
        tr->endpos[0] = hp[0]; tr->endpos[1] = hp[1]; tr->endpos[2] = hp[2];
        /* Keep fraction consistent with the endpos we just wrote: downstream code
         * (impact FX, penetration, distance falloff) derives the hit point from
         * fraction*(end-start), and handing it the BOX fraction with a bone endpos
         * feeds it two different hit points for the same bullet. */
        {
            float sd[3] = { end[0]-start[0], end[1]-start[1], end[2]-start[2] };
            float L2 = sd[0]*sd[0] + sd[1]*sd[1] + sd[2]*sd[2];
            if (L2 > 1.0f) {
                float f = ((hp[0]-start[0])*sd[0] + (hp[1]-start[1])*sd[1]
                         + (hp[2]-start[2])*sd[2]) / L2;
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                tr->fraction = f;
            }
        }
        return;
    }

    /* Box hit but no bone: decide by DISTANCE, not by a binary rule. Both binary
     * versions failed live. Always-delete (the original anti-clip) silently ate every
     * legitimate bullet that slipped past the tight capsules - "j'ai vide 3 chargeurs",
     * with the 1s DBG throttle hiding the erased shots. Always-keep (last night's
     * revert) accepted every bullet through the box, and the widened box still contains
     * the whole UPRIGHT column - so a leaning player took damage at his vacated
     * standing silhouette: "sa hitbox agit comme s'il etait toujours stand".
     *
     * perbone_test now reports how far the bullet passed from the NEAREST bone surface:
     *   - within g_reject_margin  -> plausibly the body (capsule gap, pose error):
     *                                keep the hit, strip the location (generic damage,
     *                                never a ghost headshot);
     *   - beyond it on a LEANER   -> the empty silhouette: delete the hit;
     *   - non-leaning victim      -> engine answer untouched (his box IS his body).
     * COD1RELOADED_PERBONE_MARGIN tunes the band; STRICT=1 = old full deletion. */
    if (!g_strict) {
        float leanf = *(float*)((char*)cl + PS_LEANF);
        if (leanf == 0.0f) return;
        if (clear <= g_reject_margin) {
            tr->hitLocation = HL_NONE;
            return;
        }
        /* fall through to the deletion below */
    }

    /* strict: box hit, no bone -> world miss (g_entities[1023].takedamage==0). */
    tr->hitEntityNum = ENTITYNUM_NONE;
    tr->hitLocation  = HL_NONE;
    tr->fraction     = 1.0f;
    tr->endpos[0] = end[0]; tr->endpos[1] = end[1]; tr->endpos[2] = end[2];
    tr->surfaceFlags = 0;
}

/* Wrapper: whatever path the body returns through, the forced swing state is
 * rolled back before the engine continues - nothing can leak into the networked
 * pose or a later frame. */
static void Hook_LocationalTrace(pb_trace_t* tr, const float* start, const float* end,
                                 int passEnt, int mask, int sight)
{
    hook_lt_body(tr, start, end, passEnt, mask, sight);
    /* Never let a (future) nested invocation roll back the OUTER trace's forced
     * state mid-flight - today no nesting exists (disasm-proven: nothing on the
     * pose path reaches trap_LocationalTrace), this is hardening (audit MED-4). */
    if (!g_in_trace) sync_restore();
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
        printf("%s installed (mode=%s, body=%.1f head=%.1f/len%.1f neck=%.1f leanfrac=%.2f "
               "eyelean=%.2f strict=%d, game base 0x%08lx) [pose-before-trace]\n",
               TAG, g_mode == 2 ? "dump" : (g_mode == 3 ? "pose" : "on"),
               g_radius, g_head_rad, g_head_len,
               g_neck_rad, g_lean_frac, g_eye_lean, g_strict, (unsigned long)base);
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
    if (strcmp(e, "dump") == 0 || *e == 'd') g_mode = 2;
    else if (*e == 'p') g_mode = 3;   /* "pose": fresh-pose leaners, engine decides */
    else g_mode = 1;

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
    const char* el = getenv("COD1RELOADED_PERBONE_EYELEAN");
    if (el && *el) { float v = (float)atof(el); if (v >= 0.0f && v <= 2.0f) g_eye_lean = v; }
    const char* st = getenv("COD1RELOADED_PERBONE_STRICT");
    if (st && *st && *st != '0') g_strict = 1;
    const char* ss = getenv("COD1RELOADED_PERBONE_SWINGSYNC");
    if (ss && *ss) g_swing_sync = (*ss != '0');
    const char* mg = getenv("COD1RELOADED_PERBONE_MARGIN");
    if (mg && *mg) { float v = (float)atof(mg); if (v >= 0.0f && v <= 30.0f) g_reject_margin = v; }
    const char* q;
    if ((q = getenv("COD1RELOADED_PERBONE_SHIFT_SL")) && *q)
        { float v = (float)atof(q); if (v >= 0.0f && v <= 25.0f) g_shift_sl = v; }
    if ((q = getenv("COD1RELOADED_PERBONE_SHIFT_SR")) && *q)
        { float v = (float)atof(q); if (v >= 0.0f && v <= 25.0f) g_shift_sr = v; }
    if ((q = getenv("COD1RELOADED_PERBONE_SHIFT_CL")) && *q)
        { float v = (float)atof(q); if (v >= 0.0f && v <= 25.0f) g_shift_cl = v; }
    if ((q = getenv("COD1RELOADED_PERBONE_SHIFT_CR")) && *q)
        { float v = (float)atof(q); if (v >= 0.0f && v <= 25.0f) g_shift_cr = v; }
    const char* co = getenv("COD1RELOADED_PERBONE_CORNER");
    if (co && *co) g_corner = (*co != '0');
    const char* tp = getenv("COD1RELOADED_PERBONE_SHIFT_TAPER");
    if (tp && *tp) g_shift_taper = (*tp != '0');
    const char* hc = getenv("COD1RELOADED_PERBONE_HEADRAD_CROUCH");
    if (hc && *hc) { float v = (float)atof(hc); if (v >= 0.0f && v <= 20.0f) g_head_rad_crouch = v; }
    const char* dbg = getenv("COD1RELOADED_PERBONE_DEBUG");
    if (dbg && *dbg && *dbg != '0') g_debug = 1;
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
