/*
 * pose_sync.c - make the SERVER's player pose match what the CLIENT draws.
 *
 * THE cod2x PRINCIPLE, ported properly. In CoD2 the animation controllers run on
 * both sides from one shared file, so the hit skeleton IS the drawn model and they
 * cannot disagree. CoD1 has the same bg code on both sides, but our client mod adds
 * a lateral body shift on top of it when leaning (lean_fix.cpp "helicopter" fix) -
 * drawn only. The server therefore tests a skeleton that stands ~5 units inboard of
 * the model the shooter sees.
 *
 * WHAT THE TWO SIDES ACTUALLY DO  (all disassembled from game.mp.i386.so md5
 * 343f99cd67b79ac74aeaa5261f63c011 on 2026-08-10; the client half is read from
 * cod1reloaded/src/gameplay/lean_fix.cpp)
 *
 *   shared bg code, BG_Player_DoControllersInternal (game RVA 0x1a389, and its exact
 *   twin inside cgame_mp_x86.dll) writes a 24-float controller buffer:
 *       out[20] = tag_origin_angles[2] = GetLeanFraction(lean) * 50.0f * 0.075f
 *       out[22] = tag_origin_offset[1] += -GetLeanFraction(lean) * 2.5f   <-- ALREADY
 *   i.e. CoD1 vanilla ALREADY shifts the body sideways on a lean, by 2.5 units, on
 *   BOTH sides, and it does so in code the server runs too. The old comment here
 *   ("the server has no such shift") was wrong; what is missing server-side is only
 *   the EXTRA shift our client adds.
 *
 *   client, lean_fix.cpp:154, on the same buffer, right after that function returns:
 *       lf = cbuf[20] / 3.75f                 (3.75 == 50.0 * 0.075, so lf == fLeanFrac)
 *       if (fabsf(lf) > 0.02f) cbuf[22] += -lf * K      K = 5.0 left, 2.5*2.0 right
 *   (crouch/prone can never reach this: the shared code memsets the whole buffer to 0
 *   when es->eFlags & 0xc000 (CROUCH|PRONE) - game 0x1a39f - so lf is 0 there. The
 *   client's crouch K table is dead code, and mirroring it server-side, as was tried
 *   on 2026-08-08, moves the tested skeleton away from a model that never moved.)
 *
 * WHAT THIS MODULE DOES
 *   Hook the same internal function on the server and run the same two lines on the
 *   same buffer. That is the whole fix. The server's own BG_LerpOffset + the tag
 *   publish (G_DObjSetLocalTag) then carry it to the skeleton exactly as they carry
 *   the vanilla 2.5, so the tested pose and the drawn pose are produced by identical
 *   arithmetic from identical inputs - including during the lean-in/lean-out ramp.
 *
 *   The pose is recomputed on demand: ClientEndFrame stores the controller callback
 *   in ent+0x228 (game 0x3afb6) and G_DObjCalcPose / G_DObjCalcBone (0x6c7e4 /
 *   0x6c877) invoke it before every skeleton evaluation - which is what a locational
 *   bullet trace walks. So this runs at bullet time, not only at snapshot time.
 *
 * WHY NOT THE PREVIOUS DESIGN (hook the outer BG_Player_DoControllers, then re-call
 * G_DObjSetLocalTag with a shifted copy)
 *   It worked in principle - G_DObjSetLocalTag (0x6c6c9) is a plain setter that
 *   overwrites rotTrans[tagIdx] wholesale, so calling it twice is legal - but it had
 *   to (a) reach a game function from outside, (b) re-derive the lean fraction from
 *   ps.leanf, and (c) reverse-map a clientinfo pointer to a client number and index
 *   g_entities. Every one of those was a separate way to crash or to silently no-op,
 *   and (b) was also numerically wrong: ps.leanf is clamped to +/-0.5 (PM_UpdateLean
 *   0x25cf5) while the client's lf is GetLeanFraction(leanf) = (2 - |leanf|)*leanf,
 *   i.e. 0.75 at full lean - so leanf/0.5 over-injected by 33%. Reading out[20] costs
 *   nothing and cannot be wrong: it IS the number the client divides.
 *
 * Everything here touches only the buffer and the clientinfo the engine just handed
 * us. There is no call back into the game, no global base pointer, no entity walk.
 *
 * OFF BY DEFAULT: COD1RELOADED_POSE_SYNC=1 to enable. With it enabled, set
 * COD1RELOADED_PERBONE_LEANFRAC=0 so perbone stops adding its own mirror on top.
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

#define TAG "[pose_sync]"

#define GAME_SO_NAME "game.mp.i386.so"

/* ---- game RVAs (verified against the .so on 2026-08-10) ---- */

