/*
 * test_ctrl_blend.c - host-side checks of the controller blend (lean_controllers.c v2).
 *
 *   gcc -O2 -Wall -o /tmp/tcb test/test_ctrl_blend.c -lm && /tmp/tcb
 *
 * A synthetic "engine" produces the controller buffer over time for one player:
 *   - legs animation A until T_CHANGE, then B (a strafe flip);
 *   - the legs yaw swings to +90 after the change (exponential, like BG_SwingAngles with
 *     tolerance 0), the three back bones counter-rotate 0.4/0.4/0.2 - the chain sums to 0;
 *   - a lean ramps out starting at the SAME instant (the "strafe-peek");
 *   - the view pitch moves slowly.
 * The same timeline is posed by a "client" committing every rendered frame and by a
 * "server" committing once per server frame with on-demand PEEKs in between, and the two
 * results are compared at the server's frame times.
 */
#include "../src/shared/lean_controllers.c"
#define SERVER_ENGINE_LATERAL 7.5f   /* the retuned .rodata constant of a live server (COD1RELOADED_LEAN_CONST) */

#include <stdlib.h>

#define T_CHANGE 1000
#define FAILS_MAX 64

static int g_fail = 0;
static void check(int ok, const char* what, double got)
{
    printf("  [%s] %-58s %.4f\n", ok ? " ok " : "FAIL", what, got);
    if (!ok) ++g_fail;
}

static unsigned char g_es[0x100];

static float lean_at(int t)            /* fLeanFrac: 0 -> 0.75 over 200 ms from T_CHANGE */
{
    float x = (float)(t - T_CHANGE) / 200.0f;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    return 0.75f * x;
}

static int legs_anim_at(int t, int flicker)
{
    if (flicker && t >= T_CHANGE && t < T_CHANGE + 400) return ((t / 7) & 1) ? 12 : 11;
    return t < T_CHANGE ? 11 : 12;
}

/* the engine buffer at game time t (after the lateral: engine_lateral = total, no top-up) */
static void engine_pose(int t, float* out, int flicker)
{
    const float legs = (t < T_CHANGE) ? 0.0f : 90.0f * (1.0f - expf(-(float)(t - T_CHANGE) / 67.0f));
    const float lf   = lean_at(t);
    const float roll = lf * 50.0f * 0.925f;
    const float pitch = 10.0f * sinf((float)t / 900.0f);
    int v;
    memset(out, 0, LC_FLOATS * sizeof(float));
    out[LC_TAG_ANG_YAW]   = legs;
    out[LC_BACK_LOW + 1]  = -0.4f * legs;
    out[LC_BACK_MID + 1]  = -0.4f * legs;
    out[LC_BACK_UP + 1]   = -0.2f * legs;
    out[LC_BACK_LOW + 2]  = roll * 0.5f;
    out[LC_BACK_MID + 2]  = roll * 0.5f;
    out[LC_BACK_UP + 2]   = roll * -0.6f;
    out[LC_TAG_ANG_ROLL]  = lf * LC_LEAN_ROLL_PER_FRAC;
    out[LC_TAG_OFF_Y]     = -lf * SERVER_ENGINE_LATERAL;
    out[LC_BACK_LOW + 0]  = pitch * 0.2f;
    out[LC_BACK_MID + 0]  = pitch * 0.3f;
    out[LC_BACK_UP + 0]   = pitch * 0.5f;
    v = legs_anim_at(t, flicker);
    memcpy(g_es + 0xcc, &v, 4);
}

static void pose(int cn, int t, int blend, float* out, int flicker)
{
    lc_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.side = LC_SIDE_SERVER;
    c.client_num = cn;
    c.stance = LC_STANCE_STAND;
    c.engine_lateral = SERVER_ENGINE_LATERAL;
    c.es = g_es;
    c.blend = blend;
    c.game_time = t;
    engine_pose(t, out, flicker);
    lc_apply(out, &c);
}

