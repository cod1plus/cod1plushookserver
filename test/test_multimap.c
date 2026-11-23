/* test_multimap.c - host test of the match reporting across several maps (BO3 / BO5).
 *
 *   gcc -m32 -O1 -Isrc -o /tmp/test_multimap test/test_multimap.c src/match_stats.c src/hooks.c -lpthread && /tmp/test_multimap
 *
 * Includes src/cod1plus.c itself (COD1PLUS_TEST: no constructor) and feeds it game log
 * lines the way the tailer does, then checks the payloads it would POST.
 */
#define COD1PLUS_TEST
#include "../src/cod1plus.c"

static int g_fail = 0, g_checks = 0;
static char g_payload[65536];

static void check(int ok, const char *what) {
    g_checks++;
    if (!ok) { g_fail++; printf("FAIL: %s\n      payload: %.700s\n", what, g_payload); }
}
static int has(const char *needle) { return strstr(g_payload, needle) != NULL; }

/* one round: team A = players a1..a5, team B = b1..b5, on the given sides */
static int round_line(int r, int allies, int axis, int ht, const char *side_a, const char *side_b) {
    char line[4096];
    int n = snprintf(line, sizeof(line), "  1:00 [STATS_EVENT]r=%d,as=%d,xs=%d,rw=allies,ht=%d,bp=0,ps=", r, allies, axis, ht);
    for (int i = 1; i <= 5; i++)
        n += snprintf(line + n, sizeof(line) - n, "a%d:%s:%d:1:0:100:0:0:0:%d.0:0:0:80.0:-1|", i, side_a, r, r);
    for (int i = 1; i <= 5; i++)
        n += snprintf(line + n, sizeof(line) - n, "b%d:%s:1:%d:0:100:0:0:0:1.0:0:0:80.0:-1%s", i, side_b, r, i < 5 ? "|" : "");
    g_payload[0] = 0;
    return process_log_line(line, g_payload, sizeof(g_payload));
}
static int log_line(const char *l) { g_payload[0] = 0; return process_log_line(l, g_payload, sizeof(g_payload)); }

static void setup(const char *format) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    strcpy(g_cfg.match_id, "337600");
    strcpy(g_cfg.team1_id, "246676"); strcpy(g_cfg.team2_id, "107881");
    strcpy(g_cfg.team1_name, "HIGH5"); strcpy(g_cfg.team2_name, "teamfinnea 2k10");
    strcpy(g_cfg.team1_tag, "HIGH5"); strcpy(g_cfg.team2_tag, "TF2K10");
    strcpy(g_cfg.format, format); strcpy(g_cfg.mr, "MR12");
    g_cfg.team1_side = 1; g_cfg.half_round = 12; g_cfg.score_limit = 13; g_cfg.round_limit = 24;
    strcpy(g_cfg.map, "mp_carentan");
    strcpy(g_cfg.logfile, "/tmp/cod1plus_test/games_mp.log");
    for (int i = 0; i < 10; i++) {
        snprintf(g_cfg.players[i].name, sizeof(g_cfg.players[i].name), "%c%d", i < 5 ? 'a' : 'b', i % 5 + 1);
        snprintf(g_cfg.players[i].uuid, sizeof(g_cfg.players[i].uuid), "uuid-%d", i);
        g_cfg.players[i].team = i < 5 ? 1 : 2;
    }
    g_cfg.num_players = 10;
    g_cfg.loaded = 1;
    g_team1_won_maps = g_team2_won_maps = g_finished_maps = 0;
    g_map_counted = g_match_over = 0;
    memset(g_finished_map_name, 0, sizeof(g_finished_map_name));
    g_current_map[0] = 0;
    side_tracker_reset(&g_cfg);
}