/* BG_Player_DoControllersInternal - static, no symbol. Identified by: it is the one
 * and only callee of the `call` at BG_Player_DoControllers+0x37, and it is called
 * from nowhere else in .text. install() proves both facts from the bytes. */
#define RVA_DC_INTERNAL     0x01a389
/* BG_Player_DoControllers - .dynsym, value 0x1ad71 size 0x164. Not hooked; only used
 * to prove RVA_DC_INTERNAL is really its internal on this build. */
#define RVA_DC_OUTER        0x01ad71
#define DC_OUTER_CALLOFF    0x37       /* offset of `call <internal>` inside the outer */

/* `55 | 89 e5 | 56 | 53` - push ebp / mov ebp,esp / push esi / push ebx. Exactly 5
 * bytes, and the next instruction (sub esp,0xe0) starts at 5, so the 5-byte JMP
 * detour lands on an instruction boundary and needs no NOP padding. */
#define DCI_PATCHLEN 5
static const unsigned char DCI_PROLOGUE[DCI_PATCHLEN] = { 0x55, 0x89, 0xe5, 0x56, 0x53 };

/* `55 | 89 e5 | 53 | 81 ec 94 00 00 00` */
static const unsigned char DCO_PROLOGUE[10] = {
    0x55, 0x89, 0xe5, 0x53, 0x81, 0xec, 0x94, 0x00, 0x00, 0x00
};

/* FIVE arguments. From the call site at 0x1ad86..0x1ada8, which pushes, in order:
 *   [esp+0x00] = [ebp+0x08]  obj      (the gentity; also its own entityState)
 *   [esp+0x04] = [ebp+0x0c]  es       (same pointer - es is at gentity offset 0)
 *   [esp+0x08] = [ebp+0x10]  partBits (int[4], zeroed to 0xff by G_DObjCalcPose)
 *   [esp+0x0c] = [ebp+0x14]  ci       (&bgs.clientinfo[es->clientNum], stride 0x4b0)
 *   [esp+0x10] = &outbuf              (0x60 bytes = 24 floats = 8 vec3)
 * The internal reads es at +8 (eFlags) / +0x90 (clientNum) and ci at +0x380 / +0x3b0 /
 * +0x3b8 / +0x3e4 / +0x3e8..0x3f0, and writes only outbuf. */
typedef void (*dc_internal_t)(void* obj, void* es, int* partBits, void* ci, float* out);

/* clientinfo fields the internal itself reads (same bg struct as cgame) */
#define CI_LEGS_YAW    0x380   /* legs.yawAngle   - read at 0x1a435 */
#define CI_TORSO_YAW   0x3b0   /* torso.yawAngle  - read at 0x1a441 */
#define CI_LEAN        0x3e4   /* the lean value fed to GetLeanFraction - 0x1a540   */
#define CI_VIEWYAW     0x3ec   /* playerAngles[1]. playerAngles is at 0x3e8, NOT
                                * 0x3e0 - ClientEndFrame 0x3b00c..0x3b03c copies
                                * ps.viewangles[0..2] to ci+0x3e8/0x3ec/0x3f0 and
                                * the internal reads the vec3 from 0x3e8. */
#define ES_CLIENTNUM   0x90    /* logging only; read at 0x1a3d5 and 0x3afc5 */
/* gentity bounding box - the reliable stance signal (see hook_dc_internal).
 * NOT eFlags: `es->eFlags & 0x4000` was measured FALSE on a crouched player. */
#define E_MINS         0x104
#define E_MAXS         0x110
#define STANCE_H_CROUCH  55.0f  /* below this = crouch (same value perbone_hit.c uses) */
#define STANCE_H_PRONE   40.0f  /* below this = prone: apply nothing, like the client */