/* the yaws only, before the reprojection touched pitch/roll: the reprojection never
 * writes yaw, so the post buffer's yaws are the blended ones */
static float chain_sum(const float* o)
{
    return o[LC_TAG_ANG_YAW] + o[LC_BACK_LOW + 1] + o[LC_BACK_MID + 1] + o[LC_BACK_UP + 1];
}

int main(void)
{
    float cli[4000][LC_FLOATS], srv[LC_FLOATS], tmp[LC_FLOATS], ref[LC_FLOATS];
    int t, i, cli_t[4000], ncli = 0;
    double worst_yaw = 0, worst_roll = 0, worst_sum = 0, worst_lean_lag = 0;
    int ef = 0, cn_c = 1, cn_s = 2;

    memcpy(g_es + 0x08, &ef, 4);

    printf("1) client (5 ms frames) vs server (50 ms frames + peeks), strafe flip + peek\n");
    for (t = 0; t <= 2500; t += 5) {           /* client */
        pose(cn_c, t, LC_BLEND_COMMIT, cli[ncli], 0);
        cli_t[ncli++] = t;
    }
    for (t = 0; t <= 2500; t += 50) {          /* server frames */
        pose(cn_s, t, LC_BLEND_COMMIT, srv, 0);
        /* a bullet trace between this frame and the next: bg.time is still t */
        pose(cn_s, t, LC_BLEND_PEEK, tmp, 0);
        for (i = 0; i < LC_FLOATS; ++i)
            if (fabsf(tmp[i] - srv[i]) > 1e-5f) { check(0, "peek at the frame time == commit", tmp[i] - srv[i]); break; }
        {
            const float* c = cli[t / 5];
            for (i = 0; i < 7; ++i) {
                const int j = lc_yaw_slot[i];
                double d = fabs((double)c[j] - srv[j]);
                if (d > worst_yaw) worst_yaw = d;
            }
            {
                double d = fabs((double)c[LC_BACK_LOW + 2] - srv[LC_BACK_LOW + 2]);
                if (d > worst_roll) worst_roll = d;
            }
        }
        if (fabs(chain_sum(srv)) > worst_sum) worst_sum = fabs(chain_sum(srv));
    }
    for (i = 0; i < ncli; ++i) {
        double s = fabs(chain_sum(cli[i]));
        if (s > worst_sum) worst_sum = s;
        engine_pose(cli_t[i], ref, 0);
        /* the lean passes through: tag roll equals the engine's at every client frame */
        {
            double d = fabs((double)cli[i][LC_TAG_ANG_ROLL] - ref[LC_TAG_ANG_ROLL]);
            if (d > worst_lean_lag) worst_lean_lag = d;
        }
    }
    check(worst_yaw < 3.0, "max |client - server| on any yaw (deg)", worst_yaw);
    check(worst_roll < 0.5, "max |client - server| back_low roll (deg)", worst_roll);
    check(worst_sum < 0.01, "max |yaw chain sum| both sides (0 = chest on the view)", worst_sum);
    check(worst_lean_lag < 1e-4, "tag_origin roll lag vs engine, client (deg)", worst_lean_lag);
    {
        /* mid-window: the pelvis must be partway, not at the target and not frozen */
        const float* c = cli[(T_CHANGE + 125) / 5];
        engine_pose(T_CHANGE + 125, ref, 0);
        check(c[LC_TAG_ANG_YAW] > 20.0f && c[LC_TAG_ANG_YAW] < ref[LC_TAG_ANG_YAW] - 10.0f,
              "pelvis yaw at +125 ms is mid-transition", c[LC_TAG_ANG_YAW]);
        c = cli[(T_CHANGE + 260) / 5];
        engine_pose(T_CHANGE + 260, ref, 0);
        check(fabsf(c[LC_TAG_ANG_YAW] - ref[LC_TAG_ANG_YAW]) < 0.5f,
              "pelvis yaw at +260 ms == engine (transition over)", c[LC_TAG_ANG_YAW] - ref[LC_TAG_ANG_YAW]);
    }

    printf("2) the vanilla engine limiter on the same timeline (5 ms frames), for reference\n");
    {
        float cur[LC_FLOATS];
        double vs = 0;
        engine_pose(0, cur, 0);
        for (t = 5; t <= 2500; t += 5) {
            engine_pose(t, ref, 0);
            for (i = 0; i < LC_FLOATS; ++i) cur[i] = lc_limit(ref[i], cur[i], 5 * LC_LIMIT_DEG_PER_MS);
            if (fabs(chain_sum(cur)) > vs) vs = fabs(chain_sum(cur));
        }
        printf("  vanilla max |yaw chain sum| = %.2f deg (the chest turned off the view)\n", vs);
    }

    printf("3) peeks do not move the state\n");
    {
        float a[LC_FLOATS], b[LC_FLOATS];
        memset(lc_bs, 0, sizeof(lc_bs));
        for (t = 0; t <= 1500; t += 50) pose(3, t, LC_BLEND_COMMIT, a, 0);
        for (t = 0; t <= 1500; t += 50) {
            pose(4, t, LC_BLEND_COMMIT, b, 0);
            pose(4, t, LC_BLEND_PEEK, tmp, 0);
            pose(4, t, LC_BLEND_PEEK, tmp, 0);
        }
        check(memcmp(a, b, sizeof(a)) == 0, "commit sequence identical with 2 peeks/frame", 0);
        /* a peek that sees the NEW animation before the frame commits it: pose frozen at the
         * last committed one (f = 0), not a jump */
        memset(lc_bs, 0, sizeof(lc_bs));
        for (t = 0; t <= 950; t += 50) pose(5, t, LC_BLEND_COMMIT, a, 0);
        engine_pose(1000, tmp, 0);             /* es now says anim B */
        {
            lc_ctx_t c;
            memset(&c, 0, sizeof(c));
            c.client_num = 5; c.stance = LC_STANCE_STAND; c.engine_lateral = SERVER_ENGINE_LATERAL;
            c.es = g_es; c.blend = LC_BLEND_PEEK; c.game_time = 950;
            lc_apply(tmp, &c);
        }
        check(fabsf(tmp[LC_TAG_ANG_YAW] - a[LC_TAG_ANG_YAW]) < 1e-4f,
              "early peek of a new animation holds the committed yaw", tmp[LC_TAG_ANG_YAW]);
    }

    printf("4) the lean alone never starts a transition\n");
    {
        int v = 11, restarts = 0;
        memset(lc_bs, 0, sizeof(lc_bs));
        for (t = 0; t <= 2000; t += 5) {
            float o[LC_FLOATS];
            lc_ctx_t c;
            const int prev_start = lc_bs[6].t_start;
            memset(o, 0, sizeof(o));
            o[LC_TAG_ANG_ROLL] = LC_LEAN_ROLL_PER_FRAC * 0.75f * sinf((float)t / 100.0f);
            memcpy(g_es + 0xcc, &v, 4);
            memset(&c, 0, sizeof(c));
            c.client_num = 6; c.stance = LC_STANCE_STAND; c.engine_lateral = SERVER_ENGINE_LATERAL;
            c.es = g_es; c.blend = LC_BLEND_COMMIT; c.game_time = t;
            lc_apply(o, &c);
            if (lc_bs[6].t_start != prev_start) ++restarts;
        }
        check(restarts == 0, "transitions started by a lean going in and out 3x", restarts);
    }

    printf("5) animation flicker every frame for 400 ms: slower, never frozen\n");
    {
        float o[LC_FLOATS], prev = 0;
        int stuck = 0, longest = 0;
        memset(lc_bs, 0, sizeof(lc_bs));
        for (t = 0; t <= 2000; t += 5) {
            pose(7, t, LC_BLEND_COMMIT, o, 1);
            if (t > T_CHANGE + 10 && t < T_CHANGE + 400) {
                if (fabsf(o[LC_TAG_ANG_YAW] - prev) < 1e-4f) { if (++stuck > longest) longest = stuck; }
                else stuck = 0;
            }
            prev = o[LC_TAG_ANG_YAW];
        }
        engine_pose(T_CHANGE + 400, ref, 1);
        check(longest <= 1, "consecutive frames with the pelvis yaw not moving", longest);
    }

    printf("5b) strafe spam at the server's own rate (20 Hz): the animation AND the legs goal\n"
           "    flip together on every server frame for 400 ms, as a key change does; the\n"
           "    server commits each flip, the client sees each one at its snapshot time\n");
    {
        static float legs[3001];              /* the swung legs yaw, 1 ms grid */
        float o[LC_FLOATS], cl[LC_FLOATS];
        double worst = 0, moved = 0;
        int f5, ms;
        legs[0] = 0.0f;
        for (ms = 1; ms <= 3000; ++ms) {
            const int snap = (ms / 50) * 50;
            const float goal = snap < T_CHANGE ? 0.0f
                             : (snap < T_CHANGE + 400 ? (((snap - T_CHANGE) / 50) & 1 ? -90.0f : 90.0f)
                                                      : 90.0f);
            legs[ms] = legs[ms - 1] + (goal - legs[ms - 1]) / 67.0f;
        }
        memset(lc_bs, 0, sizeof(lc_bs));
        for (t = 0; t <= 2000; t += 5) {
            const int snap = (t / 50) * 50;
            int v = snap < T_CHANGE ? 11
                  : (snap < T_CHANGE + 400 ? (((snap - T_CHANGE) / 50) & 1 ? 11 : 12) : 12);
            lc_ctx_t c;
            engine_pose(t, cl, 0);
            cl[LC_TAG_ANG_YAW] = legs[t];
            cl[LC_BACK_LOW + 1] = cl[LC_BACK_MID + 1] = -0.4f * legs[t];
            cl[LC_BACK_UP + 1] = -0.2f * legs[t];
            memcpy(g_es + 0xcc, &v, 4);
            memset(&c, 0, sizeof(c));
            c.client_num = 10; c.stance = LC_STANCE_STAND; c.engine_lateral = SERVER_ENGINE_LATERAL;
            c.es = g_es; c.blend = LC_BLEND_COMMIT; c.game_time = t;
            lc_apply(cl, &c);
            if (t % 50 == 0) {
                engine_pose(t, o, 0);
                o[LC_TAG_ANG_YAW] = legs[t];
                o[LC_BACK_LOW + 1] = o[LC_BACK_MID + 1] = -0.4f * legs[t];
                o[LC_BACK_UP + 1] = -0.2f * legs[t];
                memcpy(g_es + 0xcc, &v, 4);
                c.client_num = 11;
                lc_apply(o, &c);
                for (f5 = 0; f5 < 7; ++f5) {
                    const int j = lc_yaw_slot[f5];
                    if (fabs((double)o[j] - cl[j]) > worst) worst = fabs((double)o[j] - cl[j]);
                }
                if (t == T_CHANGE + 400 + LC_BLEND_MS) moved = fabs((double)o[LC_TAG_ANG_YAW] - legs[t]);
                if (getenv("TCB_DEBUG") && t >= 950 && t <= 1700)
                    printf("     t=%d anim=%d legs=%+.2f server=%+.2f client=%+.2f\n", t, v,
                           legs[t], o[LC_TAG_ANG_YAW], cl[LC_TAG_ANG_YAW]);
            }
        }
        check(worst < 0.5, "max |client - server| yaw under per-frame flips (deg)", worst);
        check(moved < 0.5, "pelvis == engine one window after the spam ends (deg)", moved);
    }

    printf("5c) KNOWN LIMIT, documented: the legs goal moves one server frame BEFORE the\n"
           "    animation changes (movementDir is velocity-based). The client has limited\n"
           "    toward it for that frame, the server has not: bounded by one server frame\n"
           "    of limiter travel (50 ms x 0.36 = 18 deg at 20 Hz), gone by the window's end\n");
    {
        float o[LC_FLOATS], cl[LC_FLOATS];
        double worst = 0, end_gap = 0;
        memset(lc_bs, 0, sizeof(lc_bs));
        for (t = 0; t <= 2000; t += 5) {
            const int snap = (t / 50) * 50;
            int v = snap < T_CHANGE + 50 ? 11 : 12;   /* the animation, one frame late */
            lc_ctx_t c;
            engine_pose(t, cl, 0);
            memcpy(g_es + 0xcc, &v, 4);
            memset(&c, 0, sizeof(c));
            c.client_num = 12; c.stance = LC_STANCE_STAND; c.engine_lateral = SERVER_ENGINE_LATERAL;
            c.es = g_es; c.blend = LC_BLEND_COMMIT; c.game_time = t;
            lc_apply(cl, &c);
            if (t % 50 == 0) {
                double d;
                engine_pose(t, o, 0);
                memcpy(g_es + 0xcc, &v, 4);
                c.client_num = 13;
                lc_apply(o, &c);
                d = fabs((double)o[LC_TAG_ANG_YAW] - cl[LC_TAG_ANG_YAW]);
                if (d > worst) worst = d;
                if (t == T_CHANGE + 50 + LC_BLEND_MS + 50) end_gap = d;
            }
        }
        check(worst <= 50 * LC_LIMIT_DEG_PER_MS + 0.01, "max |client - server| pelvis yaw (deg)", worst);
        check(end_gap < 0.5, "gap once the transition is over (deg)", end_gap);
    }

    printf("6) no history = the engine's pose\n");
    {
        float o[LC_FLOATS];
        memset(lc_bs, 0, sizeof(lc_bs));
        pose(8, 100, LC_BLEND_COMMIT, o, 0);
        pose(8, 1100, LC_BLEND_COMMIT, o, 0);     /* 1000 ms gap, mid-swing */
        engine_pose(1100, ref, 0);
        check(fabsf(o[LC_TAG_ANG_YAW] - ref[LC_TAG_ANG_YAW]) < 1e-4f, "after a 1 s gap, yaw == engine", o[LC_TAG_ANG_YAW]);
        pose(8, 900, LC_BLEND_COMMIT, o, 0);      /* time went backwards (map_restart) */
        engine_pose(900, ref, 0);
        check(fabsf(o[LC_TAG_ANG_YAW] - ref[LC_TAG_ANG_YAW]) < 1e-4f, "time going backwards resets", o[LC_TAG_ANG_YAW]);
    }

    printf("7) blend off = v1, bit for bit\n");
    {
        float a[LC_FLOATS], b[LC_FLOATS];
        lc_ctx_t c;
        engine_pose(1100, a, 0);
        memcpy(b, a, sizeof(a));
        memset(&c, 0, sizeof(c));
        c.client_num = 9; c.stance = LC_STANCE_STAND; c.engine_lateral = 2.5f; c.es = g_es;
        lc_apply(a, &c);
        /* v1 by hand: top-up then reprojection */
        {
            const float lf = b[LC_TAG_ANG_ROLL] / LC_LEAN_ROLL_PER_FRAC, toy = b[LC_TAG_ANG_YAW];
            b[LC_TAG_OFF_Y] += -lf * (lc_lateral_total(LC_STANCE_STAND, lf) - 2.5f);
            lc_adjust_rotation((b[LC_BACK_LOW + 1] - toy) * LC_DIAG_K, &b[LC_BACK_LOW]);
            lc_adjust_rotation((b[LC_BACK_MID + 1] - toy) * LC_DIAG_K, &b[LC_BACK_MID]);
        }
        check(memcmp(a, b, sizeof(a)) == 0, "LC_BLEND_OFF output == v1 arithmetic", 0);
    }

    printf("banner: %s\n", lc_banner());
    printf("%s\n", g_fail ? "SOME CHECKS FAILED" : "ALL OK");
    return g_fail ? 1 : 0;
}
