/*
 * pose_sync.c - make the SERVER's player pose match what the CLIENT draws.
 *
 * WHAT CHANGED, AND WHY IT MATTERS MORE THAN ANY VALUE IN HERE
 * This module used to carry its own copy of the client's adjustments - a per-stance,
 * per-side lateral table plus a back-bone reprojection - kept in step with
 * lean_fix.cpp by hand, through environment variables (POSE_SYNC_UL / _UR /
 * _UL_CROUCH / _UR_CROUCH / _DIAG / _DIAG_K). That arrangement could not converge:
 * every time one of the four cases was aligned another drifted, because nothing forced
 * the two sides to be the same code. Those knobs are GONE. The adjustments now live in
 * src/shared/lean_controllers.c, which is compiled into cod1plus.so AND into the
 * client's mss32.dll from a byte-identical copy, and both call it.
 *
 * That is the cod2x principle, at the scale CoD1 actually needs. cod2x reimplements
 * BG_Player_DoControllersInternal once in src/shared/animation.cpp and repoints both
 * call sites at it (animation.cpp:1464 server, :1466 client). We do not need to go that
 * far: both hooks already exist and are proven in production, and only OUR deltas have
 * to be shared - a dozen fields, not the full structs.
 *
 * WHAT THIS FILE STILL OWNS
 *   1. the hook itself, and the three signature checks that prove we hooked the right
 *      function on this exact build;
 *   2. retuning the shared BG lateral constant in .rodata (see patch_lean_const);
 *   3. telling the shared code the two things only this side can know: the player's
 *      STANCE (from the bounding box, not eFlags - see lc_stance_from_box) and how many
 *      lateral units this binary's own engine copy already applied.
 *
 * WHAT THE SHARED CODE OWNS: the lateral top-up and the back-bone reprojection, plus the
 * controller-buffer dump used to compare the two sides field by field. See
 * src/shared/lean_controllers.h.
 *
 * WHERE THE POSE IS USED. ClientEndFrame stores the controller callback in ent+0x228
 * (game 0x3afb6) and G_DObjCalcPose / G_DObjCalcBone (0x6c7e4 / 0x6c877) invoke it
 * before every skeleton evaluation - which is what a locational bullet trace walks. So
 * this runs at bullet time, not only at snapshot time.
 *
 * Everything here touches only the buffer and the clientinfo the engine just handed us.
 * No call back into the game, no global base pointer, no entity walk.
 *
 * ON BY DEFAULT since 2026-08-10 (COD1RELOADED_POSE_SYNC=0 disables). perbone_hit is
 * off by default for the same reason: nothing may add a second mirror on top.
 *
 * 2026-09-09: the swing INPUTS are now matched at the source by swing_sync.c (the
 * server BG_PlayerAngles runs with the client's four constants), so the yaw forcing
 * below only remains as a fallback for when swing_sync is off or not installed.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp - glibc puts it here, not in string.h */
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "hooks.h"
#include "swing_sync.h"
#include "shared/lean_controllers.h"

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
/* The shared BG lateral-shift constant. Read at 0x1a58b and NOWHERE else in the binary -
 * that single reader is what makes retuning it in place safe. See patch_lean_const. */
#define RVA_LEAN_CONST      0x073d5c

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

/* clientinfo fields THIS FILE touches. The ones the shared dump reads live in
 * lean_controllers.c; these two are here because the optional yaw forcing writes them. */
#define CI_LEGS_YAW    0x380   /* legs.yawAngle   - read by the engine at 0x1a435 */
#define CI_TORSO_YAW   0x3b0   /* torso.yawAngle  - read by the engine at 0x1a441 */
#define CI_LEAN        0x3e4   /* the value fed to GetLeanFraction - 0x1a540 */
#define CI_VIEWYAW     0x3ec   /* playerAngles[1]. playerAngles is at 0x3e8, NOT 0x3e0 -
                                * ClientEndFrame 0x3b00c..0x3b03c copies ps.viewangles
                                * [0..2] to ci+0x3e8/0x3ec/0x3f0. */
#define ES_CLIENTNUM   0x90    /* read at 0x1a3d5 and 0x3afc5 */

