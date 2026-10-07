/*
 * match_stats.c - per-player match stats computed by the server module, the zPAM way:
 * score, kills, assists, damage, deaths, grenades, plants, defuses, headshots,
 * entry kills / deaths, aces, clutches 1v1..1v5 won / attempts.
 *
 * WHY. Until now the stats were computed by the mod (PAM's _player_stat port +
 * cod1plus_fps.gsc printing [STATS_EVENT]) and this module only carried them. Here the
 * engine events are read directly, so nothing in maps/mp/gametypes has to know about
 * stats - and the round-based ones zPAM added (entry, ace, clutch) come for free.
 * The rules are zPAM's (_player_stat.gsc + sd.gsc), line for line where it matters.
 *
 * WHERE THE EVENTS COME FROM - game.mp.i386.so md5 343f99cd67b79ac74aeaa5261f63c011:
 *   damage  PlayerCmd_finishPlayerDamage 0x3ec84 (method "finishplayerdamage", table
 *           .data 0x82d70): the one place a player's health goes down, AFTER the gametype
 *           script decided (PAM refuses damage in strat time, after the round end, and has
 *           its own ready-up rules). Health before / after = the HP really lost, capped by
 *           what the victim had - zPAM's AddDamageDealt. The attacker is script argument 1,
 *           read exactly as the method reads it (Scr_GetType, Scr_GetPointerType == 0xd,
 *           Scr_GetEntity).
 *   death   player_die 0x46d56 (ClientSpawn sets ent->die to it): self, inflictor,
 *           attacker, damage, meansOfDeath, weapon, dir, hitLoc. It returns at once for a
 *           player already dead (ps.pm_type > PM_INTERMISSION) - so does the count. A turret
 *           kill is credited to the turret's user (ownerNum), as player_die itself does.
 *   log     the script builtin logPrint (0x5eb86) hands its text to G_LogPrintf from ONE
 *           call, 0x5ec21 `call G_LogPrintf@plt`. That call is re-pointed at a function
 *           that reads the text and calls G_LogPrintf with the very same pointer: what
 *           reaches games_mp.log does not change by a byte, and nothing is re-formatted
 *           (the script's text IS the format string there - a '%' in a player name stays
 *           the engine's business, as before). PAM already writes
 *             RoundStart;          a real round only: sd.gsc `if (!level.in_readyup)
 *                                  thread startRound();` - ready-up never logs it
 *             Winners;<team>;...   round end (_log.gsc logRoundEnd), <team> allies/axis/draw
 *             Bomb;guid;num;team;name;bomb_plant | bomb_defuse
 *           and [STATS_EVENT] (PAM's own numbers: compared, never used).
 *   winner  level.teamScores, axis +0x1fc / allies +0x200 (GScr_SetTeamScore 0x61570):
 *           endRound raises the winner's score in the very frame the round ends.
 *   alive   gclient sessionState +0x20cc (0 = playing), sessionTeam +0x2184 (1 axis,
 *           2 allies) - the client field getters 0x3b73e / 0x3b486.
 *
 * LIFETIME = zPAM's game["playerstats"]: kept across map_restart(true) (every round),
 * cleared on a new map or a map_restart(false) - G_InitGame(.., restart, savepersist).
 * A row belongs to a login uuid when the client sent one (cod1plus.c's collector),
 * otherwise to the in-game name; a reconnect finds its row again either way.
 *
 * OUTPUT. cod1plus_stats.json in the server directory after every round (the same
 * player objects as the FPSChallenge payload / zPAM upload), and one console line per
 * round. Nothing else is written; nothing in the game is changed.
 *
 * Env: COD1PLUS_STATS=0 off, COD1PLUS_STATS_VERBOSE=1 one line per kill / player,
 *      COD1PLUS_STATS_FILE=<path> another output file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "hooks.h"
#include "match_stats.h"

#define TAG "[stats]"

/* ---- game.mp.i386.so md5 343f99cd67b79ac74aeaa5261f63c011 ---------------------------- */
#define RVA_FINISH_DAMAGE   0x03ec84   /* PlayerCmd_finishPlayerDamage */
#define RVA_PLAYER_DIE      0x046d56   /* player_die (.dynsym) */
#define RVA_G_LOGPRINTF     0x04fbb7   /* G_LogPrintf (.dynsym) */
#define RVA_LOGPRINT_CALL   0x05ec21   /* GScr_LogPrint: call G_LogPrintf@plt (e8 1a 56 fb ff) */
#define RVA_PLT_LOGPRINTF   0x014240   /* ... whose target, the PLT entry */
#define RVA_CVAR_STRING     0x067b78   /* trap_Cvar_VariableStringBuffer(name, buf, size) */
#define RVA_SCR_NUMPARAM    0x0720fd   /* Scr_GetNumParam */
#define RVA_SCR_GETTYPE     0x0720b1   /* Scr_GetType */
#define RVA_SCR_PTRTYPE     0x0720d7   /* Scr_GetPointerType */
#define RVA_SCR_GETENTITY   0x0661d1   /* Scr_GetEntity */
#define RVA_G_ENTITIES      0x21d6c0   /* g_entities[1024] */
#define RVA_LEVEL           0x18f220   /* level */

