/*
 * cheat_gate.c - server side of the cvar cheat check (PunkBuster replacement, part 2).
 *
 * The client mod (mss32.dll) publishes two USERINFO cvars (ROM, so the player cannot
 * fake a clean verdict from his console); this module reads them for every connected
 * client and reacts:
 *
 *   cod1x_ac  (cheat_scan.cpp) count of blocklisted cheat cvar names that EXIST in the
 *             client's cvar table (ogc_aim, w_wallhack, hax_aimbot, ...).
 *   cod1x_rs  (ruleset.cpp, 1.6.6+) report of the ruleset - the long PunkBuster list
 *             the client downloads (cod1plus/rulesets) or carries compiled in, selected
 *             by the server through the systeminfo cvar sv_competitive_ruleset
 *             (competitive_sv.c). <id> below is "<base>@<version enforced>":
 *               "<id>:ok"                  every rule satisfied
 *               "<id>:v<n>:<cvar>[,<cvar>]" n PERSISTENT violations: a cvar the client
 *                                          fixed that came back out of rule = a client
 *                                          that fights the enforcement (modified build,
 *                                          external tool). This is the kickable case.
 *               "<id>:p<n>:<name>"         n probe hits: a cvar name the stock CoD1
 *                                          never registers exists (cheat config knobs,
 *                                          other games' cvars). LOG ONLY: old configs
 *                                          from Q3-era mods produce false positives.
 *               "<id>:missing"             the client does not carry the ruleset the
 *                                          server asks for (older build). LOG ONLY: the
 *                                          version gate is the tool for that.
 *
 * Together with competitive_sv.c (fair-play cvar RANGES from competitive.cfg) this
 * covers what pbsv.cfg actually did for us:
 *      pb_sv_cvar <name> IN min max   -> competitive.cfg / embedded ruleset (fixed, no kick)
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
 * THREADING: the userinfo sweep (cod1plus.c) runs on its own thread, and SV_DropClient
 * touches the netchan and the game module (ClientDisconnect) - game thread only. So the
 * sweep QUEUES the drop (cheat_gate_request_drop) and competitive_sv.c's G_RunFrame hook
 * executes it (cheat_gate_run_pending), after checking the slot still holds the same
 * player. A drop is then at most one server frame late.
 *
 * HONEST LIMIT: this is a cvar checker, not a kernel anti-cheat. It catches the
 * ordinary player running a public cheat build (which is what PB caught too). It does
 * NOT catch a recompiled client that reports "clean". The closed ecosystem (protocol
 * 10 + version gate) is what raises that bar. Do not advertise it as unbypassable.
 *
 * Clients that do not report at all (no cod1x_ac / cod1x_rs key: older mod build, or a
 * client that stripped it) are treated as UNKNOWN and only logged - the version gate is
 * the mechanism meant to require an up-to-date client, not this one.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>

#include "cheat_gate.h"
#include "cod1_defs.h"

#define TAG "[cheat_gate]"

/* modes */
enum { CG_OFF = 0, CG_LOG = 1, CG_KICK = 2 };
static int g_mode = CG_LOG;

/* per-slot memory so we log a verdict once, not every poll */
#define CG_MAX_CLIENTS 64
static int  g_last_hits[CG_MAX_CLIENTS];
static char g_last_name[CG_MAX_CLIENTS][64];
static char g_last_rs[CG_MAX_CLIENTS][96];

/* drop queue: written by the sweep thread, consumed by the game thread */
static volatile int g_drop_pending[CG_MAX_CLIENTS];
static char         g_drop_name[CG_MAX_CLIENTS][64];
static char         g_drop_why[CG_MAX_CLIENTS][96];

typedef void  (*sv_dropclient_t)(client_t *drop, const char *reason);  /* cdecl, verified */
typedef char *(*info_valueforkey_t)(const char *info, const char *key);

static void forget_slot(int slot) {
    g_last_hits[slot] = -1;
    g_last_name[slot][0] = 0;
    g_last_rs[slot][0] = 0;
}

