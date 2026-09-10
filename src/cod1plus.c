/*
 * cod1plus.c  —  CoD1 SoloQ S&D Stats Tracker
 *
 * Injected via LD_PRELOAD into cod_lnxded.
 * At round end, PAM's sd.gsc prints a [STATS_EVENT] line to qconsole.log.
 * This code tails that file, parses the event, merges with matchdata.cfg,
 * and POSTs the full fpschallenge.eu-compatible payload to the local backend.
 *
 * Data flow:
 *   PAM sd.gsc  →  qconsole.log  →  cod1plus.so  →  fpschallenge.eu 
 *                                                                                                    
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>

#include "cod1_defs.h"
#include "hooks.h"
#include "cod1reloaded.h"
#include "lean_hitbox.h"
#include "perbone_hit.h"
#include "pose_sync.h"
#include "swing_sync.h"
#include "hitbox_draw.h"
#include "antilag.h"
#include "anim_clamp.h"
#include "competitive_sv.h"
#include "cheat_gate.h"

#define COD1PLUS_TAG    "[cod1plus]"
#define CFG_PATH        "./matchdata.cfg"
#define MAX_PLAYERS     32

/* ------------------------------------------------------------------ */
/* Structures                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[64];
    char uuid[64];
    int  team; /* 1 or 2 */
} player_cfg_t;

typedef struct {
    char match_id[64];
    char start_time[64];

    char team1_id[64];
    char team1_name[128];
    char team1_tag[32];
    int  team1_side;   /* 1 = team1 starts as allies, 2 = team1 starts as axis */

    char team2_id[64];
    char team2_name[128];
    char team2_tag[32];

    char format[16];   /* "BO1", "BO3", … */
    char mr[16];       /* "MR12", "MR10", … — display only */
    int  half_round;   /* default 12 */
    int  score_limit;  /* default 13 */
    int  round_limit;  /* default 24 */

    char api_url[256];
    char demo_url[256];
    char map[64];
    char logfile[256];

    player_cfg_t players[MAX_PLAYERS];
    int          num_players;

    int loaded; /* 1 once parsed successfully */
} match_config_t;

typedef struct {
    char  name[64];
    char  team[16];   /* "allies" or "axis" */
    int   kills;
    int   deaths;
    int   assists;
    int   damage;
    int   grenades;
    int   plants;
    int   defuses;
    float score;
    int   headshots;
    int   grenade_damage;
    float adr;
    int   slot;   /* client slot (GSC entityId), -1 = unknown (old pk3, or the
                   * player had already disconnected when the round ended) */
} event_player_t;

typedef struct {
    int   round;
    int   allies_score;
    int   axis_score;
    char  round_winner[16]; /* "allies", "axis", "draw" */
    int   is_halftime;
    int   bomb_planted;
    event_player_t players[MAX_PLAYERS];
    int   num_players;
} round_event_t;

static match_config_t g_cfg;
static int            g_team1_won_maps = 0;
static int            g_team2_won_maps = 0;
static int            g_finished_maps  = 0;
typedef void (*SV_DirectConnect_t)(netadr_t from);
static hook_t             g_sv_directconnect_hook;
static SV_DirectConnect_t g_sv_directconnect_trampoline = NULL;
static char           g_client_uuid[MAX_CLIENTS][64];
static char           g_client_name[MAX_CLIENTS][64];
static int8_t         g_client_uuid_status[MAX_CLIENTS];
static unsigned char  g_client_uuid_mismatch_logged[MAX_CLIENTS];
static unsigned char  g_client_userinfo_logged[MAX_CLIENTS];
static int            g_side_tracker_initialized = 0;
static int            g_team1_is_allies_current = 1;
static int            g_last_event_round = -1;
static int            g_last_event_ht = -1;
static int            g_current_score_limit = 13;
static int            g_sv_maxclients = MAX_CLIENTS;
static int            http_post(const char *url, const char *json);
static void           parse_cmdline_fs_homepath(char *out, size_t sz);
static void           parse_cmdline_fs_game(char *out, size_t sz);

/* ------------------------------------------------------------------ */
/* Config parser — handles: set KEY "VALUE"  or  set KEY VALUE         */
/* Lines starting with '//' are comments.                              */
/* ------------------------------------------------------------------ */

static void cfg_trim(char *s) {
    /* Remove trailing whitespace/newline */
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t'))
        s[--n] = 0;
}

static int cfg_parse_line(const char *line, char *key, char *val, size_t sz) {
    /* Skip comments and blank lines */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '/' || *p == '#' || *p == '\n' || *p == '\r' || *p == 0)
        return 0;

    /* Expect "set KEY VALUE" */
    if (strncmp(p, "set ", 4) != 0) return 0;
    p += 4;
    while (*p == ' ') p++;

    /* Read key */
    size_t ki = 0;
    while (*p && *p != ' ' && *p != '\t' && ki + 1 < sz)
        key[ki++] = *p++;
    key[ki] = 0;
    if (ki == 0) return 0;

    while (*p == ' ' || *p == '\t') p++;

    /* Read value — quoted or unquoted */
    size_t vi = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && vi + 1 < sz)
            val[vi++] = *p++;
    } else {
        while (*p && *p != '\n' && *p != '\r' && vi + 1 < sz)
            val[vi++] = *p++;
    }
    val[vi] = 0;
    cfg_trim(val);
    return 1;
}

static void cfg_set(match_config_t *c, const char *key, const char *val) {
#define SET_STR(k, field) if (strcmp(key, k) == 0) { strncpy(c->field, val, sizeof(c->field)-1); return; }
#define SET_INT(k, field) if (strcmp(key, k) == 0) { c->field = atoi(val); return; }

    SET_STR("cod1plus_match_id",    match_id)
    SET_STR("cod1plus_start_time",  start_time)
    SET_STR("cod1plus_team1_id",    team1_id)
    SET_STR("cod1plus_team1_name",  team1_name)
    SET_STR("cod1plus_team1_tag",   team1_tag)
    SET_INT("cod1plus_team1_side",  team1_side)
    SET_STR("cod1plus_team2_id",    team2_id)
    SET_STR("cod1plus_team2_name",  team2_name)
    SET_STR("cod1plus_team2_tag",   team2_tag)
    SET_STR("cod1plus_format",      format)
    SET_STR("cod1plus_mr",          mr)
    SET_INT("cod1plus_half_round",  half_round)
    SET_INT("cod1plus_score_limit", score_limit)
    SET_INT("cod1plus_round_limit", round_limit)
    SET_STR("cod1plus_api_url",     api_url)
    SET_STR("cod1plus_demo_url",    demo_url)
    SET_STR("cod1plus_map",         map)
    SET_STR("cod1plus_logfile",     logfile)
#undef SET_STR
#undef SET_INT

    /* Players: cod1plus_player1 .. cod1plus_playerN  →  "name,uuid,team" */
    if (strncmp(key, "cod1plus_player", 15) == 0 && c->num_players < MAX_PLAYERS) {
        player_cfg_t *p = &c->players[c->num_players];
        char tmp[256];
        strncpy(tmp, val, sizeof(tmp)-1);
        char *name = strtok(tmp, ",");
        char *uuid = strtok(NULL, ",");
        char *team = strtok(NULL, ",");
        if (name && uuid && team) {
            strncpy(p->name, name, sizeof(p->name)-1);
            strncpy(p->uuid, uuid, sizeof(p->uuid)-1);
            p->team = atoi(team);
            c->num_players++;
        }
    }
}