/* --- controller buffer: 24 floats, 8 vec3 --------------------------------------
 *   [0..2] back_low  [3..5] back_mid  [6..8] back_up  [9..11] neck
 *   [12..14] head    [15..17] pelvis  [18..20] tag_origin_angles
 *   [21..23] tag_origin_offset
 * Proven by the tail of the internal (0x1aa87..0x1ab75): the 0..5 loop copies six
 * vec3 to out+i*12, then legsAngles -> out+0x48 and the offset vec3 -> out+0x54.
 * Byte 0x50 = float 20, byte 0x58 = float 22 - the exact indices lean_fix.cpp uses. */
#define CB_TAG_ANG_ROLL      20
#define CB_TAG_OFF_Y         22
/* Back-bone angle triples, same layout the client indexes (lean_fix.h:48-55:
 * back_low 0x00, back_mid 0x0c, back_up 0x18, tag_origin_angles 0x48 = floats 0/3/6/18).
 * Each triple is [pitch, yaw, roll]. */
#define CB_BACK_LOW           0
#define CB_BACK_MID           3
#define CB_BACK_UP            6
#define CB_TAG_ANG_YAW       19

/* --- animation_adjustRotation (cod2x), mirrored ---------------------------------
 * THE SECOND HALF OF THE DESYNC, found 2026-08-11 from enzo's testers: with the lateral
 * shift corrected, the head CENTRE lines up but the "upper OUTER corner" of the head -
 * the side the player leans toward - stays unhittable. Reported for stand-left ("en haut
 * a droite de la tete") and crouch-right ("en haut a gauche"), which are the same corner
 * seen from the shooter. Raising the lateral K never fixed it, and could not: this is a
 * ROTATION error, not a translation one.
 * The client reprojects the back bones' pitch/roll by their yaw delta vs tag_origin
 * (lean_fix.cpp, move_diag_fix=2, diag_k_pos=diag_k_neg=0.75, move_diag_parent=0 =>
 * yawDiff = childYaw - tagOriginYaw). back_low and back_mid CARRY THE HEAD, so the drawn
 * skull is tilted differently from the posed one; and since the engine's head volume is
 * an ORIENTED BOX rotating with the bone, an angular error moves the corners far more
 * than the centre. perbone_hit.c documents the symptom three times ("structural, not
 * tuning") without ever identifying this as the cause.
 * The client applies it UNCONDITIONALLY (move_diag_lean_only=false), so we must too -
 * with yawDiff ~ 0 it is the identity, which is the common case. */
#define DIAG_K_DEFAULT       0.75f
#define DIAG_FIX_DEFAULT     2      /* 2 = back_low + back_mid, exactly the client */

/* out[20] = fLeanFrac * 50.0f * 0.075f. Both constants read from .rodata:
 * 50.0 at 0x73d30 (ebx-0x15068), 0.075 at 0x73da4 (ebx-0x14ff4), combined at
 * game 0x1a8b7..0x1a8cd. lean_fix.cpp divides by the same literal 3.75f. */
#define LEAN_ROLL_PER_FRAC   3.75f
/* lean_fix.cpp: `if (... && fabsf(lf) > 0.02f)` */
#define LEAN_EPS             0.02f
/* Pure sanity guard. |GetLeanFraction(leanf)| maxes at 0.75 (leanf clamped to 0.5 by
 * PM_UpdateLean), so a legitimate frame can never reach this; it only stops a garbage
 * buffer from turning into a wild shift. */
#define LEAN_SANE_MAX        1.0f

/* Extra shift, in units at fLeanFrac = 1, ON TOP of the 2.5 the shared code already
 * applies. MUST MATCH lean_fix.cpp:154 x body_shift_lean_scale(=1.0):
 *     left  (lf < 0): K = 5.0
 *     right (lf > 0): K = 2.5 * body_shift_right_scale(=2.0) = 5.0
 * Consistent with the 2026-08-11 engine-verdict session (ENG# lines): the drawn head
 * sat ~5u further out than the tested one, both sides.
 * Live calibration: COD1RELOADED_POSE_SYNC_UL / _UR. */
#define SHIFT_UNITS_LEFT_DEFAULT   5.0f
#define SHIFT_UNITS_RIGHT_DEFAULT  5.0f