/* Stance, from the gentity bounding box.
 * NOT from eFlags, and this is the one place the two sides legitimately differ. The
 * client reads es->eFlags & 0x20, measured 2026-08-10 over ~30 stance changes on two
 * clients. Server-side an eFlags read came back wrong on 2026-08-09 (every line said
 * "stand" while crouching), and the box height is what the rest of this codebase already
 * trusts (perbone_hit.c uses the same 55.0 threshold). Rather than bake one of the two
 * into the shared file, stance is passed in and the shared dump prints it next to the raw
 * eFlags - so if they ever disagree it shows up as data instead of as a wrong hitbox. */
#define E_MINS         0x104
#define E_MAXS         0x110
#define STANCE_H_CROUCH  55.0f  /* below this = crouch */
#define STANCE_H_PRONE   40.0f  /* below this = prone: the client applies nothing */

/* ---- THE VALIDATED CONFIGURATION, BAKED IN (2026-08-10) ----------------------
 * These defaults ARE the configuration that was measured correct: client and server
 * produce a bit-identical 24-float controller buffer for the same player in the same
 * pose, in both stances. They live here and not in a startup script because three
 * separate times today a correct setting failed to reach the process and the result
 * looked like a code bug. A server started with NO environment at all is now the good
 * one; every variable below is an override for experiments, never a requirement. */
static hook_t         g_dci_hook;
static dc_internal_t  orig_dc_internal = NULL;
static int            g_enable = 1;   /* COD1RELOADED_POSE_SYNC=0 to disable */
static int            g_yaw    = 1;   /* COD1RELOADED_POSE_SYNC_YAW=0 to disable.
                                       * Forces legs/torso yaw to the view before posing,
                                       * matching swing_fix's client-only patches. With it
                                       * off, the server's legs stay planted up to 40 deg
                                       * behind the view (vanilla swingTolerance) while the
                                       * client's track it (patched to 0), and the back-bone
                                       * reprojection then injects up to 7.6 deg of pitch
                                       * into the bone that carries the head. Measured. */
static int            g_dump   = 0;   /* COD1RELOADED_CTRL_DUMP: samples per client.
                                       * Diagnostic - deliberately NOT defaulted on. */

/* An unset variable means "use the built-in default", so only an explicit falsy value
 * turns something off. Written out rather than reusing the old `*e=='o'` test, which
 * matched "on" as well as "off" - harmless while the flag was opt-in, a trap now that
 * it is opt-out. */
static int env_is_off(const char* v)
{
    if (!v || !*v) return 0;
    if (v[0] == '0' && v[1] == '\0') return 1;
    if (!strcasecmp(v, "off"))   return 1;
    if (!strcasecmp(v, "false")) return 1;
    if (!strcasecmp(v, "no"))    return 1;
    return 0;
}

/* How many lateral units THIS binary's engine copy already applied at fLeanFrac = 1,
 * read back out of .rodata at install rather than assumed. That matters: it is 2.5 on a
 * stock game.mp.i386.so and 7.5 once patch_lean_const has run, and lean_controllers.c
 * tops whatever it is up to the shared total. So the server lands on the right pose
 * whether or not COD1RELOADED_LEAN_CONST is set - the two are no longer coupled by hand. */
static float          g_engine_lateral = 2.5f;

/* defined below, next to the constant's documentation */
static float g_lean_const;
static void patch_lean_const(uintptr_t base, float want);

/* ---- glue for the shared module ---- */

static void lc_log_sink(const char* line)
{
    printf("%s %s\n", TAG, line);
    fflush(stdout);
}

/* Any monotonic millisecond clock; only used to throttle dumps, and the throttle
 * subtracts with unsigned wraparound, so truncating to 32 bits is fine. */
static unsigned int lc_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned int)((unsigned long long)ts.tv_sec * 1000ull
                          + (unsigned long long)(ts.tv_nsec / 1000000L));
}

static int lc_stance_from_box(const void* ge)
{
    const float* mns = (const float*)((const char*)ge + E_MINS);
    const float* mxs = (const float*)((const char*)ge + E_MAXS);
    const float  h   = mxs[2] - mns[2];
    if (h < STANCE_H_PRONE)  return LC_STANCE_PRONE;
    if (h < STANCE_H_CROUCH) return LC_STANCE_CROUCH;
    return LC_STANCE_STAND;
}