static int cfg_load(match_config_t *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("%s matchdata.cfg not found at '%s' — running without match config\n",
               COD1PLUS_TAG, path);
        return -1;
    }

    /* Defaults */
    c->team1_side   = 1;
    c->half_round   = 12;
    c->score_limit  = 13;
    c->round_limit  = 24;
    strncpy(c->format,   "BO1",             sizeof(c->format)-1);
    strncpy(c->mr,       "MR12",            sizeof(c->mr)-1);
    strncpy(c->api_url,  "https://fpschallenge.eu/api/v2/cod1/match/", sizeof(c->api_url)-1);
    char fs_homepath[256] = {0};
    parse_cmdline_fs_homepath(fs_homepath, sizeof(fs_homepath));
    if (fs_homepath[0]) {
        /* When a mod is loaded (fs_game set), CoD1 writes the g_log into the MOD dir,
         * not main/ — that's where PAM's [STATS_EVENT] lands. Verified in-tree:
         * __rPAMv115b5/games_mp.log = 532 events vs main/games_mp.log = 15. Fall back
         * to main/ only for a vanilla (no fs_game) server. Override any time via the
         * cod1plus_logfile key in matchdata.cfg. */
        char fs_game[128] = {0};
        parse_cmdline_fs_game(fs_game, sizeof(fs_game));
        const char *logdir = fs_game[0] ? fs_game : "main";
        snprintf(c->logfile, sizeof(c->logfile), "%s/%s/games_mp.log", fs_homepath, logdir);
    } else {
        /* CoD1 defaults to $HOME/.callofduty when fs_homepath not set on cmdline */
        const char *home = getenv("HOME");
        if (home && home[0])
            snprintf(c->logfile, sizeof(c->logfile), "%s/.callofduty/main/games_mp.log", home);
        else
            strncpy(c->logfile, "./games_mp.log", sizeof(c->logfile)-1);
    }

    char line[512];
    char key[128], val[256];
    while (fgets(line, sizeof(line), f)) {
        if (cfg_parse_line(line, key, val, sizeof(key)))
            cfg_set(c, key, val);
    }
    fclose(f);

    /* If start_time not set, use current time */
    if (c->start_time[0] == 0) {
        time_t now = time(NULL);
        struct tm *tm_info = gmtime(&now);
        strftime(c->start_time, sizeof(c->start_time),
                 "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
    }

    c->loaded = 1;
    printf("%s Match config loaded: match_id=%s  %s vs %s  format=%s\n",
           COD1PLUS_TAG, c->match_id, c->team1_name, c->team2_name, c->format);
    printf("%s %d player(s) configured\n", COD1PLUS_TAG, c->num_players);
    return 0;
}

static const char *json_skip_ws(const char *p) {
    while (p && *p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

static const char *json_find_key(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, needle)) != NULL) {
        p += strlen(needle);
        p = json_skip_ws(p);
        if (*p != ':') continue;
        p++;
        return json_skip_ws(p);
    }
    return NULL;
}

static int json_read_string(const char *p, char *out, size_t out_sz) {
    if (!p || *p != '"' || !out || out_sz == 0) return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return (*p == '"');
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_sz) {
    const char *p = json_find_key(json, key);
    if (!p) return 0;
    return json_read_string(p, out, out_sz);
}

static int json_get_int(const char *json, const char *key, int *out) {
    const char *p = json_find_key(json, key);
    if (!p || !out) return 0;
    *out = atoi(p);
    return 1;
}

static const char *json_find_array(const char *json, const char *key, const char **arr_end) {
    const char *p = json_find_key(json, key);
    if (!p || *p != '[') return NULL;
    int depth = 0;
    const char *q = p;
    while (*q) {
        if (*q == '[') depth++;
        else if (*q == ']') {
            depth--;
            if (depth == 0) {
                if (arr_end) *arr_end = q;
                return p;
            }
        }
        q++;
    }
    return NULL;
}

static int json_extract_string_range(const char *start, const char *end,
                                     const char *key, char *out, size_t out_sz) {
    if (!start || !end || start >= end) return 0;
    size_t len = (size_t)(end - start);
    char tmp[2048];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, start, len);
    tmp[len] = 0;
    return json_get_string(tmp, key, out, out_sz);
}

static int json_extract_int_range(const char *start, const char *end,
                                  const char *key, int *out) {
    if (!start || !end || start >= end) return 0;
    size_t len = (size_t)(end - start);
    char tmp[2048];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, start, len);
    tmp[len] = 0;
    return json_get_int(tmp, key, out);
}

static int parse_side_value(const char *v) {
    if (!v || !*v) return 0;
    if (strcasecmp(v, "allies") == 0) return 1;
    if (strcasecmp(v, "axis") == 0) return 2;
    if (strcmp(v, "1") == 0) return 1;
    if (strcmp(v, "2") == 0) return 2;
    return 0;
}

static int parse_team_players(const char *json, const char *team_key, int team_id,
                              player_cfg_t *out, int *count) {
    const char *team_pos = strstr(json, team_key);
    if (!team_pos) return 0;
    const char *players_end = NULL;
    const char *players = json_find_array(team_pos, "players", &players_end);
    if (!players || !players_end) return 0;
    const char *p = players;
    while (p < players_end && *count < MAX_PLAYERS) {
        const char *obj_start = strchr(p, '{');
        if (!obj_start || obj_start > players_end) break;
        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end || obj_end > players_end) break;
        player_cfg_t *pc = &out[*count];
        char name[64] = {0};
        char uuid[64] = {0};
        if (json_extract_string_range(obj_start, obj_end, "name", name, sizeof(name)) &&
            json_extract_string_range(obj_start, obj_end, "uuid", uuid, sizeof(uuid))) {
            strncpy(pc->name, name, sizeof(pc->name) - 1);
            strncpy(pc->uuid, uuid, sizeof(pc->uuid) - 1);
            pc->team = team_id;
            (*count)++;
        }
        p = obj_end + 1;
    }
    return 1;
}