/* CROUCH IS NOT EXEMPT - measured live 2026-08-11, and this overturns two "verified"
 * disassembly claims that cost us three days:
 *   - the controller buffer is NOT zeroed in crouch: the debug line shows lf = +-0.750
 *     on a crouched leaner, i.e. a FULL lean fraction reaching the buffer;
 *   - ps.leanf is NOT clamped to 0.25 in crouch either (0.750 == GetLeanFraction(0.5)).
 * So lean_fix.cpp's crouch branch is LIVE code, not dead, and its K differs from stand
 * on the LEFT only (lean_fix.cpp:154):
 *       K = is_crouch ? (left ? 12.5 : 2.5) : (left ? 5.0 : 2.5);  right *= 2.0
 *   => stand:  L 5.0   R 5.0
 *      crouch: L 12.5  R 5.0
 * Injecting 5.0 in crouch-left left a 5.6u shortfall - "I shoot his helmet and he does
 * not die". Note the 2026-08-08 attempt to mirror 12.5 was reverted on the (wrong)
 * dead-code theory, AND could not have had any effect anyway: pose_sync was inert then
 * (tag_origin unresolved). Do not re-derive this from the disassembly - it is measured. */
#define SHIFT_UNITS_LEFT_CROUCH_DEFAULT   12.5f
#define SHIFT_UNITS_RIGHT_CROUCH_DEFAULT   5.0f

static hook_t         g_dci_hook;
static dc_internal_t  orig_dc_internal = NULL;
static int            g_enable = 0;
static int            g_shift  = 1;   /* inject the body shift  */
static int            g_yaw    = 0;   /* force the swing yaw    */
static int            g_debug  = 0;
static int            g_probe  = 0;   /* COD1RELOADED_POSE_SYNC_PROBE - see hook body */
static long           g_hits   = 0;
static float          g_units_left  = SHIFT_UNITS_LEFT_DEFAULT;
static float          g_units_right = SHIFT_UNITS_RIGHT_DEFAULT;
static float          g_units_left_crouch  = SHIFT_UNITS_LEFT_CROUCH_DEFAULT;
static float          g_units_right_crouch = SHIFT_UNITS_RIGHT_CROUCH_DEFAULT;
static int            g_diag_fix = DIAG_FIX_DEFAULT;
static float          g_diag_k   = DIAG_K_DEFAULT;

/* lean_fix.cpp adjust_rotation_yd(), verbatim: rotate the (pitch, roll) pair of one
 * bone's angle triple by yawDiffDeg. Identity when yawDiffDeg is 0. */
static void adjust_rotation_yd(float yawDiffDeg, float* child)
{
    const float rad = yawDiffDeg * 0.01745329252f;
    const float cp  = cosf(rad), sp = sinf(rad);
    const float p   = child[0], r = child[2];
    child[0] = p * cp - r * sp;
    child[2] = p * sp + r * cp;
}

