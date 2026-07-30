/*
 * cheat_gate.c - server side of the cvar cheat check (PunkBuster replacement, part 2).
 *
 * The client mod (mss32.dll, cheat_scan.cpp) asks the engine whether any blocklisted
 * cheat cvar name exists in its cvar table (ogc_aim, w_wallhack, hax_aimbot, ...) and
 * publishes the count in the USERINFO cvar "cod1x_ac" (ROM, so the player cannot fake
 * a clean verdict from his console). This module reads that value for every connected
 * client and reacts.
 *
 * Together with competitive_sv.c (fair-play cvar RANGES from competitive.cfg) this
 * covers what pbsv.cfg actually did for us:
 *      pb_sv_cvar <name> IN min max   -> competitive.cfg  (clamped/locked, no kick)
 *      pb_sv_cvar <cheatname> IN 0 0  -> this module      (detect -> log or kick)
 *
 * *** DEFAULT IS OBSERVE-ONLY (LOG, NO KICK). ***
 * A false positive kicks an innocent player out of a live match, which is worse than
 * missing one cheater. Run it in observe mode first, watch the log over real matches,
 * and only then enable kicking:
 *      COD1RELOADED_CHEATGATE=log     (default) log a line, never kick
 *      COD1RELOADED_CHEATGATE=kick    drop the client
 *      COD1RELOADED_CHEATGATE=0       module off
 *
 * HONEST LIMIT: this is a cvar checker, not a kernel anti-cheat. It catches the
 * ordinary player running a public cheat build (which is what PB caught too). It does
 * NOT catch a recompiled client that reports "clean". The closed ecosystem (protocol
 * 10 + version gate) is what raises that bar. Do not advertise it as unbypassable.
 *
 * Clients that do not report at all (no cod1x_ac key: older mod build, or a client
 * that stripped it) are treated as UNKNOWN and only logged - the version gate is the
 * mechanism meant to require an up-to-date client, not this one.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "cheat_gate.h"

#define TAG "[cheat_gate]"

/* modes */
enum { CG_OFF = 0, CG_LOG = 1, CG_KICK = 2 };
static int g_mode = CG_LOG;

/* per-slot memory so we log a verdict once, not every poll */
#define CG_MAX_CLIENTS 64
static int  g_last_hits[CG_MAX_CLIENTS];
static char g_last_name[CG_MAX_CLIENTS][64];

void cheat_gate_init(void) {
    const char *e = getenv("COD1RELOADED_CHEATGATE");
    if (e && *e) {
        if      (*e == '0' || *e == 'f' || *e == 'F' || *e == 'n' || *e == 'N') g_mode = CG_OFF;
        else if (*e == 'k' || *e == 'K') g_mode = CG_KICK;
        else                              g_mode = CG_LOG;
    }
    for (int i = 0; i < CG_MAX_CLIENTS; i++) { g_last_hits[i] = -1; g_last_name[i][0] = 0; }

    printf("%s mode=%s%s\n", TAG,
           g_mode == CG_OFF ? "off" : (g_mode == CG_KICK ? "KICK" : "log-only"),
           g_mode == CG_LOG ? "  (observe first; set COD1RELOADED_CHEATGATE=kick "
                              "once you trust it)" : "");
    fflush(stdout);
}

int cheat_gate_enabled(void) { return g_mode != CG_OFF; }

/*
 * Called from the existing per-client userinfo sweep in cod1plus.c, which already has
 * the parsed slot + userinfo. `ac` is the raw value of the userinfo key "cod1x_ac"
 * (NULL/empty = the client did not report). Returns 1 if the caller should DROP this
 * client, 0 otherwise.
 */
int cheat_gate_check(int slot, const char *name, const char *ac) {
    if (g_mode == CG_OFF) return 0;
    if (slot < 0 || slot >= CG_MAX_CLIENTS) return 0;

    const char *nm = (name && name[0]) ? name : "?";

    /* no report: unknown client build. Log once, never kick from here. */
    if (!ac || !ac[0]) {
        if (g_last_hits[slot] != -2) {
            g_last_hits[slot] = -2;
            snprintf(g_last_name[slot], sizeof(g_last_name[slot]), "%s", nm);
            printf("%s slot=%d name=%s no cod1x_ac report (old client?) - ignored\n",
                   TAG, slot, nm);
            fflush(stdout);
        }
        return 0;
    }

    int hits = atoi(ac);
    if (hits < 0) hits = 0;

    if (hits == g_last_hits[slot] && !strcmp(g_last_name[slot], nm))
        return 0;                                   /* already handled this verdict */

    g_last_hits[slot] = hits;
    snprintf(g_last_name[slot], sizeof(g_last_name[slot]), "%s", nm);

    if (hits == 0) return 0;                        /* clean */

    if (g_mode == CG_KICK) {
        printf("%s slot=%d name=%s FLAGGED (%d cheat cvar(s)) -> DROP\n",
               TAG, slot, nm, hits);
        fflush(stdout);
        return 1;
    }

    printf("%s slot=%d name=%s FLAGGED (%d cheat cvar(s)) [log-only, not kicked]\n",
           TAG, slot, nm, hits);
    fflush(stdout);
    return 0;
}