static int write_matchdata_cfg_from_json(const char *json, const char *path) {
    if (!json || !path) return -1;
    match_config_t c = {0};
    int match_id = 0;
    char team1_id[64] = {0}, team2_id[64] = {0};
    char team1_name[128] = {0}, team2_name[128] = {0};
    char team1_tag[32] = {0}, team2_tag[32] = {0};
    char team1_side_s[32] = {0}, team2_side_s[32] = {0};
    char format[16] = {0};
    char map[64] = {0};
    int team1_side_i = 0, team2_side_i = 0;

    c.team1_side = 1;

    json_get_int(json, "matchId", &match_id);
    snprintf(c.match_id, sizeof(c.match_id), "%d", match_id);
    json_get_string(json, "format", format, sizeof(format));
    if (format[0]) strncpy(c.format, format, sizeof(c.format) - 1);

    const char *maps_end = NULL;
    const char *maps = json_find_array(json, "maps", &maps_end);
    if (maps && maps_end) {
        const char *p = json_skip_ws(maps + 1);
        if (*p == '"') json_read_string(p, map, sizeof(map));
    }
    if (map[0]) strncpy(c.map, map, sizeof(c.map) - 1);

    const char *team1_pos = strstr(json, "\"team1\"");
    if (team1_pos) {
        if (!json_extract_string_range(team1_pos, team1_pos + 2048, "id", team1_id, sizeof(team1_id))) {
            int id_i = 0;
            if (json_extract_int_range(team1_pos, team1_pos + 2048, "id", &id_i))
                snprintf(team1_id, sizeof(team1_id), "%d", id_i);
        }
        json_extract_string_range(team1_pos, team1_pos + 2048, "name", team1_name, sizeof(team1_name));
        json_extract_string_range(team1_pos, team1_pos + 2048, "tag", team1_tag, sizeof(team1_tag));
        json_extract_string_range(team1_pos, team1_pos + 2048, "side", team1_side_s, sizeof(team1_side_s));
        if (!team1_side_s[0])
            json_extract_string_range(team1_pos, team1_pos + 2048, "startingSide", team1_side_s, sizeof(team1_side_s));
        json_extract_int_range(team1_pos, team1_pos + 2048, "side", &team1_side_i);
        if (team1_side_i == 0)
            json_extract_int_range(team1_pos, team1_pos + 2048, "startingSide", &team1_side_i);
    }
    const char *team2_pos = strstr(json, "\"team2\"");
    if (team2_pos) {
        if (!json_extract_string_range(team2_pos, team2_pos + 2048, "id", team2_id, sizeof(team2_id))) {
            int id_i = 0;
            if (json_extract_int_range(team2_pos, team2_pos + 2048, "id", &id_i))
                snprintf(team2_id, sizeof(team2_id), "%d", id_i);
        }
        json_extract_string_range(team2_pos, team2_pos + 2048, "name", team2_name, sizeof(team2_name));
        json_extract_string_range(team2_pos, team2_pos + 2048, "tag", team2_tag, sizeof(team2_tag));
        json_extract_string_range(team2_pos, team2_pos + 2048, "side", team2_side_s, sizeof(team2_side_s));
        if (!team2_side_s[0])
            json_extract_string_range(team2_pos, team2_pos + 2048, "startingSide", team2_side_s, sizeof(team2_side_s));
        json_extract_int_range(team2_pos, team2_pos + 2048, "side", &team2_side_i);
        if (team2_side_i == 0)
            json_extract_int_range(team2_pos, team2_pos + 2048, "startingSide", &team2_side_i);
    }
    if (team1_id[0]) strncpy(c.team1_id, team1_id, sizeof(c.team1_id) - 1);
    if (team2_id[0]) strncpy(c.team2_id, team2_id, sizeof(c.team2_id) - 1);
    if (team1_name[0]) strncpy(c.team1_name, team1_name, sizeof(c.team1_name) - 1);
    if (team2_name[0]) strncpy(c.team2_name, team2_name, sizeof(c.team2_name) - 1);
    if (team1_tag[0]) strncpy(c.team1_tag, team1_tag, sizeof(c.team1_tag) - 1);
    if (team2_tag[0]) strncpy(c.team2_tag, team2_tag, sizeof(c.team2_tag) - 1);

    int parsed_team1_side = parse_side_value(team1_side_s);
    int parsed_team2_side = parse_side_value(team2_side_s);
    if (!parsed_team1_side && (team1_side_i == 1 || team1_side_i == 2))
        parsed_team1_side = team1_side_i;
    if (!parsed_team2_side && (team2_side_i == 1 || team2_side_i == 2))
        parsed_team2_side = team2_side_i;
    if (parsed_team1_side)
        c.team1_side = parsed_team1_side;
    else if (parsed_team2_side)
        c.team1_side = (parsed_team2_side == 1) ? 2 : 1;

    parse_team_players(json, "\"team1\"", 1, c.players, &c.num_players);
    parse_team_players(json, "\"team2\"", 2, c.players, &c.num_players);

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "set cod1plus_match_id       \"%s\"\n", c.match_id);
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char start_time[64];
    strftime(start_time, sizeof(start_time), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
    fprintf(f, "set cod1plus_start_time     \"%s\"\n\n", start_time);
    fprintf(f, "set cod1plus_team1_id       \"%s\"\n", c.team1_id);
    fprintf(f, "set cod1plus_team1_name     \"%s\"\n", c.team1_name);
    fprintf(f, "set cod1plus_team1_tag      \"%s\"\n", c.team1_tag);
    fprintf(f, "set cod1plus_team1_side     \"%d\"\n\n", c.team1_side);
    fprintf(f, "set cod1plus_team2_id       \"%s\"\n", c.team2_id);
    fprintf(f, "set cod1plus_team2_name     \"%s\"\n", c.team2_name);
    fprintf(f, "set cod1plus_team2_tag      \"%s\"\n\n", c.team2_tag);
    fprintf(f, "set cod1plus_format         \"%s\"\n", c.format[0] ? c.format : "BO1");
    fprintf(f, "set cod1plus_mr             \"MR12\"\n");
    fprintf(f, "set cod1plus_half_round     \"12\"\n");
    fprintf(f, "set cod1plus_score_limit    \"13\"\n");
    fprintf(f, "set cod1plus_round_limit    \"24\"\n\n");
    fprintf(f, "set cod1plus_api_url        \"https://fpschallenge.eu/api/v2/cod1/match/%s\"\n", c.match_id);
    if (c.map[0]) fprintf(f, "set cod1plus_map            \"%s\"\n\n", c.map);
    for (int i = 0; i < c.num_players; i++) {
        fprintf(f, "set cod1plus_player%d        \"%s,%s,%d\"\n",
                i + 1, c.players[i].name, c.players[i].uuid, c.players[i].team);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Strip surrounding quotes from a command-line value.
 *
 * The FPS orchestrator builds its command line WITHOUT a shell, so the quotes it
 * writes around values survive into argv verbatim: `+match create "315313"` arrives
 * as the literal 8-char string "315313" (quotes included), and `+set fs_homepath
 * "/path"` likewise. The match id then never matches any match, and the stats log
 * path resolves to nothing - the exact "No match config - log tailer idle" that was
 * reported. A shell-launched server (./start.sh 99999) has no quotes, which is why
 * it worked there and not here. Accept both forms. */
static void strip_quotes(char *s) {
    if (!s || !*s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == 0x22 || s[len-1] == 0x27 ||
                       s[len-1] == 0x0d || s[len-1] == 0x0a || s[len-1] == 0x20)) {
        s[--len] = 0;
    }
    size_t start = 0;
    while (s[start] == 0x22 || s[start] == 0x27 || s[start] == 0x20) start++;
    if (start) memmove(s, s + start, len - start + 1);
}

/* ------------------------------------------------------------------ */
/* Command-line reader                                                 */
/*                                                                     */
/* /proc/self/cmdline is NUL-separated, but WHAT lands in each slot     */
/* depends on how the server was launched:                             */
/*                                                                     */
/*   shell   (./start.sh 99999)  -> one argv entry per token:           */
/*                "+match" "create" "99999"                             */
/*   manager (FPS orchestrator)  -> the WHOLE game command line in a    */
/*                SINGLE argv entry, quotes included:                   */
/*                "+set fs_game __rPAMv115b5 ... +match create \"315326\" ..." */
/*                                                                     */
/* Matching argv slots therefore found nothing on the FPS machines -    */
/* "No match config - log tailer idle", no stats, and the stats log     */
/* path never resolved either. Flatten NULs to spaces and tokenize the  */
/* result ourselves (honouring quotes), which handles both layouts.     */
/* ------------------------------------------------------------------ */

#define CMDLINE_MAX_TOKENS 128
#define CMDLINE_TOKEN_LEN  256

static int cmdline_tokens(char tok[CMDLINE_MAX_TOKENS][CMDLINE_TOKEN_LEN]) {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return 0;
    char raw[8192];
    size_t n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    raw[n] = 0;
    for (size_t i = 0; i < n; i++) if (raw[i] == 0) raw[i] = 0x20;  /* NUL -> space */

    int count = 0;
    size_t i = 0;
    while (i < n && count < CMDLINE_MAX_TOKENS) {
        while (i < n && raw[i] == 0x20) i++;
        if (i >= n) break;
        size_t len = 0;
        if (raw[i] == 0x22) {                       /* quoted value */
            i++;
            while (i < n && raw[i] != 0x22 && len + 1 < CMDLINE_TOKEN_LEN)
                tok[count][len++] = raw[i++];
            if (i < n && raw[i] == 0x22) i++;
        } else {
            while (i < n && raw[i] != 0x20 && len + 1 < CMDLINE_TOKEN_LEN)
                tok[count][len++] = raw[i++];
        }
        tok[count][len] = 0;
        strip_quotes(tok[count]);
        if (tok[count][0]) count++;
    }
    return count;
}

/* Value that follows `key` on the command line ("fs_homepath" -> its path). */
static int cmdline_value_after(const char *key, char *out, size_t sz) {
    static char tok[CMDLINE_MAX_TOKENS][CMDLINE_TOKEN_LEN];
    const int n = cmdline_tokens(tok);
    for (int i = 0; i + 1 < n; i++) {
        if (strcasecmp(tok[i], key) == 0 && tok[i+1][0]) {
            strncpy(out, tok[i+1], sz - 1);
            out[sz - 1] = 0;
            return 0;
        }
    }
    return -1;
}

/* +match create {id} */
static int parse_cmdline_match_id(char *out, size_t sz) {
    static char tok[CMDLINE_MAX_TOKENS][CMDLINE_TOKEN_LEN];
    const int n = cmdline_tokens(tok);
    for (int i = 0; i + 2 < n; i++) {
        if (strcasecmp(tok[i], "+match") == 0 &&
            strcasecmp(tok[i+1], "create") == 0 && tok[i+2][0]) {
            strncpy(out, tok[i+2], sz - 1);
            out[sz - 1] = 0;
            return (out[0] != 0) ? 0 : -1;
        }
    }
    return -1;
}

static void parse_cmdline_fs_homepath(char *out, size_t sz) {
    if (cmdline_value_after("fs_homepath", out, sz) != 0) out[0] = 0;
}

/* CoD1 writes the g_log (games_mp.log, where PAM prints [STATS_EVENT]) into
 * fs_homepath/<fs_game>/ when a mod is loaded, NOT main/. */
static void parse_cmdline_fs_game(char *out, size_t sz) {
    if (cmdline_value_after("fs_game", out, sz) != 0) out[0] = 0;
}

static int parse_cmdline_maxclients(void) {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return MAX_CLIENTS;

    char cmdline[4096] = {0};
    size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
    fclose(f);

    size_t i = 0;
    while (i < n) {
        const char *arg = &cmdline[i];
        size_t len = strlen(arg);
        if (len == 0) { i++; continue; }

        if (strcasecmp(arg, "sv_maxclients") == 0) {
            size_t val_start = i + len + 1;
            if (val_start < n && cmdline[val_start]) {
                int v = atoi(&cmdline[val_start]);
                if (v > 0 && v <= MAX_CLIENTS) return v;
            }
        }
        i += len + 1;
    }
    return MAX_CLIENTS;
}

/* ------------------------------------------------------------------ */
/* [STATS_EVENT] line parser                                           */
/* Format: [STATS_EVENT]r=N,as=N,xs=N,rw=X,ht=N,bp=N,
 *         ps=name:team:k:d:a:dm:g:p:df:score[:hs:gd:adr[:slot]]|…
 * slot = client slot (GSC entityId), the name-independent identity key. */
/* ------------------------------------------------------------------ */

static int parse_event(const char *line, round_event_t *ev) {
    const char *tag = strstr(line, "[STATS_EVENT]");
    if (!tag) return -1;
    const char *p = tag + strlen("[STATS_EVENT]");

    memset(ev, 0, sizeof(*ev));

    /* Copy into mutable buffer */
    char buf[4096];
    strncpy(buf, p, sizeof(buf)-1);
    buf[sizeof(buf)-1] = 0;

    /* Separate ps= section before strtok destroys commas */
    char ps_buf[2048] = {0};
    char *ps_start = strstr(buf, ",ps=");
    if (ps_start) {
        strncpy(ps_buf, ps_start + 4, sizeof(ps_buf)-1);
        *ps_start = 0;
    }

    /* Parse r=,as=,xs=,rw=,ht=,bp= */
    char *tok = strtok(buf, ",");
    while (tok) {
        char k[32] = {0}, v[64] = {0};
        if (sscanf(tok, "%31[^=]=%63s", k, v) == 2) {
            if      (strcmp(k, "r")  == 0) ev->round        = atoi(v);
            else if (strcmp(k, "as") == 0) ev->allies_score  = atoi(v);
            else if (strcmp(k, "xs") == 0) ev->axis_score    = atoi(v);
            else if (strcmp(k, "rw") == 0) strncpy(ev->round_winner, v, 15);
            else if (strcmp(k, "ht") == 0) ev->is_halftime   = atoi(v);
            else if (strcmp(k, "bp") == 0) ev->bomb_planted  = atoi(v);
        }
        tok = strtok(NULL, ",");
    }

    /* Parse player list:
     * name:team:kills:deaths:assists:damage:grenades:plants:defuses:score[:hs:gd:adr[:slot]]
     * slot (field 14, added 2026-08-23) = the GSC entityId = the CLIENT SLOT, the
     * key that makes stats identity name-independent. Older pk3s send 13 or 10
     * fields; both still parse (n>=10) with slot = -1. */
    char *pline = strtok(ps_buf, "|");
    while (pline && ev->num_players < MAX_PLAYERS) {
        event_player_t *ep = &ev->players[ev->num_players];
        char score_str[32] = {0};
        char adr_str[32] = {0};
        ep->slot = -1;
        int n = sscanf(pline,
            "%63[^:]:%15[^:]:%d:%d:%d:%d:%d:%d:%d:%31[^:]:%d:%d:%31[^:]:%d",
            ep->name, ep->team,
            &ep->kills, &ep->deaths, &ep->assists, &ep->damage,
            &ep->grenades, &ep->plants, &ep->defuses, score_str,
            &ep->headshots, &ep->grenade_damage, adr_str, &ep->slot);
        if (n >= 10) {
            ep->score = strtof(score_str, NULL);
            if (n >= 13) ep->adr = strtof(adr_str, NULL);
            if (n < 14) ep->slot = -1;
            ev->num_players++;
        }
        pline = strtok(NULL, "|");
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Payload builder                */
/* ------------------------------------------------------------------ */

static void json_esc(const char *src, char *dst, size_t sz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
        else if (c >= 32 && c < 127) dst[j++] = (char)c;
    }
    dst[j] = 0;
}

static int userinfo_get(const char *userinfo, const char *key, char *out, size_t out_sz) {
    if (!userinfo || !key || !out || out_sz == 0) return 0;
    size_t key_len = strlen(key);
    const char *p = userinfo;
    while (*p) {
        if (*p == '\\') p++;
        const char *k = p;
        while (*p && *p != '\\') p++;
        size_t klen = (size_t)(p - k);
        if (*p == '\\') p++;
        const char *v = p;
        while (*p && *p != '\\') p++;
        size_t vlen = (size_t)(p - v);
        if (klen == key_len && strncmp(k, key, klen) == 0) {
            size_t copy_len = vlen < out_sz - 1 ? vlen : out_sz - 1;
            memcpy(out, v, copy_len);
            out[copy_len] = 0;
            return 1;
        }
        if (*p == '\\') p++;
    }
    return 0;
}

static int userinfo_get_safe(const char *userinfo, size_t max_len,
                             const char *key, char *out, size_t out_sz) {
    if (!userinfo || !key || !out || out_sz == 0 || max_len == 0) return 0;
    size_t len = strnlen(userinfo, max_len);
    if (len == max_len) return 0;
    char tmp[1025];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, userinfo, len);
    tmp[len] = 0;
    return userinfo_get(tmp, key, out, out_sz);
}

static const char *expected_uuid_for_name(const match_config_t *c, const char *name);

static const char *uuid_url(void) {
    const char *env = getenv("COD1PLUS_UUID_URL");
    if (env && *env) return env;
    return "";
}

static void send_uuid_event(int client_num, const char *name, const char *uuid) {
    const char *url = uuid_url();
    if (!url || !*url) return;
    char esc_name[128];
    char esc_uuid[128];
    json_esc(name ? name : "", esc_name, sizeof(esc_name));
    json_esc(uuid ? uuid : "", esc_uuid, sizeof(esc_uuid));
    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"type\":\"client_uuid\",\"client_num\":%d,\"name\":\"%s\",\"uuid\":\"%s\"}",
             client_num, esc_name, esc_uuid);
    http_post(url, payload);
}

