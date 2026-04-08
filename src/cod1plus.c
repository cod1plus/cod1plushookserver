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
        snprintf(c->logfile, sizeof(c->logfile), "%s/main/games_mp.log", fs_homepath);
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
        json_extract_string_range(team1_pos, team1_pos + 2048, "id", team1_id, sizeof(team1_id));
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
        json_extract_string_range(team2_pos, team2_pos + 2048, "id", team2_id, sizeof(team2_id));
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
/* +match create {id} — parse match ID from /proc/self/cmdline         */
/* ------------------------------------------------------------------ */

static int parse_cmdline_match_id(char *out, size_t sz) {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return -1;

    char cmdline[4096] = {0};
    size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
    fclose(f);

    /* cmdline is NUL-separated: arg0\0arg1\0arg2\0...
     * Looking for: +match\0create\0{id}\0 */
    size_t i = 0;
    while (i < n) {
        const char *arg = &cmdline[i];
        size_t len = strlen(arg);
        if (len == 0) { i++; continue; }

        if (strcasecmp(arg, "+match") == 0) {
            size_t next = i + len + 1;
            if (next < n && strcasecmp(&cmdline[next], "create") == 0) {
                size_t id_start = next + strlen(&cmdline[next]) + 1;
                if (id_start < n && cmdline[id_start]) {
                    strncpy(out, &cmdline[id_start], sz - 1);
                    out[sz - 1] = 0;
                    return 0;
                }
            }
        }
        i += len + 1;
    }
    return -1;
}

