/*
 * match_teams.c - the FPSChallenge teams on each side, for the PAM and the 1.6X streamer overlay.
 *
 * On a cup / ladder server cod1plus.c knows the match (matchdata.cfg: team names and tags, the
 * roster with each player's login), but nothing reaches the mod: matchdata.cfg is never exec'd,
 * so the PAM names the teams from the players' clan tags ("BOT") or "ALLIES" / "AXIS".
 *
 * Once a second, on the game thread, the players on a team VOTE: a roster player of team 1 on
 * allies and a roster player of team 2 on axis both say "team 1 plays allies". A player is known
 * by the login his 1.6X client proves, else by his name in the roster; one not in the roster
 * does not vote. The majority wins; a tie or nobody known keeps the last decisive vote (a round's
 * map_restart empties the teams for a second), before any vote the side tracker of the payload
 * (configured starting side, half-times, the last round event). So the names follow the real
 * sides from the warm-up on, through every half-time, on maps 2 and 3 of a BO3 where the teams
 * choose again - the same rule the payload's scores already rely on.
 *
 * Published (server cvars, set only when they change; GSC reads them with getCvar):
 *   cod1x_team_allies / cod1x_team_axis   name of the team on that side now
 *   cod1x_tag_allies  / cod1x_tag_axis    its tag
 *   cod1x_team1_side                      "allies" / "axis": where team 1 plays now
 * No match config: nothing is touched (an admin may set them by hand).
 * COD1PLUS_TEAMS=0 turns the module off.
 *
 * game.mp.i386.so md5 343f99cd67b79ac74aeaa5261f63c011 (offsets as match_stats.c).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "match_teams.h"
#include "match_stats.h"

#define TAG "[teams]"

#define RVA_G_ENTITIES      0x21d6c0   /* g_entities[1024] */
#define RVA_LEVEL           0x18f220
#define RVA_TRAP_CVAR_SET   0x067ad7   /* trap_Cvar_Set(name, value), as competitive_sv.c */
#define GENTITY_SIZE        0x31c
#define E_CLIENT            0x15c
#define E_INUSE             0x164
#define C_SESS_TEAM         0x2184     /* 1 axis, 2 allies, 3 spectator */
#define C_NAME              0x21bc
#define L_TIME              0x1e8
#define TEAM_AXIS           1
#define TEAM_ALLIES         2
#define MAX_SLOTS           64

typedef void (*trap_cvar_set_t)(const char *, const char *);

static uintptr_t g_base;
static int       g_on = -1;
static int       g_next_ms;            /* level time of the next vote */
static int       g_published;          /* the cvars hold a match */
static char      g_pub[5][136];        /* last values set: allies, axis, tag allies, tag axis, side */
static int       g_last_side = -1;     /* 1 = team 1 on allies, for the log */
static int       g_last_vote = -1;     /* the last decisive vote (kept through map restarts) */

static int enabled(void)
{
    if (g_on < 0) {
        const char *e = getenv("COD1PLUS_TEAMS");
        g_on = !(e && *e == '0');
    }
    return g_on;
}

static char *client_of(int n)
{
    char *e = (char *)(g_base + RVA_G_ENTITIES) + (size_t)n * GENTITY_SIZE;
    if (!*(e + E_INUSE)) return NULL;
    return *(char **)(e + E_CLIENT);
}

/* a cvar value without the characters that would end it in a config line or a command */
static void clean(const char *in, char *out, size_t sz)
{
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < sz; i++) {
        const unsigned char ch = (unsigned char)in[i];
        if (ch < 0x20 || ch == '"' || ch == ';' || ch == '\\') continue;
        out[o++] = (char)ch;
    }
    out[o] = 0;
}

static void set_if_changed(int i, const char *name, const char *value)
{
    if (!strcmp(g_pub[i], value)) return;
    snprintf(g_pub[i], sizeof(g_pub[i]), "%s", value);
    ((trap_cvar_set_t)(g_base + RVA_TRAP_CVAR_SET))(name, value);
}

void match_teams_game_init(uintptr_t game_base)
{
    if (!enabled()) return;
    g_base = game_base;
    g_next_ms = 0;
    /* a new map: the module's cvars survive it, but publish again (a by-hand value may have
     * replaced them meanwhile) */
    memset(g_pub, 0, sizeof(g_pub));
    g_published = 0;
}

void match_teams_frame(void)
{
    char n1[128], g1[32], n2[128], g2[32];
    int now, allies = 0, axis = 0, team1_allies;

    if (!enabled() || !g_base) return;
    now = *(int *)(g_base + RVA_LEVEL + L_TIME);
    if (now < g_next_ms && g_next_ms - now < 5000) return;      /* a map restart winds time back */
    g_next_ms = now + 1000;

    if (!cod1plus_match_team(1, n1, sizeof(n1), g1, sizeof(g1)) ||
        !cod1plus_match_team(2, n2, sizeof(n2), g2, sizeof(g2)))
        return;                                                  /* no FPSChallenge match */

    for (int n = 0; n < MAX_SLOTS; n++) {
        const char *c = client_of(n);
        char login[64], name[40];
        int side, rt;
        if (!c) continue;
        side = *(const int *)(c + C_SESS_TEAM);
        if (side != TEAM_AXIS && side != TEAM_ALLIES) continue;
        snprintf(name, sizeof(name), "%.35s", c + C_NAME);
        cod1plus_client_uuid(n, login, sizeof(login));
        rt = cod1plus_roster_team(login, name);
        if (!rt) continue;
        if ((rt == 1) == (side == TEAM_ALLIES)) allies++; else axis++;
    }
    /* a decisive vote wins; else the last one (every round's map_restart leaves the teams
     * empty for about a second: falling back to the tracker there made the names flicker);
     * the side tracker only before the first vote */
    if (allies != axis) {
        team1_allies = allies > axis;
        g_last_vote = team1_allies;
    } else if (g_last_vote >= 0) {
        team1_allies = g_last_vote;
    } else {
        team1_allies = cod1plus_match_team1_allies();
    }

    {
        char a[136], x[136], ta[40], tx[40];
        clean(team1_allies ? n1 : n2, a, sizeof(a));
        clean(team1_allies ? n2 : n1, x, sizeof(x));
        clean(team1_allies ? g1 : g2, ta, sizeof(ta));
        clean(team1_allies ? g2 : g1, tx, sizeof(tx));
        set_if_changed(0, "cod1x_team_allies", a);
        set_if_changed(1, "cod1x_team_axis", x);
        set_if_changed(2, "cod1x_tag_allies", ta);
        set_if_changed(3, "cod1x_tag_axis", tx);
        set_if_changed(4, "cod1x_team1_side", team1_allies ? "allies" : "axis");
        if (!g_published || team1_allies != g_last_side) {
            printf("%s %s plays allies, %s plays axis (%s: %d vote(s) allies, %d axis)\n", TAG, a, x,
                   allies != axis ? "players" : "side tracker", allies, axis);
            fflush(stdout);
        }
    }
    g_published = 1;
    g_last_side = team1_allies;
}