#define GENTITY_SIZE    0x31c
#define E_ETYPE         0x004
#define E_OWNERNUM      0x150          /* r.ownerNum (turret user) */
#define E_CLIENT        0x15c
#define E_INUSE         0x164
#define E_HEALTH        0x238
#define C_PM_TYPE       0x004
#define C_SESS_STATE    0x20cc
#define C_SESS_TEAM     0x2184
#define C_NAME          0x21bc
#define L_TIME          0x1e8
#define L_SCORE_AXIS    0x1fc
#define L_SCORE_ALLIES  0x200

#define TEAM_AXIS           1
#define TEAM_ALLIES         2
#define MOD_GRENADE_SPLASH  4
#define MOD_HEAD_SHOT       8
#define MOD_SUICIDE         22
#define ET_TURRET           11
#define PM_INTERMISSION     5
#define MAX_GENTITIES       1024
#define SCR_ENTITY_PTR      0xd

#define MAX_SLOTS       64
#define MAX_REC         128
#define CL_MAX          16     /* 1vN tracked up to 1v16 */
#define CL_OUT          5      /* 1v1..1v5 always written, bigger ones only if they happened */
#define MIN_TEAM_SIZE   2      /* zPAM level.stat_round_min_team_size: no entry/ace/clutch in a 1v1 match */
#define ASSIST_MS       5000

static const uint8_t PRO_DAMAGE[5]  = { 0x55, 0x89, 0xe5, 0x56, 0x53 };
static const uint8_t PRO_DIE[7]     = { 0x55, 0x89, 0xe5, 0x53, 0x83, 0xec, 0x64 };

typedef void  (*finish_damage_t)(int entnum);
typedef void  (*player_die_t)(char *self, char *inflictor, char *attacker, int damage,
                              int mod, int weapon, const float *dir, int hitloc);
typedef void  (*logprintf_t)(const char *fmt, ...);
typedef void  (*cvar_string_t)(const char *name, char *buf, int size);
typedef int   (*scr_numparam_t)(void);
typedef int   (*scr_type_t)(unsigned int index);
typedef char *(*scr_entity_t)(unsigned int index);

typedef struct {
    int  used;
    char uuid[64];               /* login uuid, "" = none known */
    char name[64];               /* last in-game name */
    char norm[64];               /* name without colours / spaces, lower case */
    int  side;                   /* team in his last live round: 1 axis, 2 allies */
    int  played;                 /* on a team during a live round at least once */
    int  score2;                 /* score in half points (assist / plant / defuse = 0.5) */
    int  kills, assists, damage, deaths, grenades, plants, defuses, headshots;
    int  entry_kills, entry_deaths, aces;
    int  clutch_won[CL_MAX + 1], clutch_att[CL_MAX + 1];
    int  round_kills, round_kills_team;
} rec_t;

typedef struct {
    int rec;                     /* row of the client in this slot, -1 = not bound yet */
    int att1, att1_time;         /* last enemy who hurt him (row) and when (level.time) */
    int att2, att2_time;         /* the one before him: the assist candidate */
    int pend_switch, pend_team, pend_rec;  /* MOD_SUICIDE death, undone if his team changes */
    char seen_uuid[64];          /* login read last frame: adopted once read the same twice */
} slot_t;

static int       g_enabled = -1;
static int       g_verbose = 0;
static char      g_json_path[256] = "cod1plus_stats.json";
static int       g_on = 0;                 /* hooks live on g_base */
static uintptr_t g_base = 0;
static hook_t    g_h_damage, g_h_die;
static finish_damage_t orig_finish_damage;
static player_die_t    orig_player_die;
static logprintf_t     real_logprintf;
static scr_numparam_t  p_numparam;
static scr_type_t      p_gettype, p_ptrtype;
static scr_entity_t    p_getentity;

static rec_t  g_rec[MAX_REC];
static slot_t g_slot[MAX_SLOTS];
static char   g_map[64];
static int    g_rounds;                     /* rounds finished on this map */

static int r_active;                        /* RoundStart; seen, end not yet */
static int r_entry_done;
static int r_size[3];                       /* max alive seen this round, per team */
static int r_clutch[3], r_clutch_enemies[3];
static int r_snap_axis, r_snap_allies;

/* ------------------------------------------------------------------------------------- */
/* engine reads                                                                          */
/* ------------------------------------------------------------------------------------- */

static char *ent(int n) { return (char *)(g_base + RVA_G_ENTITIES) + (size_t)n * GENTITY_SIZE; }

static char *client_of(int n)               /* gclient of a connected slot, or NULL */
{
    if (n < 0 || n >= MAX_SLOTS) return NULL;
    char *e = ent(n);
    if (!*(e + E_INUSE)) return NULL;
    return *(char **)(e + E_CLIENT);
}

static int slot_of_ent(const char *e)       /* client slot of an entity pointer, or -1 */
{
    uintptr_t b = g_base + RVA_G_ENTITIES;
    if (!e || (uintptr_t)e < b) return -1;
    uintptr_t d = (uintptr_t)e - b;
    if (d % GENTITY_SIZE) return -1;
    int n = (int)(d / GENTITY_SIZE);
    return client_of(n) ? n : -1;
}

static int team_of(int n)
{
    char *c = client_of(n);
    if (!c) return 0;
    int t = *(int *)(c + C_SESS_TEAM);
    return (t == TEAM_AXIS || t == TEAM_ALLIES) ? t : 0;
}

static int is_alive(int n)
{
    char *c = client_of(n);
    return c && *(int *)(c + C_SESS_STATE) == 0;
}

static int level_int(int off) { return *(int *)(g_base + RVA_LEVEL + off); }

static const char *team_name(int t) { return t == TEAM_AXIS ? "axis" : t == TEAM_ALLIES ? "allies" : "none"; }