static void parse_cmdline_fs_homepath(char *out, size_t sz) {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return;
    char cmdline[4096] = {0};
    size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
    fclose(f);
    size_t i = 0;
    while (i < n) {
        const char *arg = &cmdline[i];
        size_t len = strlen(arg);
        if (len == 0) { i++; continue; }
        if (strcasecmp(arg, "fs_homepath") == 0) {
            size_t val = i + len + 1;
            if (val < n && cmdline[val]) {
                strncpy(out, &cmdline[val], sz - 1);
                out[sz - 1] = 0;
                return;
            }
        }
        i += len + 1;
    }
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
/* Format: [STATS_EVENT]r=N,as=N,xs=N,rw=X,ht=N,bp=N,ps=n:t:k:d:a:dm:g:p:df:s|… */
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

    /* Parse player list: name:team:kills:deaths:assists:damage:grenades:plants:defuses:score */
    char *pline = strtok(ps_buf, "|");
    while (pline && ev->num_players < MAX_PLAYERS) {
        event_player_t *ep = &ev->players[ev->num_players];
        char score_str[32] = {0};
        char adr_str[32] = {0};
        int n = sscanf(pline,
            "%63[^:]:%15[^:]:%d:%d:%d:%d:%d:%d:%d:%31[^:]:%d:%d:%31s",
            ep->name, ep->team,
            &ep->kills, &ep->deaths, &ep->assists, &ep->damage,
            &ep->grenades, &ep->plants, &ep->defuses, score_str,
            &ep->headshots, &ep->grenade_damage, adr_str);
        if (n >= 10) {
            ep->score = strtof(score_str, NULL);
            if (n == 13) ep->adr = strtof(adr_str, NULL);
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
        if (!userinfo_get_safe(userinfo, 1024, "login", uuid, sizeof(uuid))) {
            continue;
        }
        if (uuid[0] == 0) continue;
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
static const char *lookup_uuid(const match_config_t *c, const char *name) {
    for (int i = 0; i < c->num_players; i++)
        if (strcasecmp(c->players[i].name, name) == 0)
            return c->players[i].uuid;
    return name; /* fallback */
}

static const char *expected_uuid_for_name(const match_config_t *c, const char *name) {
    if (!c || !c->loaded || !name || !*name) return NULL;
    for (int i = 0; i < c->num_players; i++) {
        if (strcasecmp(c->players[i].name, name) == 0)
            return c->players[i].uuid;
    }
    return NULL;
}

static int client_uuid_status_for_name(const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_client_name[i][0] && strcasecmp(g_client_name[i], name) == 0)
            return g_client_uuid_status[i];
    }
    return -1;
}

/* Return "team1" or "team2" for a player by name from config.
   If not found, use GSC team (allies/axis) + halftime state to infer. */
static const char *lookup_team_label(const match_config_t *c,
                                     const char *name,
                                     const char *gsc_team,
                                     int team1_is_allies)
{
    /* Prefer explicit config mapping */
    for (int i = 0; i < c->num_players; i++) {
        if (strcasecmp(c->players[i].name, name) == 0)
            return c->players[i].team == 1 ? "team1" : "team2";
    }

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

static void side_tracker_reset(const match_config_t *c) {
    int configured_team1_side = (c && c->team1_side == 2) ? 2 : 1;
    g_team1_is_allies_current = (configured_team1_side == 1);
    g_current_score_limit = (c && c->score_limit > 0) ? c->score_limit : 13;
    g_last_event_round = -1;
    g_last_event_ht = -1;
    g_side_tracker_initialized = 1;
}

static void side_tracker_update(const match_config_t *c, const round_event_t *ev) {
    if (!g_side_tracker_initialized) side_tracker_reset(c);
    if (!ev) return;

    int round_reset = (g_last_event_round > 0 && ev->round > 0 && ev->round < g_last_event_round);
    if (round_reset && g_last_event_ht == 1 && ev->is_halftime == 0) {
        int max_score = (ev->allies_score > ev->axis_score) ? ev->allies_score : ev->axis_score;
        int ot_score_limit = max_score + 4;
        if (ot_score_limit > g_current_score_limit)
            g_current_score_limit = ot_score_limit;
        printf("%s Overtime detected: dynamic score_limit=%d\n", COD1PLUS_TAG, g_current_score_limit);
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

    g_last_event_round = ev->round;
    g_last_event_ht = ev->is_halftime;
}

static int build_payload(const match_config_t *c,
                         const round_event_t *ev,
                         char *out, size_t out_sz)
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
    if (t1_score >= effective_score_limit || t2_score >= effective_score_limit ||
        ev->round >= c->round_limit)
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
    int players_count = 0;
    for (int i = 0; i < ev->num_players; i++) {
        const event_player_t *ep = &ev->players[i];
        const char *expected = expected_uuid_for_name(c, ep->name);
        int status = expected ? client_uuid_status_for_name(ep->name) : -1;
        if (expected && status == 0) continue;
        players_count++;
    }

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
        const char *expected   = expected_uuid_for_name(c, ep->name);
        int status             = expected ? client_uuid_status_for_name(ep->name) : -1;
        if (expected && status == 0) continue;
        const char *uuid       = lookup_uuid(c, ep->name);
        const char *team_label = lookup_team_label(c, ep->name, ep->team,
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
             "curl -fsS -X POST -H \"Content-Type: application/json\" --data-binary @- \"%s\" > /dev/null",
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
        snprintf(cmd, sizeof(cmd), "curl -fsL \"%s\"", url);
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
            if (strstr(line, "[STATS_EVENT]")) {
                printf("%s Event received: %.120s...\n", COD1PLUS_TAG, line);

                round_event_t ev;
                if (parse_event(line, &ev) == 0) {
                    side_tracker_update(&g_cfg, &ev);
                    printf("%s Round %d done — %d players, winner=%s, as=%d xs=%d\n",
                           COD1PLUS_TAG, ev.round, ev.num_players,
                           ev.round_winner, ev.allies_score, ev.axis_score);

                    char payload[65536];
                    build_payload(&g_cfg, &ev, payload, sizeof(payload));
                    printf("%s Sending payload (%zu bytes) to %s\n",
                           COD1PLUS_TAG, strlen(payload), g_cfg.api_url);

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