static void hook_dc_internal(void* obj, void* es, int* partBits, void* ci, float* out)
{
    char*         c        = (char*)ci;
    int           forced   = 0;
    float         sv_torso = 0.0f, sv_legs = 0.0f;
    /* hook_install() fills hook->trampoline BEFORE it writes the JMP into the target,
     * so this fallback closes the (microscopic, watcher-thread) window in which the
     * detour is live but orig_dc_internal has not been assigned yet. If even that is
     * unset something is very wrong: hand back the all-zero buffer, which is the same
     * thing the game itself produces for a crouched/prone player - a valid pose, not
     * a crash and not garbage. */
    dc_internal_t orig     = orig_dc_internal;

    if (!orig) orig = (dc_internal_t)g_dci_hook.trampoline;
    if (!orig) {
        if (out) memset(out, 0, 24 * sizeof(float));
        return;
    }

    /* --- PROBE: resolve the last two unknowns blocking the shared-code port ------
     * Writing src/shared/*.c requires two facts that are currently GUESSES, and a guess
     * baked into the one file that is meant to be authoritative for both sides would
     * recreate the exact divergence the port exists to remove:
     *
     *   1. WHICH BIT MEANS CROUCH. lean_fix.cpp gates on es->eFlags & 0x4000, but that
     *      read came back FALSE for a crouched player server-side on 2026-08-09, which is
     *      why pose_sync falls back to the bounding-box height. The shared file cannot
     *      carry two different stance tests. Printing eFlags raw next to the measured box
     *      height settles it: if the bit is simply a different one, we read it off the
     *      value; if eFlags really is 0 here, the box height is the only honest input and
     *      the CALLER must pass stance in.
     *   2. THE animMovetype ENUM ORDER (es+0xe0, 4 bits, idle + 8 directions). CoD1 has no
     *      animations[].flags & 0x10/0x20 for cod2x's isMovingLeft/Right - a scan of the
     *      whole .text found zero such tests - so animMovetype is the replacement, and it
     *      is networked, hence identical on both sides by construction. Its value order is
     *      not derivable from disassembly; walking each direction is minutes of work.
     *
     * ONE line per direction change, so it cannot flood a live server. Read-only.
     * Enable with COD1RELOADED_POSE_SYNC_PROBE=1. */
    if (g_probe && es && obj) {
        const unsigned  ef  = *(const unsigned*)((const char*)es + 0x08);
        const int       mvt = (*(const int*)((const char*)es + 0xe0)) & 0xf;
        const int       la  = (*(const int*)((const char*)es + 0xcc)) & 0x3ff;
        const int       cn  = *(const int*)((const char*)es + ES_CLIENTNUM);
        const float*    mns = (const float*)((const char*)obj + E_MINS);
        const float*    mxs = (const float*)((const char*)obj + E_MAXS);
        const float     h   = mxs[2] - mns[2];
        static int   last_mvt[64];
        static float last_h[64];
        if (cn >= 0 && cn < 64 && (last_mvt[cn] != mvt || last_h[cn] != h)) {
            last_mvt[cn] = mvt; last_h[cn] = h;
            printf("%s PROBE cn=%d animMovetype=%2d legsAnim=%3d eFlags=0x%08x "
                   "(0x4000=%d 0x8000=%d) boxh=%.0f -> %s\n",
                   TAG, cn, mvt, la, ef, (ef & 0x4000) ? 1 : 0, (ef & 0x8000) ? 1 : 0, h,
                   h < STANCE_H_PRONE ? "prone" : (h < STANCE_H_CROUCH ? "CROUCH" : "stand"));
            fflush(stdout);
        }
    }

    /* --- pre: optionally force the swing yaw the client draws ------------------
     * swing_fix locks the drawn torso/legs yaw to the view; vanilla server lags.
     * Only meaningful while leaning, and the shared code throws the whole buffer
     * away in crouch/prone, so no stance gate is needed here. Default OFF: the
     * 2026-08-10 dump session showed alignment without it. */
    if (g_enable && g_yaw && ci) {
        if (*(const float*)(c + CI_LEAN) != 0.0f) {
            const float vy = *(const float*)(c + CI_VIEWYAW);
            sv_torso = *(float*)(c + CI_TORSO_YAW);
            sv_legs  = *(float*)(c + CI_LEGS_YAW);
            *(float*)(c + CI_TORSO_YAW) = vy;
            *(float*)(c + CI_LEGS_YAW)  = vy;
            forced = 1;
        }
    }

    orig(obj, es, partBits, ci, out);

    if (forced) {
        *(float*)(c + CI_TORSO_YAW) = sv_torso;
        *(float*)(c + CI_LEGS_YAW)  = sv_legs;
    }

    /* --- post: the client's edits, verbatim, on the same buffer ---------------- */
    if (!g_enable || !out) return;

    /* 1) lateral body shift - only while leaning */
    if (g_shift) {
        const float lf = out[CB_TAG_ANG_ROLL] * (1.0f / LEAN_ROLL_PER_FRAC);
        float units;
        int   crouch;

        /* `goto` rather than `return`: the rotation mirror below must still run - the
         * client applies it unconditionally, so a non-leaning player needs it too. */
        if (!(lf > LEAN_EPS || lf < -LEAN_EPS)) goto diag;   /* also rejects NaN */
        if (lf > LEAN_SANE_MAX || lf < -LEAN_SANE_MAX) goto diag;

        /* STANCE FROM THE BOUNDING BOX HEIGHT, not from eFlags.
         * Measured live 2026-08-11: `es->eFlags & 0x4000` reads FALSE on a crouched
         * player - every debug line said "stand" while crouching. That also explains why
         * the shared code's `eFlags & 0xC000 -> memset` at 0x1a39f never fires in crouch
         * (hence the full lean fraction we see in the buffer): whatever 0x4000 means on
         * this build, it is not "crouched".
         * The box height IS reliable and is what the rest of this codebase already uses
         * (perbone_hit.c `is_crouch = (maxs[2]-mins[2]) < 55.0f`, and grant's stand gate,
         * which demonstrably excluded crouched players). obj == es == the gentity, so
         * mins/maxs are at gentity+0x104 / +0x110.
         * Standing box is 72 tall, crouch 55-ish, prone ~30 - so <STANCE_H_CROUCH is
         * crouch-or-lower, and below STANCE_H_PRONE we skip entirely (the client returns
         * early on prone, lean_fix.cpp:130). */
        {
            const char*  ge  = (const char*)obj;
            const float* mns = (const float*)(ge + E_MINS);
            const float* mxs = (const float*)(ge + E_MAXS);
            const float  h   = mxs[2] - mns[2];
            if (h < STANCE_H_PRONE) goto diag;       /* prone: client shifts nothing */
            crouch = (h < STANCE_H_CROUCH);
        }

        units = crouch ? (lf < 0.0f ? g_units_left_crouch : g_units_right_crouch)
                       : (lf < 0.0f ? g_units_left        : g_units_right);
        if (units == 0.0f) goto diag;

        out[CB_TAG_OFF_Y] += -lf * units;

        if (g_debug && (++g_hits % 200) == 1) {
            const float* dmn = (const float*)((const char*)obj + E_MINS);
            const float* dmx = (const float*)((const char*)obj + E_MAXS);
            /* `native` is what the shared BG code itself already put in the buffer -
             * derived by subtraction, NOT from a hardcoded 2.5, because
             * COD1RELOADED_LEAN_CONST may have retuned that constant. Printing the
             * literal here was wrong and read as "the patch did not apply". */
            const float injected = -lf * units;
            printf("%s cn=%d %s %s h=%.0f lf=%.3f native=%.2f inject=%.2f -> off_y=%.2f "
                   "(K=%.1f yaw=%d)\n",
                   TAG, es ? *(const int*)((const char*)es + ES_CLIENTNUM) : -1,
                   crouch ? "crouch" : "stand ", lf < 0.0f ? "L" : "R",
                   dmx[2] - dmn[2],
                   lf, out[CB_TAG_OFF_Y] - injected, injected,
                   out[CB_TAG_OFF_Y], units, forced);
            fflush(stdout);
        }
    }

diag:
    /* 2) animation_adjustRotation on the back bones - ALWAYS, like the client.
     * back_low and back_mid carry the head; without this the drawn skull is tilted
     * differently from the posed one and the engine's oriented head BOX has its outer
     * top corner in the wrong place - the "coin haut exterieur non hitable". */
    if (g_diag_fix > 0) {
        const float toy   = out[CB_TAG_ANG_YAW];
        const float kside = g_diag_k;   /* client: diag_k_pos == diag_k_neg == 0.75 */
        adjust_rotation_yd((out[CB_BACK_LOW + 1] - toy) * kside, &out[CB_BACK_LOW]);
        if (g_diag_fix >= 2)
            adjust_rotation_yd((out[CB_BACK_MID + 1] - toy) * kside, &out[CB_BACK_MID]);
        if (g_diag_fix >= 3)
            adjust_rotation_yd((out[CB_BACK_UP + 1] - toy) * kside, &out[CB_BACK_UP]);
    }
}