static void read_name(int n, char *out, size_t sz)
{
    const char *c = client_of(n);
    size_t i = 0;
    if (c) {
        const char *s = c + C_NAME;
        for (; i + 1 < sz && i < 35 && s[i]; i++)
            out[i] = ((unsigned char)s[i] < 0x20) ? '?' : s[i];
    }
    out[i] = 0;
    if (!out[0]) snprintf(out, sz, "client%d", n);
}

/* same normalisation as cod1plus.c name_normalize: colour codes and whitespace out, lower case */
static void name_norm(const char *in, char *out, size_t sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < sz; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '^' && in[i + 1]) { i++; continue; }
        if (ch <= 0x20) continue;
        out[o++] = (char)tolower(ch);
    }
    out[o] = 0;
}

static int parse_int(const char *s)         /* no atoi: see glibc_compat.h */
{
    int neg = 0, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* "12.5" -> 25 half points */
static int parse_half(const char *s)
{
    int neg = 0, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    v *= 2;
    if (*s == '.' && s[1] >= '5' && s[1] <= '9') v += 1;
    return neg ? -v : v;
}

static void score_str(int s2, char *out, size_t sz)
{
    int a = s2 < 0 ? -s2 : s2;
    snprintf(out, sz, "%s%d.%d", s2 < 0 ? "-" : "", a / 2, (a % 2) ? 5 : 0);
}

/* ------------------------------------------------------------------------------------- */
/* rows                                                                                  */
/* ------------------------------------------------------------------------------------- */

static void slot_clear(int n)
{
    memset(&g_slot[n], 0, sizeof(g_slot[n]));
    g_slot[n].rec = g_slot[n].att1 = g_slot[n].att2 = g_slot[n].pend_rec = -1;
}

static int rec_new(void)
{
    for (int r = 0; r < MAX_REC; r++) {
        if (g_rec[r].used) continue;
        memset(&g_rec[r], 0, sizeof(g_rec[r]));
        g_rec[r].used = 1;
        return r;
    }
    static int warned = 0;
    if (!warned) { warned = 1; printf("%s row table full (%d players this map)\n", TAG, MAX_REC); fflush(stdout); }
    return -1;
}

static int rec_bound_elsewhere(int r, int slot)
{
    for (int j = 0; j < MAX_SLOTS; j++)
        if (j != slot && g_slot[j].rec == r && client_of(j)) return 1;
    return 0;
}

static int rec_find_uuid(const char *uuid)
{
    for (int r = 0; r < MAX_REC; r++)
        if (g_rec[r].used && g_rec[r].uuid[0] && strcmp(g_rec[r].uuid, uuid) == 0) return r;
    return -1;
}

/* a row with that name nobody connected holds, without a login or with this one */
static int rec_find_name(const char *norm, const char *uuid, int slot)
{
    for (int r = 0; r < MAX_REC; r++) {
        if (!g_rec[r].used || strcmp(g_rec[r].norm, norm) != 0) continue;
        if (g_rec[r].uuid[0] && (!uuid[0] || strcmp(g_rec[r].uuid, uuid) != 0)) continue;
        if (rec_bound_elsewhere(r, slot)) continue;
        return r;
    }
    return -1;
}

/* the client of a name-only row turns out to have a login that already owns a row */
static void rec_merge(int dst, int src)
{
    rec_t *d = &g_rec[dst], *s = &g_rec[src];
    d->played |= s->played;
    if (!d->side) d->side = s->side;
    d->score2 += s->score2;       d->kills += s->kills;       d->assists += s->assists;
    d->damage += s->damage;       d->deaths += s->deaths;     d->grenades += s->grenades;
    d->plants += s->plants;       d->defuses += s->defuses;   d->headshots += s->headshots;
    d->entry_kills += s->entry_kills; d->entry_deaths += s->entry_deaths; d->aces += s->aces;
    for (int i = 0; i <= CL_MAX; i++) { d->clutch_won[i] += s->clutch_won[i]; d->clutch_att[i] += s->clutch_att[i]; }
    d->round_kills += s->round_kills;
    if (s->round_kills_team) d->round_kills_team = s->round_kills_team;
    for (int j = 0; j < MAX_SLOTS; j++) {
        if (g_slot[j].rec == src) g_slot[j].rec = dst;
        if (g_slot[j].att1 == src) g_slot[j].att1 = dst;
        if (g_slot[j].att2 == src) g_slot[j].att2 = dst;
        if (g_slot[j].pend_rec == src) g_slot[j].pend_rec = dst;
    }
    for (int t = 1; t <= 2; t++) if (r_clutch[t] == src) r_clutch[t] = dst;
    memset(s, 0, sizeof(*s));
}

static int bind_slot(int n)
{
    char name[64], norm[64], uuid[64];
    if (!client_of(n)) { slot_clear(n); return -1; }
    read_name(n, name, sizeof(name));
    name_norm(name, norm, sizeof(norm));
    uuid[0] = 0;
    cod1plus_client_uuid(n, uuid, sizeof(uuid));
    if (strcmp(uuid, g_slot[n].seen_uuid) != 0) {
        /* cod1plus.c's collector thread writes that table: never act on a half-written login */
        snprintf(g_slot[n].seen_uuid, sizeof(g_slot[n].seen_uuid), "%s", uuid);
        uuid[0] = 0;
    }

    int r = g_slot[n].rec;
    if (r >= 0 && !g_rec[r].used) r = -1;
    if (r >= 0 && uuid[0] && g_rec[r].uuid[0] && strcmp(g_rec[r].uuid, uuid) != 0)
        r = -1;                                   /* another login in this slot */
    if (r >= 0 && uuid[0] && !g_rec[r].uuid[0]) {  /* the login arrived after the name */
        int o = rec_find_uuid(uuid);
        if (o >= 0 && o != r) { rec_merge(o, r); r = o; }
        else snprintf(g_rec[r].uuid, sizeof(g_rec[r].uuid), "%s", uuid);
    }
    if (r < 0) {
        if (uuid[0]) r = rec_find_uuid(uuid);
        if (r < 0) r = rec_find_name(norm, uuid, n);
        if (r < 0) r = rec_new();
        if (r < 0) return -1;
        if (uuid[0] && !g_rec[r].uuid[0]) snprintf(g_rec[r].uuid, sizeof(g_rec[r].uuid), "%s", uuid);
    }
    snprintf(g_rec[r].name, sizeof(g_rec[r].name), "%s", name);
    snprintf(g_rec[r].norm, sizeof(g_rec[r].norm), "%s", norm);
    g_slot[n].rec = r;
    return r;
}

static int R(int n)                             /* row of slot n, bound on demand */
{
    if (n < 0 || n >= MAX_SLOTS) return -1;
    int r = g_slot[n].rec;
    return (r >= 0 && g_rec[r].used) ? r : bind_slot(n);
}

static void reset_map(void)
{
    memset(g_rec, 0, sizeof(g_rec));
    for (int n = 0; n < MAX_SLOTS; n++) slot_clear(n);
    g_rounds = 0;
    r_active = 0;
}

/* ------------------------------------------------------------------------------------- */
/* rounds (zPAM _player_stat.gsc RoundStarted / AddRoundKill / UpdateClutch / RoundEnded) */
/* ------------------------------------------------------------------------------------- */

/* alive players per team (and one of them), raising the round's team sizes */
static void count_alive(int alive[3], int last[3])
{
    alive[0] = alive[1] = alive[2] = 0;
    last[0] = last[1] = last[2] = -1;
    for (int n = 0; n < MAX_SLOTS; n++) {
        int t = team_of(n);
        if (!t || !is_alive(n)) continue;
        alive[t]++;
        last[t] = n;
        if (r_active) {
            int r = R(n);
            if (r >= 0) { g_rec[r].played = 1; g_rec[r].side = t; }
        }
    }
    for (int t = 1; t <= 2; t++) if (alive[t] > r_size[t]) r_size[t] = alive[t];
}

static void round_start(void)
{
    int alive[3], last[3];
    if (r_active) {
        printf("%s round start while round %d was still open (no end seen) - its clutches/aces dropped\n",
               TAG, g_rounds + 1);
        fflush(stdout);
    }
    r_active = 1;
    r_entry_done = 0;
    for (int t = 0; t < 3; t++) { r_size[t] = 0; r_clutch[t] = -1; r_clutch_enemies[t] = 0; }
    r_snap_axis = level_int(L_SCORE_AXIS);
    r_snap_allies = level_int(L_SCORE_ALLIES);
    for (int r = 0; r < MAX_REC; r++) { g_rec[r].round_kills = 0; g_rec[r].round_kills_team = 0; }
    for (int n = 0; n < MAX_SLOTS; n++) {
        g_slot[n].att1 = g_slot[n].att2 = -1;
        g_slot[n].pend_switch = 0;
    }
    count_alive(alive, last);
    if (g_verbose) { printf("%s round %d start (score axis %d allies %d)\n", TAG, g_rounds + 1, r_snap_axis, r_snap_allies); fflush(stdout); }
}

/* a player becomes the last one alive of his team while enemies are left: 1vX attempt */
static void update_clutch(void)
{
    int alive[3], last[3];
    count_alive(alive, last);
    for (int t = 1; t <= 2; t++) {
        int e = (t == TEAM_AXIS) ? TEAM_ALLIES : TEAM_AXIS;
        if (r_clutch[t] >= 0 || r_size[t] < MIN_TEAM_SIZE) continue;
        if (alive[t] != 1 || alive[e] < 1) continue;
        int r = R(last[t]);
        if (r < 0) continue;
        r_clutch[t] = r;
        r_clutch_enemies[t] = alive[e];
        if (g_verbose) { printf("%s clutch: %s alone vs %d\n", TAG, g_rec[r].name, alive[e]); fflush(stdout); }
    }
}

static void write_json(void);

static void round_end(int winner, const char *how)
{
    if (!r_active) return;
    r_active = 0;
    char clutch_txt[160] = "", ace_txt[160] = "";

    for (int t = 1; t <= 2; t++) {
        int r = r_clutch[t];
        if (r < 0 || !g_rec[r].used) continue;
        int n = r_clutch_enemies[t];
        if (n < 1) continue;
        if (n > CL_MAX) n = CL_MAX;
        g_rec[r].clutch_att[n]++;
        if (winner == t) g_rec[r].clutch_won[n]++;
        size_t l = strlen(clutch_txt);
        snprintf(clutch_txt + l, sizeof(clutch_txt) - l, "%s%s 1v%d %s", l ? ", " : "",
                 g_rec[r].name, n, winner == t ? "won" : "lost");
    }
    for (int r = 0; r < MAX_REC; r++) {
        rec_t *p = &g_rec[r];
        if (!p->used || !p->round_kills_team) continue;
        int enemies = r_size[p->round_kills_team];
        if (enemies >= MIN_TEAM_SIZE && p->round_kills >= enemies) {
            p->aces++;
            size_t l = strlen(ace_txt);
            snprintf(ace_txt + l, sizeof(ace_txt) - l, "%s%s", l ? ", " : "", p->name);
        }
    }
    g_rounds++;
    write_json();
    printf("%s round %d over, winner %s (%s), %dv%d: clutch %s, ace %s -> %s\n", TAG, g_rounds,
           team_name(winner), how, r_size[TEAM_ALLIES], r_size[TEAM_AXIS],
           clutch_txt[0] ? clutch_txt : "-", ace_txt[0] ? ace_txt : "-", g_json_path);
    if (g_verbose) {
        for (int r = 0; r < MAX_REC; r++) {
            rec_t *p = &g_rec[r];
            char sc[16];
            if (!p->used || !p->played) continue;
            score_str(p->score2, sc, sizeof(sc));
            printf("%s   %-20s %-6s score %s K %d D %d A %d dmg %d hs %d nade %d plant %d defuse %d "
                   "entry %d/%d ace %d\n", TAG, p->name, team_name(p->side), sc, p->kills, p->deaths,
                   p->assists, p->damage, p->headshots, p->grenades, p->plants, p->defuses,
                   p->entry_kills, p->entry_deaths, p->aces);
        }
    }
    fflush(stdout);
}

static void poll_scores(void)
{
    int ax = level_int(L_SCORE_AXIS), al = level_int(L_SCORE_ALLIES);
    if (ax == r_snap_axis && al == r_snap_allies) return;
    int dax = ax - r_snap_axis, dal = al - r_snap_allies;
    r_snap_axis = ax;
    r_snap_allies = al;
    if (dax > 0 && dal == 0) round_end(TEAM_AXIS, "team score");
    else if (dal > 0 && dax == 0) round_end(TEAM_ALLIES, "team score");
    /* anything else (both moved, a score went down): the Winners; line decides */
}

/* ------------------------------------------------------------------------------------- */
/* events                                                                                */
/* ------------------------------------------------------------------------------------- */

static void on_death(int v, int vteam, int a, int ateam, int mod)
{
    if (!r_active) return;
    int rv = R(v);
    if (rv < 0) return;
    rec_t *V = &g_rec[rv];
    V->deaths++;
    V->played = 1;
    if (vteam) V->side = vteam;
    if (mod == MOD_SUICIDE && (a < 0 || a == v)) {
        /* PAM's team change is `self suicide()` then a new sessionteam, same frame:
         * zPAM does not count that death (self.switching_teams) - undone in the frame tick */
        g_slot[v].pend_switch = 1;
        g_slot[v].pend_team = vteam;
        g_slot[v].pend_rec = rv;
    }
    if (a < 0 || a == v || !ateam || !vteam) return;
    int ra = R(a);
    if (ra < 0) return;
    rec_t *A = &g_rec[ra];
    A->played = 1;
    A->side = ateam;
    if (ateam == vteam) {                         /* team kill */
        A->score2 -= 2;
        if (g_verbose) { printf("%s team kill %s -> %s\n", TAG, A->name, V->name); fflush(stdout); }
        return;
    }
    A->kills++;
    A->score2 += 2;
    if (mod == MOD_HEAD_SHOT) A->headshots++;
    if (mod == MOD_GRENADE_SPLASH) A->grenades++;

    A->round_kills++;
    A->round_kills_team = vteam;
    int entry = 0;
    if (!r_entry_done && r_size[TEAM_AXIS] >= MIN_TEAM_SIZE && r_size[TEAM_ALLIES] >= MIN_TEAM_SIZE) {
        r_entry_done = 1;
        entry = 1;
        A->entry_kills++;
        V->entry_deaths++;
    }
    const slot_t *s = &g_slot[v];
    int assist = -1;
    if (s->att1 >= 0 && s->att2 >= 0 && s->att2 != ra && s->att2_time + ASSIST_MS > level_int(L_TIME)
        && g_rec[s->att2].used) {
        assist = s->att2;
        g_rec[assist].assists++;
        g_rec[assist].score2 += 1;
    }
    if (g_verbose) {
        printf("%s kill %s -> %s mod %d%s%s%s\n", TAG, A->name, V->name, mod,
               entry ? " ENTRY" : "", assist >= 0 ? " assist " : "", assist >= 0 ? g_rec[assist].name : "");
        fflush(stdout);
    }
}

static void on_bomb(const char *line)
{
    /* Bomb;<guid>;<num>;<team>;<name>;bomb_plant|bomb_defuse - the name may hold ';' */
    const char *q = strchr(line + 5, ';');
    const char *act = strrchr(line, ';');
    if (!q || !act) return;
    int num = parse_int(q + 1);
    act++;
    int plant = strncmp(act, "bomb_plant", 10) == 0, defuse = strncmp(act, "bomb_defuse", 11) == 0;
    if ((!plant && !defuse) || !r_active) return;
    int r = R(num);
    if (r < 0) return;
    if (plant) g_rec[r].plants++; else g_rec[r].defuses++;
    g_rec[r].score2 += 1;
    g_rec[r].played = 1;
    if (g_verbose) { printf("%s %s by %s\n", TAG, plant ? "plant" : "defuse", g_rec[r].name); fflush(stdout); }
}

static int find_rec_by_name(const char *name)
{
    char norm[64];
    name_norm(name, norm, sizeof(norm));
    for (int r = 0; r < MAX_REC; r++)
        if (g_rec[r].used && strcmp(g_rec[r].norm, norm) == 0) return r;
    return -1;
}

/* PAM's [STATS_EVENT] (cod1plus_fps.gsc): ps=name:team:kills:deaths:assists:damage:grenades:
 * plants:defuses:score[:hs:gd:adr[:slot]]|... - same counters, computed by the script. Read
 * only to say whether the two agree. Assists are EXPECTED to differ: PAM gives the assist to
 * the biggest single hit (>= 41 damage, no time limit, sd.gsc markPossibleAssist), zPAM - and
 * this module - to the enemy who hit him before the killer did, within 5 s; the score moves
 * with them by 0.5 each. Anything else differing gets its own line. */
static void compare_pam(const char *line)
{
    const char *p = strstr(line, "ps=");
    int compared = 0, same = 0, assists_only = 0, other = 0;
    if (!p) return;
    p += 3;
    while (*p && *p != '\n') {
        char seg[512];
        size_t l = strcspn(p, "|\n");
        if (l >= sizeof(seg)) l = sizeof(seg) - 1;
        memcpy(seg, p, l);
        seg[l] = 0;
        p += l;
        if (*p == '|') p++;

        char *f[16];
        int nf = 0;
        f[nf++] = seg;
        for (char *c = seg; *c && nf < 16; c++)
            if (*c == ':') { *c = 0; f[nf++] = c + 1; }
        if (nf < 10) continue;
        int slot = (nf >= 14) ? parse_int(f[13]) : -1;
        int r = (slot >= 0 && slot < MAX_SLOTS) ? g_slot[slot].rec : -1;
        if (r < 0 || !g_rec[r].used) r = find_rec_by_name(f[0]);
        if (r < 0) {
            int pk = parse_int(f[2]), pd = parse_int(f[3]);
            if (pk || pd) { printf("%s vs PAM: %s (K %d D %d) has no row here\n", TAG, f[0], pk, pd); }
            continue;
        }
        const rec_t *o = &g_rec[r];
        int pk = parse_int(f[2]), pd = parse_int(f[3]), pa = parse_int(f[4]), pg = parse_int(f[6]);
        int pp = parse_int(f[7]), pf = parse_int(f[8]), ps2 = parse_half(f[9]);
        int ph = (nf >= 11) ? parse_int(f[10]) : o->headshots;
        int pdmg = parse_int(f[5]);
        compared++;
        int core = pk == o->kills && pd == o->deaths && pg == o->grenades && pp == o->plants &&
                   pf == o->defuses && ph == o->headshots && pdmg == o->damage &&
                   ps2 - pa == o->score2 - o->assists;      /* score apart from the assists */
        if (core && pa == o->assists) same++;
        else if (core) assists_only++;
        else other++;
        if (core && !g_verbose) continue;
        char s_me[16], s_pam[16];
        score_str(o->score2, s_me, sizeof(s_me));
        score_str(ps2, s_pam, sizeof(s_pam));
        printf("%s vs PAM: %s here/PAM K %d/%d D %d/%d A %d/%d HS %d/%d nade %d/%d plant %d/%d "
               "defuse %d/%d damage %d/%d score %s/%s%s\n", TAG, o->name, o->kills, pk, o->deaths, pd,
               o->assists, pa, o->headshots, ph, o->grenades, pg, o->plants, pp, o->defuses, pf,
               o->damage, pdmg, s_me, s_pam, core ? "" : "  <-- DIFFERS");
    }
    printf("%s vs PAM [STATS_EVENT]: %d players, %d identical, %d differ by the assist rule only, "
           "%d differ otherwise\n", TAG, compared, same, assists_only, other);
    fflush(stdout);
}

static void on_log_line(const char *line)
{
    if (strncmp(line, "RoundStart;", 11) == 0) {
        round_start();
    } else if (strncmp(line, "Winners;", 8) == 0) {
        const char *w = line + 8;
        int winner = strncmp(w, "allies", 6) == 0 ? TEAM_ALLIES : strncmp(w, "axis", 4) == 0 ? TEAM_AXIS : 0;
        round_end(winner, "Winners; line");
    } else if (strncmp(line, "Bomb;", 5) == 0) {
        on_bomb(line);
    } else if (strstr(line, "[STATS_EVENT]")) {
        compare_pam(line);
    }
}

/* ------------------------------------------------------------------------------------- */
/* hooks                                                                                 */
/* ------------------------------------------------------------------------------------- */

static int script_attacker(void)
{
    if (p_numparam() < 2) return -1;
    if (!p_gettype(1) || p_ptrtype(1) != SCR_ENTITY_PTR) return -1;
    return slot_of_ent(p_getentity(1));
}

static void hook_finish_damage(int entnum)
{
    int v = -1, a = -1, before = 0;
    if (g_on && r_active && entnum >= 0 && entnum < MAX_SLOTS && client_of(entnum)) {
        before = *(int *)(ent(entnum) + E_HEALTH);
        int at = script_attacker();
        int tv = team_of(entnum), ta = team_of(at);
        if (before > 0 && at >= 0 && at != entnum && tv && ta && tv != ta) {
            int ra = R(at);
            if (ra >= 0) {
                /* zPAM onPlayerDamaged: done BEFORE finishPlayerDamage, so the killing hit's
                 * author is already lastAttacker when the kill is counted */
                slot_t *s = &g_slot[entnum];
                if (s->att1 >= 0 && s->att1 != ra) { s->att2 = s->att1; s->att2_time = s->att1_time; }
                s->att1 = ra;
                s->att1_time = level_int(L_TIME);
                v = entnum;
                a = at;
            }
        }
    }
    orig_finish_damage(entnum);
    if (v >= 0) {
        int after = *(int *)(ent(v) + E_HEALTH);
        int dealt = before - (after > 0 ? after : 0);
        int ra = R(a);
        if (dealt > 0 && ra >= 0) g_rec[ra].damage += dealt;
    }
}

static void hook_player_die(char *self, char *inflictor, char *attacker, int damage, int mod,
                            int weapon, const float *dir, int hitloc)
{
    int v = -1, vteam = 0, a = -1, ateam = 0;
    if (g_on) {
        int n = slot_of_ent(self);
        char *c = client_of(n);
        if (c && *(int *)(c + C_PM_TYPE) <= PM_INTERMISSION) {   /* alive: a 2nd death is ignored */
            const char *at = attacker;
            if (at && *(const int *)(at + E_ETYPE) == ET_TURRET) {
                int own = *(const int *)(at + E_OWNERNUM);
                if (own >= 0 && own < MAX_GENTITIES - 1) at = ent(own);
            }
            v = n;
            vteam = team_of(n);
            a = slot_of_ent(at);
            ateam = team_of(a);
            if (r_active) { int alive[3], last[3]; count_alive(alive, last); }  /* victim still alive */
        }
    }
    orig_player_die(self, inflictor, attacker, damage, mod, weapon, dir, hitloc);
    if (v >= 0) on_death(v, vteam, a, ateam, mod);
}

/* GScr_LogPrint's call: the script's text, which G_LogPrintf takes as its format */
static void hook_script_logprint(const char *text)
{
    if (g_on && text) on_log_line(text);
    real_logprintf(text);
}

static void install(uintptr_t base)
{
    static int logged = 0;
    uint8_t *fd = (uint8_t *)(base + RVA_FINISH_DAMAGE);
    uint8_t *pd = (uint8_t *)(base + RVA_PLAYER_DIE);
    uint8_t *cs = (uint8_t *)(base + RVA_LOGPRINT_CALL);
    uintptr_t call_to = (cs[0] == 0xE8) ? (uintptr_t)cs + 5 + (uintptr_t)(intptr_t)*(int32_t *)(cs + 1) : 0;

    if (g_on && base == g_base && fd[0] == 0xE9 && pd[0] == 0xE9 &&
        call_to == (uintptr_t)hook_script_logprint) return;
    g_on = 0;
    if (memcmp(fd, PRO_DAMAGE, sizeof(PRO_DAMAGE)) || memcmp(pd, PRO_DIE, sizeof(PRO_DIE)) ||
        call_to != base + RVA_PLT_LOGPRINTF) {
        if (!logged) {
            logged = 1;
            printf("%s code mismatch (%02x %02x / %02x %02x / %02x) - different game.mp.i386.so "
                   "build? stats off\n", TAG, fd[0], fd[1], pd[0], pd[1], cs[0]);
            fflush(stdout);
        }
        return;
    }
    g_base = base;
    p_numparam  = (scr_numparam_t)(base + RVA_SCR_NUMPARAM);
    p_gettype   = (scr_type_t)(base + RVA_SCR_GETTYPE);
    p_ptrtype   = (scr_type_t)(base + RVA_SCR_PTRTYPE);
    p_getentity = (scr_entity_t)(base + RVA_SCR_GETENTITY);
    real_logprintf = (logprintf_t)(base + RVA_G_LOGPRINTF);   /* the body: sets its own ebx */
    {
        char map[64] = "";
        ((cvar_string_t)(base + RVA_CVAR_STRING))("mapname", map, sizeof(map));
        if (map[0]) snprintf(g_map, sizeof(g_map), "%s", map);
    }
    /* game thread, before G_InitGame: none of these can be running */
    if (hook_unprotect((uintptr_t)cs, 5) != 0) goto fail;
    *(int32_t *)(cs + 1) = (int32_t)((uintptr_t)hook_script_logprint - ((uintptr_t)cs + 5));
    if (hook_install(&g_h_die, (uintptr_t)pd, (uintptr_t)hook_player_die, sizeof(PRO_DIE)) != 0) goto fail;
    orig_player_die = (player_die_t)g_h_die.trampoline;
    if (hook_install(&g_h_damage, (uintptr_t)fd, (uintptr_t)hook_finish_damage, sizeof(PRO_DAMAGE)) != 0) goto fail;
    orig_finish_damage = (finish_damage_t)g_h_damage.trampoline;
    g_on = 1;
    logged = 0;
    printf("%s installed (game base 0x%08lx, map %s): damage / death / logPrint -> %s after every round\n",
           TAG, (unsigned long)base, g_map[0] ? g_map : "?", g_json_path);
    fflush(stdout);
    return;
fail:
    printf("%s hook_install failed - stats off\n", TAG);
    fflush(stdout);
}

/* ------------------------------------------------------------------------------------- */
/* output                                                                                */
/* ------------------------------------------------------------------------------------- */

static void json_esc(const char *in, char *out, size_t sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < sz; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch < 0x20) o += (size_t)snprintf(out + o, sz - o, "\\u%04x", ch);
        else out[o++] = (char)ch;
    }
    out[o] = 0;
}