static void collect_client_uuids(void) {
    serverStatic_t *svs = (serverStatic_t *)ADDR_SVS;
    if (!svs || !svs->clients) return;
    /* The walk below strides svs->clients by CLIENT_T_SIZE (362 KB). The command line
     * rarely carries sv_maxclients (start.sh keeps it in the .cfg), and the fallback of
     * MAX_CLIENTS=64 then read up to 23 MB past a 12/16-slot array every second: fine
     * while that memory happened to be mapped, a SIGSEGV the day it was not (gungame
     * end-of-round crash hunt, 2026-08-28). The engine's own cvar is the truth: its
     * cvar_t* lives at ADDR_SV_MAXCLIENTS_CVAR (set by SV_Init, read as ->integer at
     * +0x20 by SV_Startup/SV_ChangeMaxClients themselves - cod_lnxded 0x80905a1). */
    {
        const char *cv = *(const char **)ADDR_SV_MAXCLIENTS_CVAR;
        if (cv) {
            int mc = *(const int *)(cv + 0x20);
            if (mc > 0 && mc <= MAX_CLIENTS && mc != g_sv_maxclients) {
                printf("%s sv_maxclients %d -> %d (engine cvar)\n",
                       COD1PLUS_TAG, g_sv_maxclients, mc);
                fflush(stdout);
                g_sv_maxclients = mc;
            }
        }
    }
    for (int i = 0; i < g_sv_maxclients; i++) {
        client_t *cl = (client_t *)((char *)svs->clients + (CLIENT_T_SIZE_V15 * i));
        clientConnectState_t state = SVSCLIENT_STATE(cl);
        if (state < CS_CONNECTED || state > CS_ACTIVE) {
            g_client_uuid[i][0] = 0;
            g_client_name[i][0] = 0;
            g_client_uuid_status[i] = -1;
            g_client_uuid_mismatch_logged[i] = 0;
            g_client_userinfo_logged[i] = 0;
            continue;
        }
        const char *userinfo = (const char *)((char *)cl + CLIENT_T_OFF_USERINFO);
        char uuid[64];
        if (!userinfo || userinfo[0] != '\\') {
            continue;
        }
        /* cvar cheat check (PB replacement): read the client's self-reported verdict.
         * Observe-only by default; see cheat_gate.c. Runs before the login/uuid path so
         * it also covers clients that never send a login key. */
        if (cheat_gate_enabled()) {
            char ac[16], acname[64];
            if (!userinfo_get_safe(userinfo, 1024, "cod1x_ac", ac, sizeof(ac))) ac[0] = 0;
            if (!userinfo_get_safe(userinfo, 1024, "name", acname, sizeof(acname))) acname[0] = 0;
            cheat_gate_check(i, acname, ac);
        }
        if (!userinfo_get_safe(userinfo, 1024, "login", uuid, sizeof(uuid))) {
            continue;
        }
        if (uuid[0] == 0) continue;
        /* Refresh the NAME on every pass, not only when the uuid changes: a player
         * who renames mid-game keeps the same login, so the old code kept his OLD
         * name here forever and the name-fallback matching silently failed for him
         * (the exact "changed name -> no stats" report, 2026-08-23). */
        {
            char curname[64];
            if (userinfo_get_safe(userinfo, 1024, "name", curname, sizeof(curname)) &&
                curname[0] && strcmp(g_client_name[i], curname) != 0) {
                strncpy(g_client_name[i], curname, sizeof(g_client_name[i]) - 1);
                g_client_name[i][sizeof(g_client_name[i]) - 1] = 0;
            }
        }
        if (strcmp(g_client_uuid[i], uuid) != 0) {
            strncpy(g_client_uuid[i], uuid, sizeof(g_client_uuid[i]) - 1);
            g_client_uuid[i][sizeof(g_client_uuid[i]) - 1] = 0;
            char name[64];
            if (!userinfo_get_safe(userinfo, 1024, "name", name, sizeof(name))) name[0] = 0;
            if (name[0]) {
                strncpy(g_client_name[i], name, sizeof(g_client_name[i]) - 1);
                g_client_name[i][sizeof(g_client_name[i]) - 1] = 0;
            }
            printf("%s client_uuid slot=%d name=%s uuid=%s\n", COD1PLUS_TAG, i, name, uuid);
            if (g_cfg.loaded && name[0]) {
                const char *expected = expected_uuid_for_name(&g_cfg, name);
                if (expected && expected[0]) {
                    g_client_uuid_status[i] = (strcmp(uuid, expected) == 0) ? 1 : 0;
                    if (g_client_uuid_status[i] == 0 && !g_client_uuid_mismatch_logged[i]) {
                        printf("%s client_uuid mismatch name=%s expected=%s got=%s\n",
                               COD1PLUS_TAG, name, expected, uuid);
                        g_client_uuid_mismatch_logged[i] = 1;
                    }
                } else {
                    g_client_uuid_status[i] = -1;
                }
            } else {
                g_client_uuid_status[i] = -1;
            }
            send_uuid_event(i, name, uuid);
            g_client_userinfo_logged[i] = 1;
        }
    }
}