/* defined below, next to the constant's documentation */
static float g_lean_const;
static void patch_lean_const(uintptr_t base, float want);

/* ============================ install ============================ */
static void try_install(void)
{
    uintptr_t base = 0;
    FILE* f;
    char line[512];
    int32_t rel;

    if (orig_dc_internal) return;

    f = fopen("/proc/self/maps", "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, GAME_SO_NAME)) {
            unsigned long lo = strtoul(line, NULL, 16);
            if (!base || lo < base) base = (uintptr_t)lo;
        }
    }
    fclose(f);
    if (!base) return;

    /* Three independent checks. Any mismatch = a different build = do nothing.
     *   1. the OUTER function still has its known prologue,
     *   2. the call at outer+0x37 is a 5-byte E8 that resolves to RVA_DC_INTERNAL
     *      (this is what makes 0x1a389 "the controller internal" and not an address
     *      that happens to look like a function),
     *   3. the internal has its known 5-byte prologue. */
    {
        const unsigned char* o = (const unsigned char*)(base + RVA_DC_OUTER);
        const unsigned char* p = (const unsigned char*)(base + RVA_DC_INTERNAL);
        const unsigned char* callsite = o + DC_OUTER_CALLOFF;
        static int logged = 0;
        int ok = 1;

        if (memcmp(o, DCO_PROLOGUE, sizeof(DCO_PROLOGUE)) != 0) ok = 0;
        if (ok && callsite[0] != 0xE8) ok = 0;
        if (ok) {
            memcpy(&rel, callsite + 1, 4);
            if ((uintptr_t)(callsite + 5) + (intptr_t)rel != base + RVA_DC_INTERNAL) ok = 0;
        }
        if (ok && memcmp(p, DCI_PROLOGUE, sizeof(DCI_PROLOGUE)) != 0) ok = 0;

        if (!ok) {
            if (!logged) {
                logged = 1;
                printf("%s signature mismatch at BG_Player_DoControllers"
                       "(Internal) - not installing\n", TAG);
                fflush(stdout);
            }
            return;
        }
    }

    /* Before hooking: if asked, retune the shared BG constant itself. Done here and
     * not in init() because it needs the module base. Idempotent - patch_lean_const
     * accepts the value already being the target. */
    if (g_lean_const > 0.0f) patch_lean_const(base, g_lean_const);

    if (hook_install(&g_dci_hook, base + RVA_DC_INTERNAL,
                     (uintptr_t)hook_dc_internal, DCI_PATCHLEN) == 0) {
        orig_dc_internal = (dc_internal_t)g_dci_hook.trampoline;
        printf("%s installed (yaw=%d shift=%d stand L/R=%.1f/%.1f crouch L/R=%.1f/%.1f, "
               "game base 0x%08lx) [build " __DATE__ " " __TIME__ "]\n",
               TAG, g_yaw, g_shift, g_units_left, g_units_right,
               g_units_left_crouch, g_units_right_crouch, (unsigned long)base);
        fflush(stdout);
    } else {
        printf("%s hook_install failed\n", TAG);
        fflush(stdout);
    }
}