static void write_json(void)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_json_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        static int warned = 0;
        if (!warned) { warned = 1; printf("%s cannot write %s\n", TAG, tmp); }
        return;
    }
    char map[96];
    json_esc(g_map, map, sizeof(map));
    fprintf(f, "{\n  \"map\": \"%s\",\n  \"rounds\": \"%d\",\n  \"players\": [", map, g_rounds);
    int first = 1;
    for (int r = 0; r < MAX_REC; r++) {
        const rec_t *p = &g_rec[r];
        if (!p->used || !p->played) continue;
        char uuid[96], label[16], tname[160], e_uuid[192], e_name[160], e_tname[320], sc[16];
        cod1plus_player_identity(p->uuid, p->name, p->side == TEAM_ALLIES,
                                 uuid, sizeof(uuid), label, sizeof(label), tname, sizeof(tname));
        json_esc(uuid, e_uuid, sizeof(e_uuid));
        json_esc(p->name, e_name, sizeof(e_name));
        json_esc(tname, e_tname, sizeof(e_tname));
        score_str(p->score2, sc, sizeof(sc));
        fprintf(f, "%s\n    {\n"
                   "      \"key\": \"UUID_%s\",\n      \"uuid\": \"%s\",\n      \"name\": \"%s\",\n"
                   "      \"team\": \"%s\",\n      \"team_name\": \"%s\",\n      \"score\": \"%s\",\n"
                   "      \"kills\": \"%d\",\n      \"assists\": \"%d\",\n      \"damage\": \"%d\",\n"
                   "      \"deaths\": \"%d\",\n      \"grenades\": \"%d\",\n      \"plants\": \"%d\",\n"
                   "      \"defuses\": \"%d\",\n      \"headshots\": \"%d\",\n"
                   "      \"entry_kills\": \"%d\",\n      \"entry_deaths\": \"%d\",\n      \"aces\": \"%d\"",
                first ? "" : ",", e_uuid, e_uuid, e_name, label, e_tname, sc,
                p->kills, p->assists, p->damage, p->deaths, p->grenades, p->plants, p->defuses,
                p->headshots, p->entry_kills, p->entry_deaths, p->aces);
        for (int c = 1; c <= CL_MAX; c++) {
            if (c > CL_OUT && !p->clutch_att[c]) continue;
            fprintf(f, ",\n      \"clutches_1v%d\": \"%d\",\n      \"clutches_1v%d_attempts\": \"%d\"",
                    c, p->clutch_won[c], c, p->clutch_att[c]);
        }
        fprintf(f, "\n    }");
        first = 0;
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
    rename(tmp, g_json_path);
}