static void SV_DirectConnect_Hook(netadr_t from) {
    /* cod1reloaded version gate: reject outdated clients at connect time
     * (before a slot is allocated). 0 = rejected, error already sent. */
    if (!cod1reloaded_allow_connect(from))
        return;
    if (g_sv_directconnect_trampoline) {
        g_sv_directconnect_trampoline(from);
    }
}


static void *uuid_collector_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(1);
        collect_client_uuids();
    }
    return NULL;
}

/* Return the UUID for a player by name (from match config).
   If no config / no match → returns the name itself as fallback UUID. */
/* Compare two player names the way a human would.
 *
 * Every association in this file - stats event -> match config, stats event ->
 * connected slot - is keyed on the IN-GAME name, while the match config carries the
 * FPSChallenge username. A CoD colour code (^1..^9), a clan tag spacing difference
 * or a stray space is enough to make the exact comparison fail, and then the player
 * is either dropped from the report (name found, uuid mismatch) or emitted with his
 * NAME as uuid (name not found), which the backend cannot match. Either way he
 * silently has no stats - one different player per match, exactly as reported in
 * three consecutive 5v5s. Normalise before comparing: strip colour codes and all
 * whitespace, fold case. */
static void name_normalize(const char *in, char *out, size_t sz) {
    size_t o = 0;
    if (!in || !out || sz == 0) { if (out && sz) out[0] = 0; return; }
    for (size_t i = 0; in[i] && o + 1 < sz; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '^' && in[i+1]) { i++; continue; }   /* ^N colour code */
        if (ch <= 0x20) continue;                      /* spaces / control */
        out[o++] = (char)tolower(ch);
    }
    out[o] = 0;
}

static int name_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    if (strcasecmp(a, b) == 0) return 1;               /* fast path: exact */
    char na[96], nb[96];
    name_normalize(a, na, sizeof(na));
    name_normalize(b, nb, sizeof(nb));
    return (na[0] && strcmp(na, nb) == 0);
}

/* Find the roster entry for an in-game name, or -1.
 *
 * Tier 1: normalised equality - the common case.
 *
 * Tier 2: the roster name appears INSIDE the in-game name. Players decorate their nick
 *   with a clan tag the FPSChallenge username does not carry - "^6pP ^7wormii" for
 *   roster "wormii", "^5[^7diversity^5] ^7mskrQo" for "mskrQo" - and tier 1 cannot see
 *   through that: normalising only removes the colour codes, the tag stays and the
 *   strings still differ. Those two players are exactly the ones that came back from
 *   match 316953 with "uuid":"^6pP ^7wormii", i.e. their own name as identity, which the
 *   backend cannot resolve to an account.
 *
 *   Accepted ONLY when exactly one roster entry is contained in the name. With two
 *   candidates there is no way to tell which player this is, and a wrong guess credits
 *   someone else's stats - worse than no stats. Very short roster names are excluded
 *   for the same reason: a two-letter nick matches half the server by accident. */
#define ROSTER_SUBSTR_MIN 3

static int find_config_player(const match_config_t *c, const char *name) {
    if (!c || !name || !*name) return -1;

    for (int i = 0; i < c->num_players; i++)
        if (name_eq(c->players[i].name, name))
            return i;

    char nin[96];
    name_normalize(name, nin, sizeof(nin));
    if (!nin[0]) return -1;

    int found = -1;
    for (int i = 0; i < c->num_players; i++) {
        char nc[96];
        name_normalize(c->players[i].name, nc, sizeof(nc));
        if (strlen(nc) < ROSTER_SUBSTR_MIN) continue;
        if (strstr(nin, nc)) {
            if (found >= 0) return -1;      /* ambiguous - refuse to guess */
            found = i;
        }
    }
    return found;
}

static const char *lookup_uuid(const match_config_t *c, const char *name) {
    /* First: live-captured login UUID by in-game name. This is the only identity the
     * player actually proves, so it wins over anything derived from the roster. */
    for (int i = 0; i < g_sv_maxclients; i++) {
        if (g_client_name[i][0] && g_client_uuid[i][0] &&
            name_eq(g_client_name[i], name))
            return g_client_uuid[i];
    }
    /* Second: match config by FPSChallenge username, clan tags tolerated. */
    int idx = find_config_player(c, name);
    if (idx >= 0)
        return c->players[idx].uuid;
    return name; /* fallback */
}

static const char *expected_uuid_for_name(const match_config_t *c, const char *name) {
    if (!c || !c->loaded || !name || !*name) return NULL;
    int idx = find_config_player(c, name);
    return (idx >= 0) ? c->players[idx].uuid : NULL;
}

/* UID-only resolution (2026-08-23). The GSC now appends each stats row's client
 * SLOT (entityId, -1 once that player disconnected). The slot indexes straight
 * into the login-uuid table the collector maintains from live userinfo - the one
 * identity the player actually proves - so nicknames, colour codes, clan tags and
 * mid-game renames stop mattering entirely. All the name matching above survives
 * only as the fallback: old pk3s without the field, and players who left before
 * the round ended (their slot may already belong to someone else, so it must NOT
 * be trusted - the GSC sends -1 for them). */
static const char *uuid_for_event_player(const match_config_t *c,
                                         const event_player_t *ep) {
    if (ep->slot >= 0 && ep->slot < MAX_CLIENTS && ep->slot < g_sv_maxclients &&
        g_client_uuid[ep->slot][0])
        return g_client_uuid[ep->slot];
    return lookup_uuid(c, ep->name);
}

/* Kept for diagnostics: 1 = login matches the roster entry for this name,
 * 0 = it does not, -1 = unknown. No longer gates the report. */
__attribute__((unused))
static int client_uuid_status_for_name(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_client_name[i][0] && name_eq(g_client_name[i], name))
            return g_client_uuid_status[i];
    }
    return -1;
}

/* Return "team1" or "team2" for a player. `login` is the ALREADY-RESOLVED uuid
   (slot-based when available) so this stays consistent with the reported identity.
   If the uuid is not in the roster, fall back to name, then to GSC side + halftime. */
static const char *lookup_team_label(const match_config_t *c,
                                     const char *login,
                                     const char *name,
                                     const char *gsc_team,
                                     int team1_is_allies)
{
    /* Prefer the LOGIN uuid: it is the only identity the player actually proves.
     * In-game names are free text - a player may use a nick that has nothing to do
     * with his FPSChallenge username, which is common and perfectly legitimate. */
    if (login && login[0] && strcmp(login, name) != 0) {
        for (int i = 0; i < c->num_players; i++)
            if (c->players[i].uuid[0] && strcmp(c->players[i].uuid, login) == 0)
                return c->players[i].team == 1 ? "team1" : "team2";
    }
    /* then the config name, for a client that never sent a login */
    int idx = find_config_player(c, name);
    if (idx >= 0)
        return c->players[idx].team == 1 ? "team1" : "team2";

    /* Fallback: infer from GSC side + halftime.
     * team1_side=1 means team1 starts as allies.
     * After halftime the sides are swapped. */
    int player_is_allies = (strcmp(gsc_team, "allies") == 0);
    return (player_is_allies == team1_is_allies) ? "team1" : "team2";
}