/* ===================== THE cod2x WAY: patch the shared constant ==================
 * Instead of mirroring the client's extra shift with a hook, change the number the
 * SHARED BG code already multiplies by - so both sides compute the same thing with the
 * same code, and nothing has to be kept in step by hand.
 *
 *   shared: out[22] += -fLeanFrac * 2.5      (server .rodata 0x73d5c, read at 0x1a58b;
 *                                             cgame carries its own copy of the same)
 *   client total today = 2.5 (its constant) + 5.0 (lean_fix.cpp hook) = 7.5
 *
 * So setting the SERVER constant to 7.5 makes the server pose match the drawn model
 * natively, WITH NO CLIENT CHANGE AT ALL - the client already lands on 7.5 by adding its
 * hook on top of its own 2.5.
 *
 * What it does NOT cover: crouch. The client's crouch-left K is 12.5, i.e. 15.0 total,
 * and one unconditional constant cannot be both 7.5 and 15.0. So with LEAN_CONST set,
 * turn the STAND injection off (UL=UR=0) and keep only the crouch extra:
 *     COD1RELOADED_LEAN_CONST=7.5 POSE_SYNC_UL=0 POSE_SYNC_UR=0
 *     COD1RELOADED_POSE_SYNC_UL_CROUCH=7.5 COD1RELOADED_POSE_SYNC_UR_CROUCH=0
 * (crouch-left then gets 7.5 native + 7.5 injected = 15.0, crouch-right 7.5 = its 2.5+5.0.)
 *
 * The long game is to patch the same constant in cgame too and delete both the client
 * hook and this module - then the desync is structurally impossible rather than
 * corrected. That needs a client redeploy, so it is not done here.
 *
 * Verified before writing: the 4 bytes must currently read 2.5 (or the target already),
 * otherwise this is a different build and we do nothing. */