/* ------------------------------------------------------------------------------------- */
/* entry points                                                                          */
/* ------------------------------------------------------------------------------------- */

int match_stats_enabled(void)
{
    if (g_enabled < 0) {
        const char *e = getenv("COD1PLUS_STATS");
        g_enabled = !(e && (*e == '0' || *e == 'f' || *e == 'F' || *e == 'n' || *e == 'N'));
        const char *v = getenv("COD1PLUS_STATS_VERBOSE");
        g_verbose = (v && *v == '1');
        const char *p = getenv("COD1PLUS_STATS_FILE");
        if (p && *p) snprintf(g_json_path, sizeof(g_json_path), "%s", p);
        for (int n = 0; n < MAX_SLOTS; n++) slot_clear(n);
        if (!g_enabled) { printf("%s disabled (COD1PLUS_STATS=%s)\n", TAG, e); fflush(stdout); }
    }
    return g_enabled;
}

void match_stats_game_init(uintptr_t game_base, int restart, int savepersist)
{
    if (!match_stats_enabled()) return;
    if (!restart || !savepersist) {
        /* game[] starts over (new map, map_restart(false)): zPAM's playerstats with it */
        if (g_rounds || r_active) {
            printf("%s new %s: stats of %s (%d rounds) cleared\n", TAG, restart ? "match" : "map",
                   g_map[0] ? g_map : "the previous map", g_rounds);
            fflush(stdout);
        }
        reset_map();
    } else {
        if (r_active) {
            printf("%s restart before round %d ended - its clutches/aces dropped\n", TAG, g_rounds + 1);
            fflush(stdout);
            r_active = 0;
        }
        for (int n = 0; n < MAX_SLOTS; n++) {                /* rebound by identity next frame */
            char seen[sizeof(g_slot[n].seen_uuid)];
            memcpy(seen, g_slot[n].seen_uuid, sizeof(seen));
            slot_clear(n);
            memcpy(g_slot[n].seen_uuid, seen, sizeof(seen));
        }
    }
    if (game_base) install(game_base);
}

void match_stats_frame(void)
{
    if (!g_on) return;
    for (int n = 0; n < MAX_SLOTS; n++) {
        if (!client_of(n)) {
            if (g_slot[n].rec >= 0 || g_slot[n].pend_switch) slot_clear(n);
            continue;
        }
        bind_slot(n);
        if (g_slot[n].pend_switch) {
            int r = g_slot[n].pend_rec;
            if (team_of(n) != g_slot[n].pend_team && r >= 0 && g_rec[r].used && g_rec[r].deaths > 0) {
                g_rec[r].deaths--;
                if (g_verbose) { printf("%s %s changed team: that suicide is not a death\n", TAG, g_rec[r].name); fflush(stdout); }
            }
            g_slot[n].pend_switch = 0;
        }
    }
    if (r_active) {
        update_clutch();
        poll_scores();
    }
}