/* Compute team1_score / team2_score from allies/axis scores + halftime. */
static void resolve_scores(const match_config_t *c,
                            int allies_score, int axis_score, int team1_is_allies,
                            int *t1, int *t2)
{
    (void)c;

    if (team1_is_allies) { *t1 = allies_score; *t2 = axis_score; }
    else                  { *t1 = axis_score;  *t2 = allies_score; }
}

/* Last scores we saw in a stats event - the tie that overtime starts from. */
static int g_last_event_allies = -1;
static int g_last_event_axis   = -1;

/* The last event we reported, kept so the end-of-map log line can re-send it as
 * finished. Deliberately NOT paired with a sticky "map ended" global: side_tracker_reset()
 * runs once in the .so constructor and never again, so any per-map flag would survive into
 * the next map of a BO3 and mark every round of it finished. The re-send passes
 * force_finished explicitly instead, and owns no state. */
static round_event_t   g_last_ev;
static int             g_have_last_ev  = 0;

/* PAM logs a bare "OverTime;" line from _overtime.gsc::Do_Overtime() into the very log we
 * already tail, at the moment it commits to overtime. Use it: it is a statement of fact,
 * not an inference.
 *
 * The old heuristic below (round counter went backwards AND scores are tied) cannot work.
 * sd.gsc calls logStats() at the END of a round, BEFORE the round/score-limit checks that
 * trigger overtime, so the first event after the reset is the end of OT round 1 - by then
 * someone has won a round and the scores are 13-12, never tied. The tie test therefore
 * never passed and the limit was never raised; the match only ever "finished" because the
 * round_limit clause fired. With that clause now correctly gated on a winner, overtime
 * needs a signal that actually arrives.
 *
 * PAM's own rule, identical in every MR variant (rules/sd/score/mr*.gsc:27):
 *     scr_sd_end_score = game["overtime_score"] + 4      // 12/12 -> 16/16 -> 20/20
 * so +4 per overtime, applied to the tied score we last saw. */
static void overtime_signalled(void) {
    int base = (g_last_event_allies > g_last_event_axis)
             ? g_last_event_allies : g_last_event_axis;
    if (base <= 0) {
        printf("%s OverTime; seen but no round scores yet - score limit left at %d\n",
               COD1PLUS_TAG, g_current_score_limit);
        return;
    }
    if (base + 4 > g_current_score_limit) {
        g_current_score_limit = base + 4;
        printf("%s OverTime; (score %d-%d): score_limit now %d\n",
               COD1PLUS_TAG, g_last_event_allies, g_last_event_axis, g_current_score_limit);
    }
}

static void side_tracker_reset(const match_config_t *c) {
    int configured_team1_side = (c && c->team1_side == 2) ? 2 : 1;
    g_team1_is_allies_current = (configured_team1_side == 1);
    g_current_score_limit = (c && c->score_limit > 0) ? c->score_limit : 13;
    g_last_event_round = -1;
    g_last_event_ht = -1;
    g_last_event_allies = -1;
    g_last_event_axis = -1;
    g_have_last_ev = 0;
    g_side_tracker_initialized = 1;
}

static void side_tracker_update(const match_config_t *c, const round_event_t *ev) {
    if (!g_side_tracker_initialized) side_tracker_reset(c);
    if (!ev) return;

    /* New map in a BO3/BO5: the score counters start over, which overtime never does
     * (it keeps them and only raises the limit). Without this the 16 or 20 left by an
     * overtime on map 1 would still be the limit on map 2, and map 2 would never reach
     * it - reported "playing" to the last round. */
    if (g_last_event_allies >= 0 &&
        ev->allies_score + ev->axis_score < g_last_event_allies + g_last_event_axis) {
        int base = (c && c->score_limit > 0) ? c->score_limit : 13;
        if (g_current_score_limit != base) {
            printf("%s New map (scores restarted): score_limit back to %d\n",
                   COD1PLUS_TAG, base);
            g_current_score_limit = base;
        }
    }

    int round_reset = (g_last_event_round > 0 && ev->round > 0 && ev->round < g_last_event_round);
    /* Kept as a belt-and-braces fallback if the OverTime; line is ever missed. */
    if (round_reset && ev->allies_score > 0 && ev->allies_score == ev->axis_score) {
        int ot_score_limit = ev->allies_score + 4;
        if (ot_score_limit > g_current_score_limit) {
            g_current_score_limit = ot_score_limit;
            printf("%s Overtime detected (score=%d-%d): score_limit now %d\n",
                   COD1PLUS_TAG, ev->allies_score, ev->axis_score, g_current_score_limit);
        }
    }

    if (g_last_event_ht != -1 && ev->is_halftime != g_last_event_ht) {
        if (g_last_event_ht == 0 && ev->is_halftime == 1) {
            g_team1_is_allies_current = !g_team1_is_allies_current;
            printf("%s Side swap detected: team1_is_allies=%d\n", COD1PLUS_TAG, g_team1_is_allies_current);
        } else if (g_last_event_ht == 1 && ev->is_halftime == 0 && !round_reset) {
            g_team1_is_allies_current = !g_team1_is_allies_current;
            printf("%s Side swap detected (ht reset): team1_is_allies=%d\n", COD1PLUS_TAG, g_team1_is_allies_current);
        }
    }

    g_last_event_round  = ev->round;
    g_last_event_ht     = ev->is_halftime;
    g_last_event_allies = ev->allies_score;
    g_last_event_axis   = ev->axis_score;
}

