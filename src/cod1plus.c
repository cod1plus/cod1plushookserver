/*
 * cod1plus.c  —  CoD1 SoloQ S&D Stats Tracker
 *
 * Injected via LD_PRELOAD into cod_lnxded.
 * At round end, PAM's sd.gsc prints a [STATS_EVENT] line to qconsole.log.
 * This code tails that file, parses the event, merges with matchdata.cfg,
 * and POSTs the full fpschallenge.eu-compatible payload to the local backend.
 *
 * Data flow:
 *   PAM sd.gsc  →  qconsole.log  →  cod1plus.so  →  localhost:3005/api/round_end
 *                                                          ↓
 *                                               fpschallenge.eu (via HTTPS in Node.js)
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
    strncpy(c->api_url,  "http://localhost:3005/api/round_end", sizeof(c->api_url)-1);
    strncpy(c->logfile,  "./qconsole.log",  sizeof(c->logfile)-1);

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
/* Payload builder — produces the fpschallenge.eu JSON                 */
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

/* Return the UUID for a player by name (from match config).
   If no config / no match → returns the name itself as fallback UUID. */
static const char *lookup_uuid(const match_config_t *c, const char *name) {
    for (int i = 0; i < c->num_players; i++)
        if (strcasecmp(c->players[i].name, name) == 0)
            return c->players[i].uuid;
    return name; /* fallback */
}

/* Return "team1" or "team2" for a player by name from config.
   If not found, use GSC team (allies/axis) + halftime state to infer. */
static const char *lookup_team_label(const match_config_t *c,
                                     const char *name,
                                     const char *gsc_team,
                                     int is_halftime)
{
    /* Prefer explicit config mapping */
    for (int i = 0; i < c->num_players; i++) {
        if (strcasecmp(c->players[i].name, name) == 0)
            return c->players[i].team == 1 ? "team1" : "team2";
    }

    /* Fallback: infer from GSC side + halftime.
     * team1_side=1 means team1 starts as allies.
     * After halftime the sides are swapped. */
    int team1_is_allies = (c->team1_side == 1);
    if (is_halftime) team1_is_allies = !team1_is_allies;

    int player_is_allies = (strcmp(gsc_team, "allies") == 0);
    return (player_is_allies == team1_is_allies) ? "team1" : "team2";
}

/* Compute team1_score / team2_score from allies/axis scores + halftime. */
static void resolve_scores(const match_config_t *c,
                            int allies_score, int axis_score, int is_halftime,
                            int *t1, int *t2)
{
    /* After PAM halftime: scores are swapped.
     * Before halftime: allies_score belongs to whoever started as allies. */
    int team1_is_allies = (c->team1_side == 1);
    if (is_halftime) team1_is_allies = !team1_is_allies;

    if (team1_is_allies) { *t1 = allies_score; *t2 = axis_score; }
    else                  { *t1 = axis_score;  *t2 = allies_score; }
}

static int build_payload(const match_config_t *c,
                         const round_event_t *ev,
                         char *out, size_t out_sz)
{
    int t1_score, t2_score;
    resolve_scores(c, ev->allies_score, ev->axis_score, ev->is_halftime,
                   &t1_score, &t2_score);

    /* Determine match state */
    const char *state = "playing";
    if (t1_score >= c->score_limit || t2_score >= c->score_limit ||
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
    {
        FILE *f = fopen("/proc/self/cmdline", "r");
        if (f) {
            char cmdline[2048] = {0};
            fread(cmdline, 1, sizeof(cmdline)-1, f);
            fclose(f);
            /* cmdline is NUL-separated; scan for "+map\0mapname" */
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
        c->num_players > 0 ? c->num_players : ev->num_players,
        c->team1_tag, c->team2_tag,
        g_team1_won_maps, g_team2_won_maps, g_finished_maps,
        t1_score, t2_score,
        mapn, round_str, state);

    /* Players array */
    for (int i = 0; i < ev->num_players && pos < (int)out_sz - 256; i++) {
        const event_player_t *ep = &ev->players[i];
        const char *uuid       = lookup_uuid(c, ep->name);
        const char *team_label = lookup_team_label(c, ep->name, ep->team,
                                                   ev->is_halftime);
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

        char adr_str2[32];
        snprintf(adr_str2, sizeof(adr_str2), "%.2f", ep->adr);

        int n = snprintf(out + pos, out_sz - pos,
            "%s{"
            "\"key\":\"%s\","
            "\"uuid\":\"%s\","
            "\"name\":\"%s\","
            "\"team\":\"%s\","
            "\"team_name\":\"%s\","
            "\"score\":\"%s\","
            "\"kills\":\"%d\","
            "\"assists\":\"%d\","
            "\"damage\":\"%d\","
            "\"deaths\":\"%d\","
            "\"headshots\":\"%d\","
            "\"grenades\":\"%d\","
            "\"plants\":\"%d\","
            "\"defuses\":\"%d\","
            "\"grenade_damage\":\"%d\","
            "\"adr\":\"%s\""
            "}",
            i ? "," : "",
            key_uuid, esc_uuid, esc_name,
            team_label, esc_tname, score_str,
            ep->kills, ep->assists, ep->damage, ep->deaths,
            ep->headshots, ep->grenades, ep->plants, ep->defuses,
            ep->grenade_damage, adr_str2);
        pos += n;
    }

    snprintf(out + pos, out_sz - pos, "]}");
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTTP POST (raw socket, sends to local backend on localhost)         */
/* ------------------------------------------------------------------ */

static int http_post(const char *url, const char *json) {
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

/* ------------------------------------------------------------------ */
/* HTTP GET to local backend (for match setup)                         */
/* ------------------------------------------------------------------ */

#define BACKEND_PORT_DEFAULT 3005

static int http_get(const char *url_path, int port, char *resp, size_t resp_sz) {
    struct hostent *srv = gethostbyname("localhost");
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
        "Host: localhost:%d\r\n"
        "Connection: close\r\n"
        "\r\n",
        url_path, port);

    send(sock, req, strlen(req), 0);

    /* Read full response */
    size_t total = 0;
    ssize_t n;
    while ((n = recv(sock, resp + total, resp_sz - total - 1, 0)) > 0)
        total += (size_t)n;
    resp[total] = 0;
    close(sock);

    /* Skip HTTP headers — find body after \r\n\r\n */
    char *body = strstr(resp, "\r\n\r\n");
    if (body) {
        body += 4;
        memmove(resp, body, strlen(body) + 1);
    }

    return 0;
}

static int fetch_match_setup(const char *match_id) {
    const char *port_env = getenv("COD1PLUS_BACKEND_PORT");
    int port = (port_env && *port_env) ? atoi(port_env) : BACKEND_PORT_DEFAULT;

    char url_path[512];
    snprintf(url_path, sizeof(url_path), "/api/match_setup?id=%s", match_id);

    printf("%s Fetching match config from backend (port %d) for match %s...\n",
           COD1PLUS_TAG, port, match_id);

    char resp[8192] = {0};
    if (http_get(url_path, port, resp, sizeof(resp)) != 0) {
        printf("%s Failed to connect to backend\n", COD1PLUS_TAG);
        return -1;
    }

    if (strstr(resp, "\"ok\":true")) {
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

    /* Check for +match create {id} in command line */
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
    }

    cfg_load(&g_cfg, CFG_PATH);

    pthread_t tid;
    if (pthread_create(&tid, NULL, log_tailer_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s Log tailer thread started\n", COD1PLUS_TAG);
    }
}

static void __attribute__((destructor)) fini(void) {
    printf("%s Unloaded\n", COD1PLUS_TAG);
}