static void hook_dc_internal(void* obj, void* es, int* partBits, void* ci, float* out)
{
    char*         c        = (char*)ci;
    int           forced   = 0;
    float         sv_torso = 0.0f, sv_legs = 0.0f;
    lc_ctx_t      ctx;
    /* hook_install() fills hook->trampoline BEFORE it writes the JMP into the target,
     * so this fallback closes the (microscopic, watcher-thread) window in which the
     * detour is live but orig_dc_internal has not been assigned yet. If even that is
     * unset something is very wrong: hand back the all-zero buffer, which is the same
     * thing the game itself produces for a player whose eFlags hit the 0xc000 test at
     * 0x1a39f - a valid pose, not a crash and not garbage. */
    dc_internal_t orig     = orig_dc_internal;

    if (!orig) orig = (dc_internal_t)g_dci_hook.trampoline;
    if (!orig) {
        if (out) memset(out, 0, LC_FLOATS * sizeof(float));
        return;
    }

    /* --- pre: optionally force the swing yaw the client draws ------------------
     * swing_fix.cpp patches BG_PlayerAngles in cgame ONLY (legs swingTolerance 40->0,
     * torso yaw swingSpeed ->1.0, torso yaw movefrac 0.3->0, lean swing speed 0.15->1.0).
     * Every one of those writes ci->legs.yawAngle / ci->torso.yawAngle / the lean swing
     * channel - exactly the fields the engine controller function reads - so the drawn
     * pose is computed from DIFFERENT INPUTS than the tested one, and no adjustment
     * downstream of the buffer can close that. This switch forces our inputs to the view
     * the way the client's patches force its own.
     * Default OFF, because it has never been measured against a dump. That is precisely
     * what the "IN" line of COD1RELOADED_CTRL_DUMP is for: if the client's legsY/torsoY
     * differ from ours for the same player in the same pose, turn this on. */
    /* swing_sync patched the server's own BG_PlayerAngles to the client's constants:
     * legs/torso/lean already ARE what the client draws, including the movement
     * offset on the legs that "legs = view" would wrongly erase for a moving player.
     * Only force when that mirror is not in place. */
    if (g_enable && g_yaw && ci && !swing_sync_installed()) {
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

    /* --- post: the shared adjustments, the same code the client runs ------------ */
    if (!g_enable || !out || !obj) {
        if (forced) {
            *(float*)(c + CI_TORSO_YAW) = sv_torso;
            *(float*)(c + CI_LEGS_YAW)  = sv_legs;
        }
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.side           = LC_SIDE_SERVER;
    ctx.client_num     = es ? *(const int*)((const char*)es + ES_CLIENTNUM) : -1;
    ctx.stance         = lc_stance_from_box(obj);
    ctx.engine_lateral = g_engine_lateral;
    ctx.now_ms         = lc_now_ms();
    ctx.es             = es;
    ctx.ci             = ci;
    ctx.dump           = g_dump;
    ctx.log            = lc_log_sink;

    lc_apply(out, &ctx);

    /* Restore AFTER the dump, not before it. With yaw forcing on, restoring first made
     * the "IN" line report the clientinfo the engine did NOT use - it showed legsY=89.98
     * while the buffer had actually been built from legsY = viewYaw. Comparing that line
     * against the client's would have been comparing a value against something that was
     * never an input. lc_apply only reads ci, so holding the forced values across it is
     * free; the window is still one hook call. */
    if (forced) {
        *(float*)(c + CI_TORSO_YAW) = sv_torso;
        *(float*)(c + CI_LEGS_YAW)  = sv_legs;
    }
}

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

    /* Read back what the engine will actually multiply by, whatever happened above.
     * Never assume it: an assumption here is the same class of bug as the one this
     * module exists to remove. */
    memcpy(&g_engine_lateral, (const void*)(base + RVA_LEAN_CONST),
           sizeof(g_engine_lateral));

    if (hook_install(&g_dci_hook, base + RVA_DC_INTERNAL,
                     (uintptr_t)hook_dc_internal, DCI_PATCHLEN) == 0) {
        orig_dc_internal = (dc_internal_t)g_dci_hook.trampoline;
        printf("%s installed (yaw=%d engine_lateral=%.2f, game base 0x%08lx) "
               "[build " __DATE__ " " __TIME__ "]\n",
               TAG, g_yaw, g_engine_lateral, (unsigned long)base);
        /* Identity of the shared module. The client logs the same line at its own
         * install. If the two strings differ, one repo's copy of
         * src/shared/lean_controllers.* was updated and the other was not, and every
         * alignment claim resting on them is void. */
        printf("%s shared: %s\n", TAG, lc_banner());
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
 *
 * The client cannot do this: MSVC POOLED its float literals, so cgame_mp_x86.dll holds
 * exactly ONE 2.5f (VA 0x3006b6bc) with FIVE readers, and patching it would corrupt four
 * unrelated computations. GCC did not pool - our 2.5f has exactly one reader - so the
 * server's copy can be retuned in place, which is why the arrangement is asymmetric.
 *
 * Setting this to 7.5 makes the engine itself produce the total the shared code wants, so
 * lean_controllers.c adds nothing on this side. Leaving it unset is now equally correct:
 * the constant stays 2.5, try_install reads that back into g_engine_lateral, and the
 * shared code tops up the remaining 5.0. The two paths land on the same pose - which is
 * the point of routing it through one file.
 *
 * Verified before writing: the 4 bytes must currently read 2.5 (or the target already),
 * otherwise this is a different build and we do nothing. */

/* 7.5 by default: the total the client also reaches (its own unpatchable 2.5 plus the
 * 5.0 lean_controllers.c tops it up with). Setting it to 0 leaves the .rodata alone, and
 * the shared code then adds the missing 5.0 here instead - both land on the same pose, so
 * this is a preference for WHERE the constant lives, not a correctness switch. */
static float g_lean_const = 7.5f;   /* COD1RELOADED_LEAN_CONST=0 to leave .rodata alone */

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
    printf("%s lean const: shared BG lateral shift %.2f -> %.2f\n", TAG, cur, want);
    fflush(stdout);
}

static void* watcher_thread(void* arg)
{
    (void)arg;
    for (;;) { try_install(); usleep(400 * 1000); }
    return NULL;
}

/* Environment variables that used to hand-tune this module's own copy of the client's
 * adjustments. They are retired, not renamed: the values they set now come from
 * src/shared/lean_controllers.c, which the client compiles too. Warn loudly if a startup
 * script still passes one, because silently ignoring it is exactly how a server ends up
 * posed differently from what it is told it is posed like. */
static void warn_retired(void)
{
    static const char* const retired[] = {
        "COD1RELOADED_POSE_SYNC_SHIFT",
        "COD1RELOADED_POSE_SYNC_UL",
        "COD1RELOADED_POSE_SYNC_UR",
        "COD1RELOADED_POSE_SYNC_UL_CROUCH",
        "COD1RELOADED_POSE_SYNC_UR_CROUCH",
        "COD1RELOADED_POSE_SYNC_DIAG",
        "COD1RELOADED_POSE_SYNC_DIAG_K",
        "COD1RELOADED_POSE_SYNC_DEBUG",
        "COD1RELOADED_POSE_SYNC_PROBE",
        NULL
    };
    int i;
    for (i = 0; retired[i]; ++i) {
        const char* v = getenv(retired[i]);
        if (v && *v) {
            printf("%s IGNORED: %s=%s - retired, the value now lives in "
                   "src/shared/lean_controllers.c (COD1RELOADED_CTRL_DUMP replaces "
                   "DEBUG and PROBE)\n", TAG, retired[i], v);
            fflush(stdout);
        }
    }
}

void pose_sync_init(void)
{
    const char* v;
    pthread_t tid;

    /* ON unless explicitly turned off - see the defaults block at the top. */
    if (env_is_off(getenv("COD1RELOADED_POSE_SYNC"))) {
        printf("%s disabled by COD1RELOADED_POSE_SYNC - the drawn model and the tested "
               "skeleton will NOT agree on a leaning player\n", TAG);
        fflush(stdout);
        return;
    }

    warn_retired();

    if (env_is_off(getenv("COD1RELOADED_POSE_SYNC_YAW"))) g_yaw = 0;

    /* Controller-buffer dump, same variable name and same output format as the client -
     * that is the whole point, the two logs are meant to be diffed line against line.
     * N = max samples per client. */
    v = getenv("COD1RELOADED_CTRL_DUMP");
    if (v && *v) {
        g_dump = atoi(v);
        if (g_dump < 0) g_dump = 0;
    }

    /* The cod2x way: retune the shared constant instead of mirroring the client. */
    v = getenv("COD1RELOADED_LEAN_CONST");
    if (v && *v) g_lean_const = (float)atof(v);

    if (pthread_create(&tid, NULL, watcher_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s watcher started (yaw=%d dump=%d lean_const=%.2f)\n",
               TAG, g_yaw, g_dump, g_lean_const);
        fflush(stdout);
    }
}