static int build_payload(const match_config_t *c,
                         const round_event_t *ev,
                         char *out, size_t out_sz,
                         int force_finished)
{
    int team1_is_allies = g_side_tracker_initialized
                        ? g_team1_is_allies_current
                        : ((c->team1_side == 2) ? 0 : 1);

    int t1_score, t2_score;
    resolve_scores(c, ev->allies_score, ev->axis_score, team1_is_allies,
                   &t1_score, &t2_score);

    /* Determine match state */
    const char *state = "playing";
    int effective_score_limit = g_side_tracker_initialized
                              ? g_current_score_limit
                              : c->score_limit;
    if (effective_score_limit <= 0) effective_score_limit = 13;

    /* A match is over when someone REACHES THE SCORE LIMIT - never merely because the
     * round counter hit round_limit. In MR12 a 12-12 after 24 rounds goes to overtime,
     * and reporting finished there closed the match on the platform with a draw score
     * (match 316953, 2026-08-07: "round":"Round 24 | MR12", 12-12, state finished).
     *
     * The overtime handling in side_tracker_update() cannot save us here: it only raises
     * g_current_score_limit once PAM has restarted the round counter at 1, which happens
     * on the FIRST event of overtime - one event AFTER this one. So the round_limit test
     * fires first, every time.
     *
     * round_limit is kept only as a safety net for a match that somehow overruns, and it
     * must never fire on a tie: with equal scores there is no winner to report. */
    if (force_finished)
        state = "finished";                 /* PAM said so - MatchEnd; / MapEnd; */
    else if (t1_score >= effective_score_limit || t2_score >= effective_score_limit)
        state = "finished";
    else if (ev->round >= c->round_limit && t1_score != t2_score)
        state = "finished";

    /* Round display string, e.g. "Round 3 | MR12" */
    char round_str[64];
    snprintf(round_str, sizeof(round_str), "Round %d | %s", ev->round, c->mr);

    /* Escaped strings */
    char t1n[256], t2n[256], mapn[64], demo[512];
    json_esc(c->team1_name,  t1n,  sizeof(t1n));
    json_esc(c->team2_name,  t2n,  sizeof(t2n));
    json_esc(c->demo_url,    demo, sizeof(demo));

    /* Read map name from /proc/self/cmdline (+map argument) */
    mapn[0] = 0;
    if (c->map[0]) {
        strncpy(mapn, c->map, sizeof(mapn) - 1);
    } else {
        FILE *f = fopen("/proc/self/cmdline", "r");
        if (f) {
            char cmdline[2048] = {0};
            fread(cmdline, 1, sizeof(cmdline)-1, f);
            fclose(f);
            for (int i = 0; i < 2000; i++) {
                if (cmdline[i] == 0 && strncmp(&cmdline[i+1], "+map", 4) == 0) {
                    strncpy(mapn, &cmdline[i+6], sizeof(mapn)-1);
                    break;
                }
            }
        }
        if (mapn[0] == 0) strncpy(mapn, "unknown", sizeof(mapn)-1);
    }

    /* Begin JSON */
    /* Every player in the event is reported - see the note on the emit loop. */
    const int players_count = ev->num_players;

    int pos = snprintf(out, out_sz,
        "{"
        "\"type\":\"data\","
        "\"start_time\":\"%s\","
        "\"match_id\":\"%s\","
        "\"team1_id\":\"%s\","
        "\"team2_id\":\"%s\","
        "\"team1_name\":\"%s\","
        "\"team2_name\":\"%s\","
        "\"demoUploadURL\":\"%s\","
        "\"format\":\"%s\","
        "\"forceNickNames\":\"true\","
        "\"playersCount\":\"%d\","
        "\"team1_tag\":\"%s\","
        "\"team2_tag\":\"%s\","
        "\"team1_winnedMaps\":\"%d\","
        "\"team2_winnedMaps\":\"%d\","
        "\"finishedMapsCount\":\"%d\","
        "\"team1_score\":\"%d\","
        "\"team2_score\":\"%d\","
        "\"map\":\"%s\","
        "\"round\":\"%s\","
        "\"state\":\"%s\","
        "\"debug\":\"sd endround\","
        "\"players\":[",
        c->start_time,
        c->match_id,
        c->team1_id, c->team2_id,
        t1n, t2n,
        demo,
        c->format,
        players_count,
        c->team1_tag, c->team2_tag,
        g_team1_won_maps, g_team2_won_maps, g_finished_maps,
        t1_score, t2_score,
        mapn, round_str, state);

    /* Players array */
    int emitted_players = 0;
    for (int i = 0; i < ev->num_players && pos < (int)out_sz - 256; i++) {
        const event_player_t *ep = &ev->players[i];
        /* NEVER drop a player from the report. This used to skip anyone whose
         * in-game name matched a config entry while his login uuid did not, as an
         * anti-impersonation guard - but names are free text and a legitimate
         * player using an unrelated nick was silently erased from the stats (one
         * different player per 5v5, three matches running). Identity comes from the
         * login uuid, which is emitted below; the backend decides what to do with a
         * player it cannot resolve. The mismatch is still logged at connect time. */
        const char *uuid       = uuid_for_event_player(c, ep);
        const char *team_label = lookup_team_label(c, uuid, ep->name, ep->team,
                                                   team1_is_allies);
        const char *team_name  = (strcmp(team_label, "team1") == 0)
                                 ? c->team1_name : c->team2_name;

        char esc_name[128], esc_uuid[128], esc_tname[256];
        json_esc(ep->name,  esc_name,  sizeof(esc_name));
        json_esc(uuid,      esc_uuid,  sizeof(esc_uuid));
        json_esc(team_name, esc_tname, sizeof(esc_tname));

        char key_uuid[128];
        snprintf(key_uuid, sizeof(key_uuid), "UUID_%s", esc_uuid);

        char score_str[32];
        snprintf(score_str, sizeof(score_str), "%.1f", ep->score);

        char damage_str[32];
        snprintf(damage_str, sizeof(damage_str), "%.1f", (float)ep->damage);

        int n = snprintf(out + pos, out_sz - pos,
            "%s{"
            "\"key\":\"%s\","
            "\"uuid\":\"%s\","
            "\"name\":\"%s\","
            "\"team\":\"%s\","
            "\"team_name\":\"%s\","
            "\"score\":\"%s\","
            "\"kills\":\"%d\","
            "\"headshots\":\"%d\","
            "\"assists\":\"%d\","
            "\"damage\":\"%s\","
            "\"deaths\":\"%d\","
            "\"grenades\":\"%d\","
            "\"plants\":\"%d\","
            "\"defuses\":\"%d\""
            "}",
            emitted_players ? "," : "",
            key_uuid, esc_uuid, esc_name,
            team_label, esc_tname, score_str,
            ep->kills, ep->headshots, ep->assists, damage_str, ep->deaths,
            ep->grenades, ep->plants, ep->defuses);
        pos += n;
        emitted_players++;
    }

    snprintf(out + pos, out_sz - pos, "]}");
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTTP POST (raw socket, sends to local backend on localhost)         */
/* ------------------------------------------------------------------ */

static int http_post_https(const char *url, const char *json) {
    if (!url || !json) return -1;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "LD_PRELOAD= curl -fsS -X POST -H \"Content-Type: application/json\" --data-binary @- \"%s\" 2>&1",
             url);
    FILE *fp = popen(cmd, "w");
    if (!fp) return -1;
    fwrite(json, 1, strlen(json), fp);
    int rc = pclose(fp);
    return (rc == 0) ? 0 : -1;
}

static int http_post(const char *url, const char *json) {
    if (url && strncmp(url, "https://", 8) == 0)
        return http_post_https(url, json);

    /* Parse http://host:port/path */
    char host[128] = "localhost";
    int  port      = 3005;
    char path[256] = "/api/round_end";

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    char hostport[256] = {0};
    const char *slash = strchr(p, '/');
    if (slash) {
        strncpy(hostport, p, (size_t)(slash - p));
        strncpy(path, slash, sizeof(path)-1);
    } else {
        strncpy(hostport, p, sizeof(hostport)-1);
    }
    char *colon = strchr(hostport, ':');
    if (colon) { *colon = 0; port = atoi(colon+1); }
    if (hostport[0]) strncpy(host, hostport, sizeof(host)-1);

    struct hostent *srv = gethostbyname(host);
    if (!srv) { printf("%s gethostbyname failed\n", COD1PLUS_TAG); return -1; }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr.s_addr, srv->h_addr, (size_t)srv->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        printf("%s HTTP connect failed\n", COD1PLUS_TAG);
        return -1;
    }

    char req[65536];
    snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, port, strlen(json), json);

    send(sock, req, strlen(req), 0);
    close(sock);
    return 0;
}

static int http_get_url(const char *url, char *resp, size_t resp_sz) {
    if (!url || !resp || resp_sz == 0) return -1;
    if (strncmp(url, "https://", 8) == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "LD_PRELOAD= curl -fsL \"%s\"", url);
        FILE *fp = popen(cmd, "r");
        if (!fp) return -1;
        size_t total = 0;
        int ch;
        while ((ch = fgetc(fp)) != EOF && total + 1 < resp_sz) {
            resp[total++] = (char)ch;
        }
        resp[total] = 0;
        int rc = pclose(fp);
        return (rc == 0) ? 0 : -1;
    }

    char host[128] = "localhost";
    int  port      = 80;
    char path[512] = "/";

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    char hostport[256] = {0};
    const char *slash = strchr(p, '/');
    if (slash) {
        strncpy(hostport, p, (size_t)(slash - p));
        strncpy(path, slash, sizeof(path)-1);
    } else {
        strncpy(hostport, p, sizeof(hostport)-1);
    }
    char *colon = strchr(hostport, ':');
    if (colon) { *colon = 0; port = atoi(colon+1); }
    if (hostport[0]) strncpy(host, hostport, sizeof(host)-1);

    struct hostent *srv = gethostbyname(host);
    if (!srv) { printf("%s gethostbyname failed\n", COD1PLUS_TAG); return -1; }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr.s_addr, srv->h_addr, (size_t)srv->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port);

    send(sock, req, strlen(req), 0);

    size_t total = 0;
    ssize_t n;
    while ((n = recv(sock, resp + total, resp_sz - total - 1, 0)) > 0)
        total += (size_t)n;
    resp[total] = 0;
    close(sock);

    char *body = strstr(resp, "\r\n\r\n");
    if (body) {
        body += 4;
        memmove(resp, body, strlen(body) + 1);
    }
    return 0;
}

static int fetch_match_setup(const char *match_id) {
    const char *api_base = getenv("COD1PLUS_API_BASE");
    const char *game = getenv("COD1PLUS_GAME");
    if (!api_base || !*api_base) api_base = "https://fpschallenge.eu/api/v2";
    if (!game || !*game) game = "cod1";

    char url[512];
    snprintf(url, sizeof(url), "%s/%s/match/%s", api_base, game, match_id);

    printf("%s Fetching match config from FPSChallenge for match %s...\n",
           COD1PLUS_TAG, match_id);

    char resp[16384] = {0};
    if (http_get_url(url, resp, sizeof(resp)) != 0) {
        printf("%s Failed to fetch match config\n", COD1PLUS_TAG);
        return -1;
    }

    if (write_matchdata_cfg_from_json(resp, CFG_PATH) == 0) {
        printf("%s Match config fetched and written to matchdata.cfg\n", COD1PLUS_TAG);
        return 0;
    }

    printf("%s Match setup failed: %s\n", COD1PLUS_TAG, resp);
    return -1;
}

/* ------------------------------------------------------------------ */
/* qconsole.log tailer thread                                          */
/* ------------------------------------------------------------------ */