void cheat_gate_init(void) {
    const char *e = getenv("COD1RELOADED_CHEATGATE");
    if (e && *e) {
        if      (*e == '0' || *e == 'f' || *e == 'F' || *e == 'n' || *e == 'N') g_mode = CG_OFF;
        else if (*e == 'k' || *e == 'K') g_mode = CG_KICK;
        else                              g_mode = CG_LOG;
    }
    for (int i = 0; i < CG_MAX_CLIENTS; i++) {
        forget_slot(i);
        g_drop_pending[i] = 0;
        g_drop_name[i][0] = 0;
        g_drop_why[i][0] = 0;
    }

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

/*
 * Embedded ruleset report. Same call site as cheat_gate_check. Only the "v" (persistent
 * violation) verdict is kickable; probes and a missing ruleset are informative.
 */
int cheat_gate_check_ruleset(int slot, const char *name, const char *rs, const char *want) {
    if (g_mode == CG_OFF) return 0;
    if (slot < 0 || slot >= CG_MAX_CLIENTS) return 0;
    if (!want || !want[0]) return 0;                /* server publishes no ruleset */

    const char *nm = (name && name[0]) ? name : "?";
    if (!rs) rs = "";

    if (!strcmp(g_last_rs[slot], rs) && !strcmp(g_last_name[slot], nm))
        return 0;                                   /* already handled this verdict */
    snprintf(g_last_rs[slot], sizeof(g_last_rs[slot]), "%s", rs);
    snprintf(g_last_name[slot], sizeof(g_last_name[slot]), "%s", nm);

    if (!rs[0]) {
        printf("%s slot=%d name=%s no cod1x_rs report (client older than 1.6.6) - "
               "embedded ruleset %s not enforced for him\n", TAG, slot, nm, want);
        fflush(stdout);
        return 0;
    }

    /* "<base>@<version>:<verdict>" (the version part is optional on both sides) */
    const char *colon = strchr(rs, ':');
    if (!colon) return 0;
    const char *verdict = colon + 1;
    size_t idlen = (size_t)(colon - rs);
    const char *at = memchr(rs, '@', idlen);
    size_t baselen = at ? (size_t)(at - rs) : idlen;
    int client_ver = at ? atoi(at + 1) : 0;
    const char *wat = strchr(want, '@');
    size_t wbaselen = wat ? (size_t)(wat - want) : strlen(want);
    int want_ver = wat ? atoi(wat + 1) : 0;
    int id_ok = (baselen == wbaselen && !strncasecmp(rs, want, baselen));

    if (!strcmp(verdict, "ok")) {
        if (!id_ok)
            printf("%s slot=%d name=%s reports ruleset \"%.*s\" ok but the server asks "
                   "for %s\n", TAG, slot, nm, (int)idlen, rs, want);
        else if (want_ver && client_ver < want_ver)
            printf("%s slot=%d name=%s enforces %.*s v%d, the server asks v%d (his download "
                   "is pending or the file online is behind) [log-only]\n",
                   TAG, slot, nm, (int)baselen, rs, client_ver, want_ver);
        return 0;
    }
    if (!strcmp(verdict, "missing")) {
        printf("%s slot=%d name=%s does not carry ruleset %s (client build too old) "
               "[log-only]\n", TAG, slot, nm, want);
        fflush(stdout);
        return 0;
    }
    if (verdict[0] == 'p') {
        printf("%s slot=%d name=%s ruleset probe hit(s): %s [log-only, never kicked]\n",
               TAG, slot, nm, verdict);
        fflush(stdout);
        return 0;
    }
    if (verdict[0] == 'v') {
        if (g_mode == CG_KICK && id_ok) {
            printf("%s slot=%d name=%s PERSISTENT ruleset violation(s): %s -> DROP\n",
                   TAG, slot, nm, verdict);
            fflush(stdout);
            return 1;
        }
        printf("%s slot=%d name=%s PERSISTENT ruleset violation(s): %s [log-only, "
               "not kicked]\n", TAG, slot, nm, verdict);
        fflush(stdout);
        return 0;
    }
    printf("%s slot=%d name=%s unknown cod1x_rs verdict \"%s\" - ignored\n",
           TAG, slot, nm, rs);
    fflush(stdout);
    return 0;
}

void cheat_gate_request_drop(int slot, const char *name, const char *why) {
    if (slot < 0 || slot >= CG_MAX_CLIENTS) return;
    if (g_drop_pending[slot]) return;
    snprintf(g_drop_name[slot], sizeof(g_drop_name[slot]), "%s", name ? name : "");
    snprintf(g_drop_why[slot], sizeof(g_drop_why[slot]), "%s", why ? why : "");
    __sync_synchronize();
    g_drop_pending[slot] = 1;
}

/* GAME thread. */
void cheat_gate_run_pending(void) {
    for (int slot = 0; slot < CG_MAX_CLIENTS; slot++) {
        if (!g_drop_pending[slot]) continue;
        __sync_synchronize();
        g_drop_pending[slot] = 0;

        serverStatic_t *svs = (serverStatic_t *)ADDR_SVS;
        if (!svs || !svs->clients) continue;
        client_t *cl = (client_t *)((char *)svs->clients + (CLIENT_T_SIZE_V15 * slot));
        clientConnectState_t state = SVSCLIENT_STATE(cl);
        if (state < CS_CONNECTED || state > CS_ACTIVE) { forget_slot(slot); continue; }

        /* the slot may have been handed to someone else since the sweep looked at it:
         * compare the same userinfo "name" the sweep read (engine Info_ValueForKey,
         * static buffer, game thread only - which is where we are) */
        const char *cur = ((info_valueforkey_t)ADDR_INFO_VALUEFORKEY)(
            (const char *)cl + CLIENT_T_OFF_USERINFO, "name");
        if (!cur) cur = "";
        if (g_drop_name[slot][0] && strncmp(cur, g_drop_name[slot], sizeof(g_drop_name[slot]) - 1)) {
            printf("%s slot=%d now holds \"%s\", not \"%s\" - drop skipped\n",
                   TAG, slot, cur, g_drop_name[slot]);
            fflush(stdout);
            forget_slot(slot);
            continue;
        }

        printf("%s slot=%d name=%s DROPPED (%s)\n", TAG, slot, cur, g_drop_why[slot]);
        fflush(stdout);
        ((sv_dropclient_t)ADDR_SV_DROPCLIENT)(cl, "EXE_PLAYERKICKED");
        forget_slot(slot);                          /* a reconnect gets a fresh verdict */
    }
}