int main(void) {
    mkdir("/tmp/cod1plus_test", 0755);
    remove("/tmp/cod1plus_test/cod1plus_match.state");

    /* ---------------- BO1: unchanged behaviour ---------------- */
    setup("BO1");
    log_line("  0:00 InitGame: \\g_gametype\\sd\\mapname\\mp_carentan\\protocol\\10");
    check(round_line(1, 1, 0, 0, "allies", "axis") == 1, "BO1: a round event is reported");
    check(has("\"state\":\"playing\"") && has("\"map\":\"mp_carentan\""), "BO1: playing on mp_carentan");
    round_line(21, 13, 8, 1, "axis", "allies");
    check(has("\"state\":\"finished\""), "BO1: score limit = finished");
    check(has("\"finishedMapsCount\":\"0\"") && has("\"team1_winnedMaps\":\"0\"") && !has("finishedMap1"),
          "BO1: the map counters never move");
    check(has("\"debug\":\"sd endround\""), "BO1: debug stays 'sd endround'");
    check(log_line(" 35:23 MatchEnd;") == 1 && has("\"state\":\"finished\"") && has("\"finishedMapsCount\":\"0\""),
          "BO1: MatchEnd re-sends finished, counters still 0");

    /* ---------------- BO3, team1 wins 2-0 ---------------- */
    setup("BO3");
    log_line("  0:00 InitGame: \\g_gametype\\sd\\mapname\\mp_carentan\\protocol\\10");
    round_line(1, 1, 0, 0, "allies", "axis");
    check(has("\"format\":\"BO3\"") && has("\"state\":\"playing\"") && has("\"finishedMapsCount\":\"0\""),
          "BO3 map 1: playing, nothing finished");
    check(has("\"team1_score\":\"1\"") && has("\"team2_score\":\"0\""), "BO3 map 1: team1 = allies");
    round_line(12, 8, 4, 0, "allies", "axis");
    /* halftime: sides swap, PAM swaps the scores with the teams */
    round_line(13, 4, 9, 1, "axis", "allies");
    check(has("\"team1_score\":\"9\"") && has("\"team2_score\":\"4\""), "BO3 map 1: after the side swap team1 keeps its score");
    round_line(21, 8, 13, 1, "axis", "allies");
    check(has("\"state\":\"finished\"") && has("\"debug\":\"finishMap\""), "BO3 map 1: last round = finished / finishMap");
    check(has("\"finishedMapsCount\":\"1\"") && has("\"team1_winnedMaps\":\"1\"") && has("\"team2_winnedMaps\":\"0\""),
          "BO3 map 1: 1 map played, 1-0");
    check(has("\"finishedMap1\":\"mp_carentan\"") && !has("finishedMap2"), "BO3 map 1: finishedMap1 = mp_carentan");
    check(has("\"team1_score\":\"13\"") && has("\"team2_score\":\"8\""), "BO3 map 1: 13-8");
    check(log_line(" 35:23 MatchEnd;") == 1 && has("\"finishedMapsCount\":\"1\"") && has("\"team1_winnedMaps\":\"1\""),
          "BO3 map 1: MatchEnd does not count the map twice");
    check(!g_match_over, "BO3: 1-0 is not the end of the match");

    /* map 2: another map, and team1 starts on AXIS this time */
    log_line(" 40:00 InitGame: \\g_gametype\\sd\\mapname\\mp_dawnville\\protocol\\10");
    round_line(1, 0, 1, 0, "axis", "allies");
    check(has("\"map\":\"mp_dawnville\"") && has("\"state\":\"playing\""), "BO3 map 2: playing on mp_dawnville");
    check(has("\"team1_score\":\"1\"") && has("\"team2_score\":\"0\""), "BO3 map 2: team1 read on axis from its players");
    check(has("\"finishedMapsCount\":\"1\"") && has("\"team1_winnedMaps\":\"1\"") && has("\"finishedMap1\":\"mp_carentan\""),
          "BO3 map 2: counters carried over");
    check(has("\"debug\":\"sd endround\""), "BO3 map 2: a normal round is 'sd endround'");
    round_line(13, 8, 5, 1, "allies", "axis");
    round_line(20, 13, 7, 1, "allies", "axis");
    check(has("\"state\":\"finished\"") && has("\"finishedMapsCount\":\"2\"") && has("\"team1_winnedMaps\":\"2\"") &&
          has("\"team2_winnedMaps\":\"0\""), "BO3 map 2: finished, 2-0");
    check(has("\"finishedMap1\":\"mp_carentan\"") && has("\"finishedMap2\":\"mp_dawnville\""), "BO3 map 2: both maps listed");
    check(g_match_over, "BO3: 2-0 decides the match");
    check(log_line(" 70:00 MatchEnd;") == 1 && has("\"team1_winnedMaps\":\"2\"") && has("\"finishedMapsCount\":\"2\""),
          "BO3 map 2: MatchEnd re-send keeps 2-0");
    check(round_line(1, 1, 0, 0, "allies", "axis") == 0, "BO3: nothing is reported once the match is decided");

    /* ---------------- BO3 going to map 3, with an overtime and a lost map ---------------- */
    setup("BO3");
    log_line("  0:00 InitGame: \\mapname\\mp_harbor\\x");
    round_line(24, 12, 12, 1, "axis", "allies");
    check(has("\"state\":\"playing\""), "BO3: 12-12 after 24 rounds is not finished");
    log_line(" 30:00 OverTime;");
    round_line(1, 13, 12, 0, "axis", "allies");
    check(has("\"state\":\"playing\"") && has("\"finishedMapsCount\":\"0\""), "BO3: overtime keeps playing");
    round_line(4, 16, 12, 1, "axis", "allies");         /* allies = team2 wins the overtime */
    check(has("\"state\":\"finished\"") && has("\"team2_winnedMaps\":\"1\"") && has("\"team1_winnedMaps\":\"0\""),
          "BO3: map 1 lost by team1 in overtime -> 0-1");
    log_line(" 40:00 MatchEnd;");
    log_line(" 41:00 InitGame: \\mapname\\mp_carentan\\x");
    round_line(1, 1, 0, 0, "allies", "axis");
    check(has("\"state\":\"playing\"") && has("\"finishedMapsCount\":\"1\""), "BO3: map 2 starts with the base score limit");
    round_line(18, 13, 5, 1, "axis", "allies");         /* allies = team2?? no: team1 is on axis here */
    check(has("\"state\":\"finished\""), "BO3: map 2 finished");
    check((has("\"team1_winnedMaps\":\"0\"") && has("\"team2_winnedMaps\":\"2\"")), "BO3: team2 takes it 0-2 (team1 on axis lost 5-13)");

    /* ---------------- restart between two maps: the counters come back ---------------- */
    setup("BO3");
    remove("/tmp/cod1plus_test/cod1plus_match.state");
    log_line("  0:00 InitGame: \\mapname\\mp_carentan\\x");
    round_line(20, 13, 7, 1, "allies", "axis");
    log_line(" 30:00 MatchEnd;");
    g_team1_won_maps = g_team2_won_maps = g_finished_maps = 0; g_map_counted = 0;      /* the process restarts */
    memset(g_finished_map_name, 0, sizeof(g_finished_map_name));
    match_state_load();
    check(g_finished_maps == 1 && g_team1_won_maps == 1 && strcmp(g_finished_map_name[0], "mp_carentan") == 0,
          "restart: counters and map names restored from the state file");
    strcpy(g_cfg.match_id, "999");
    g_finished_maps = 0; g_team1_won_maps = 0;
    match_state_load();
    check(g_finished_maps == 0, "restart: another match does not inherit the file");

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