static void *log_tailer_thread(void *arg) {
    (void)arg;
    printf("%s Log tailer started, waiting for matchdata.cfg...\n", COD1PLUS_TAG);

    /* Wait for config to be loaded */
    for (int i = 0; i < 60 && !g_cfg.loaded; i++) sleep(1);
    if (!g_cfg.loaded) {
        printf("%s No match config — log tailer idle\n", COD1PLUS_TAG);
        /* Still tail the log, will build payload without config */
    }

    const char *logpath = g_cfg.logfile[0] ? g_cfg.logfile : "./qconsole.log";
    printf("%s Tailing '%s' for [STATS_EVENT] lines...\n", COD1PLUS_TAG, logpath);

    FILE *f = NULL;
    long  last_pos = 0;

    while (1) {
        sleep(1);

        /* (Re)open the log file */
        if (!f) {
            f = fopen(logpath, "r");
            if (!f) continue;
            /* Seek to end on first open to skip historical log entries */
            if (last_pos == 0) {
                fseek(f, 0, SEEK_END);
                last_pos = ftell(f);
            }
        }

        /* Check if file was rotated (new file smaller than last position) */
        struct stat st;
        if (stat(logpath, &st) == 0 && (long)st.st_size < last_pos) {
            fclose(f);
            f = NULL;
            last_pos = 0;
            continue;
        }

        fseek(f, last_pos, SEEK_SET);
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
            /* PAM announces overtime in the log before the first OT round is played. */
            if (strstr(line, "OverTime;"))
                overtime_signalled();

            /* ...and announces the real end of the map: "MatchEnd;" in match mode,
             * "MapEnd;" in public mode (_end_of_map.gsc::Do_Map_End). This is the only
             * trustworthy "it is over" signal - the scores alone cannot tell a 12-12 that
             * is about to go to overtime from a 12-12 that ends the map because
             * scr_overtime is off. Without it, gating the round_limit clause on a winner
             * would leave a genuine drawn map reported as "playing" forever.
             *
             * sd.gsc runs logStats() BEFORE the limit checks, so the last round's event
             * has already been sent by the time this line appears: re-send it, with the
             * state forced to finished. */
            if (strstr(line, "MatchEnd;") || strstr(line, "MapEnd;")) {
                printf("%s %.20s - map over\n", COD1PLUS_TAG,
                       strstr(line, "MatchEnd;") ? "MatchEnd;" : "MapEnd;");
                if (g_have_last_ev) {
                    char payload[65536];
                    build_payload(&g_cfg, &g_last_ev, payload, sizeof(payload), 1);
                    printf("%s Re-sending final payload as finished\n", COD1PLUS_TAG);
                    if (http_post(g_cfg.api_url, payload) != 0)
                        printf("%s Final payload send FAILED\n", COD1PLUS_TAG);
                }
            }

            if (strstr(line, "[STATS_EVENT]")) {
                printf("%s Event received: %.120s...\n", COD1PLUS_TAG, line);

                round_event_t ev;
                if (parse_event(line, &ev) == 0) {
                    side_tracker_update(&g_cfg, &ev);
                    printf("%s Round %d done — %d players, winner=%s, as=%d xs=%d\n",
                           COD1PLUS_TAG, ev.round, ev.num_players,
                           ev.round_winner, ev.allies_score, ev.axis_score);

                    g_last_ev = ev;
                    g_have_last_ev = 1;

                    char payload[65536];
                    build_payload(&g_cfg, &ev, payload, sizeof(payload), 0);
                    printf("%s Sending payload (%zu bytes) to %s\n",
                           COD1PLUS_TAG, strlen(payload), g_cfg.api_url);
                    printf("%s Payload: %s\n", COD1PLUS_TAG, payload);

                    if (http_post(g_cfg.api_url, payload) == 0)
                        printf("%s Payload sent OK\n", COD1PLUS_TAG);
                    else
                        printf("%s Payload send FAILED\n", COD1PLUS_TAG);
                }
            }
        }
        last_pos = ftell(f);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Constructor / Destructor                                            */
/* ------------------------------------------------------------------ */

static void __attribute__((constructor)) init(void) {
    printf("%s Loaded (v2 — S&D SoloQ)\n", COD1PLUS_TAG);

    /* cod1reloaded server-side: protocol -> 10, master repoint, version gate.
     * Runs in the .so constructor, before the engine main()/SV_Init. */
    cod1reloaded_apply_patches();

    /* read-only: if COD1RELOADED_TICKDIAG=1, log level.time/svs.time rate per second */
    cod1reloaded_start_tickdiag();

    /* cod1reloaded server-side: make the bullet hitbox follow the lean (anti
     * "clip"/corner-peek). Spawns a watcher that hooks game.mp.i386.so once it
     * is loaded and re-installs on map change. COD1RELOADED_LEAN_HITBOX=0 off. */
    lean_hitbox_init();

    /* cod1reloaded: per-bone bullet hit refinement (fine phase on top of the
     * box shift). Off unless COD1RELOADED_PERBONE_HIT is set (dump | on). */
    perbone_hit_init();

    /* cod1reloaded: reproduce the CLIENT's drawn pose (view-locked swing + lean
     * body shift) inside the server's own controllers, so the tested skeleton is
     * the drawn one - the cod2x principle, replacing perbone's compensations.
     * Off unless COD1RELOADED_POSE_SYNC=1. */
    /* cod1reloaded: the server's BG_PlayerAngles gets the client's four swing constants
     * (swing_fix.cpp) - drawn swing == tested swing for players who are NOT leaning
     * too, which pose_sync's lean-gated yaw forcing never covered. 2026-09-09. */
    swing_sync_init();
    pose_sync_init();
    hitbox_draw_init();   /* dev visualiser; no-op unless COD1RELOADED_HITBOX_DRAW=1 */

    /* cod1reloaded: server-side lag compensation. CoD1 has NO native antilag;
     * this implements it and reads the (previously dead) g_antilag cvar so
     * `rcon g_antilag 1` toggles it live. Off unless COD1RELOADED_ANTILAG=1. */
    antilag_init();

    /* stop "Player animation index out of range" drops: clamp instead of Com_Error */
    anim_clamp_init();

    /* publish g_competitive as SYSTEMINFO so modded clients lock their fair-play
     * cvars (the cod2x model). Inert until the cfg sets g_competitive 1. */
    competitive_sv_init();

    /* cvar cheat check (PB replacement part 2). Observe-only by default. */
    cheat_gate_init();

    const char *mode = getenv("COD1PLUS_MODE");
    int live_mode = (!mode || strcasecmp(mode, "live") == 0);
    if (mode && strcasecmp(mode, "dev") == 0) live_mode = 0;

    if (live_mode) {
        char match_id[64] = {0};
        if (parse_cmdline_match_id(match_id, sizeof(match_id)) == 0) {
            printf("%s Match ID from cmdline: %s\n", COD1PLUS_TAG, match_id);
            int ok = 0;
            for (int attempt = 1; attempt <= 10 && !ok; attempt++) {
                if (fetch_match_setup(match_id) == 0) {
                    ok = 1;
                } else {
                    printf("%s Backend not ready, retrying in 2s (%d/10)...\n",
                           COD1PLUS_TAG, attempt);
                    sleep(2);
                }
            }
            if (!ok)
                printf("%s Could not fetch match config — falling back to local file\n",
                       COD1PLUS_TAG);
        } else {
            printf("%s No match id found — using local matchdata.cfg\n", COD1PLUS_TAG);
        }
    } else {
        printf("%s Mode dev — using local matchdata.cfg only\n", COD1PLUS_TAG);
    }

    g_sv_maxclients = parse_cmdline_maxclients();
    printf("%s sv_maxclients=%d\n", COD1PLUS_TAG, g_sv_maxclients);

    cfg_load(&g_cfg, CFG_PATH);
    side_tracker_reset(&g_cfg);

    if (hook_install(&g_sv_directconnect_hook, (uintptr_t)ADDR_SV_DIRECTCONNECT,
                     (uintptr_t)SV_DirectConnect_Hook, 5) == 0) {
        g_sv_directconnect_trampoline = (SV_DirectConnect_t)g_sv_directconnect_hook.trampoline;
        printf("%s SV_DirectConnect hook installed\n", COD1PLUS_TAG);
    } else {
        printf("%s SV_DirectConnect hook failed\n", COD1PLUS_TAG);
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, log_tailer_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s Log tailer thread started\n", COD1PLUS_TAG);
    }

    pthread_t uuid_tid;
    if (pthread_create(&uuid_tid, NULL, uuid_collector_thread, NULL) == 0) {
        pthread_detach(uuid_tid);
        printf("%s UUID collector thread started\n", COD1PLUS_TAG);
    }
}

static void __attribute__((destructor)) fini(void) {
    hook_remove(&g_sv_directconnect_hook);
    printf("%s Unloaded\n", COD1PLUS_TAG);
}