#define RVA_LEAN_CONST  0x073d5c

static float g_lean_const = 0.0f;   /* 0 = leave the constant alone */

static void patch_lean_const(uintptr_t base, float want)
{
    float* p = (float*)(base + RVA_LEAN_CONST);
    float  cur;
    if (hook_unprotect((uintptr_t)p, (int)sizeof(float)) != 0) {
        printf("%s lean const: cannot unprotect .rodata - skipped\n", TAG);
        fflush(stdout);
        return;
    }
    cur = *p;
    if (!(cur > 2.49f && cur < 2.51f) && !(cur > want - 0.01f && cur < want + 0.01f)) {
        printf("%s lean const: 0x%x reads %.3f, expected 2.5 - NOT patching\n",
               TAG, (unsigned)RVA_LEAN_CONST, cur);
        fflush(stdout);
        return;
    }
    *p = want;
    printf("%s lean const: shared BG lateral shift %.2f -> %.2f "
           "(server now matches the client's 2.5+5.0 natively; set UL/UR to 0)\n",
           TAG, cur, want);
    fflush(stdout);
}

static void* watcher_thread(void* arg)
{
    (void)arg;
    for (;;) { try_install(); usleep(400 * 1000); }
    return NULL;
}

void pose_sync_init(void)
{
    const char* e = getenv("COD1RELOADED_POSE_SYNC");
    const char *y, *s, *d, *ul, *ur;
    pthread_t tid;

    if (!e || *e == '0' || *e == 'o' || *e == 'f' || *e == 'n') return;
    g_enable = 1;

    /* Yaw forcing defaults OFF: the 2026-08-10 dump session showed LEFT lean
     * drawn==tested with no yaw forcing at all. POSE_SYNC_YAW=1 for experiments. */
    y = getenv("COD1RELOADED_POSE_SYNC_YAW");
    if (y && *y) g_yaw = (*y != '0');
    s = getenv("COD1RELOADED_POSE_SYNC_SHIFT");
    if (s && *s) g_shift = (*s != '0');
    d = getenv("COD1RELOADED_POSE_SYNC_DEBUG");
    if (d && *d && *d != '0') g_debug = 1;

    /* Per-side extra shift in units at fLeanFrac = 1 (see the K table above). */
    ul = getenv("COD1RELOADED_POSE_SYNC_UL");
    if (ul && *ul) g_units_left = (float)atof(ul);
    ur = getenv("COD1RELOADED_POSE_SYNC_UR");
    if (ur && *ur) g_units_right = (float)atof(ur);
    ul = getenv("COD1RELOADED_POSE_SYNC_UL_CROUCH");
    if (ul && *ul) g_units_left_crouch = (float)atof(ul);
    ur = getenv("COD1RELOADED_POSE_SYNC_UR_CROUCH");
    if (ur && *ur) g_units_right_crouch = (float)atof(ur);
    /* Rotation mirror: DIAG=0 disables it, DIAG_K rescales it (client value 0.75). */
    ul = getenv("COD1RELOADED_POSE_SYNC_DIAG");
    if (ul && *ul) g_diag_fix = atoi(ul);
    ur = getenv("COD1RELOADED_POSE_SYNC_DIAG_K");
    if (ur && *ur) g_diag_k = (float)atof(ur);
    /* The cod2x way: retune the shared constant instead of mirroring the client.
     * 7.5 makes the server match the client's 2.5+5.0 with no client change. */
    ul = getenv("COD1RELOADED_POSE_SYNC_PROBE");
    if (ul && *ul && *ul != '0') g_probe = 1;
    ul = getenv("COD1RELOADED_LEAN_CONST");
    if (ul && *ul) g_lean_const = (float)atof(ul);

    if (pthread_create(&tid, NULL, watcher_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started (yaw=%d shift=%d stand L/R=%.1f/%.1f "
               "crouch L/R=%.1f/%.1f diag=%d k=%.2f)\n",
               TAG, g_yaw, g_shift, g_units_left, g_units_right,
               g_units_left_crouch, g_units_right_crouch, g_diag_fix, g_diag_k);
        fflush(stdout);
    }
}
