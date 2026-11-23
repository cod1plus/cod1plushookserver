/*
 * test_match_stats.c - drive match_stats.c's rules on the host with a fake game module.
 *
 *   gcc -m32 -O1 -Wall -Wextra -include src/glibc_compat.h -o /tmp/tms test/test_match_stats.c && /tmp/tms
 *
 * g_entities / level / gclients live in a plain buffer at the real RVAs; the engine calls the
 * module would make (finishPlayerDamage -> ent->die = player_die, logPrint) are replayed in
 * the same order, through the module's own hook functions. 32-bit only: the module reads
 * 4-byte pointers out of the entities, as in the game.
 */
#include "../src/match_stats.c"

#include <assert.h>

/* ---- what cod1plus.c provides in the real module -------------------------------------- */
static char t_login[MAX_SLOTS][64];
int cod1plus_client_uuid(int slot, char *out, size_t sz)
{
    snprintf(out, sz, "%s", (slot >= 0 && slot < MAX_SLOTS) ? t_login[slot] : "");
    return out[0] != 0;
}
void cod1plus_player_identity(const char *login, const char *name, int side_allies,
                              char *uuid, size_t usz, char *label, size_t lsz,
                              char *tname, size_t tsz)
{
    snprintf(uuid, usz, "%s", (login && login[0]) ? login : name);
    snprintf(label, lsz, "%s", side_allies ? "team1" : "team2");
    snprintf(tname, tsz, "%s", side_allies ? "Allies Team" : "Axis Team");
}

/* ---- fake game module ----------------------------------------------------------------- */
#define CLIENT_SIZE 0x22cc
static unsigned char *mem;
static unsigned char  t_clients[MAX_SLOTS][CLIENT_SIZE];
static char          *t_attacker;      /* script arg 1 of the next finishPlayerDamage */
static int            t_damage, t_mod;
static int            t_logged;

static int   s_numparam(void) { return 10; }
static int   s_gettype(unsigned int i) { (void)i; return 1; }
static int   s_ptrtype(unsigned int i) { (void)i; return SCR_ENTITY_PTR; }
static char *s_getentity(unsigned int i) { (void)i; return t_attacker; }
static void  s_logprintf(const char *fmt, ...) { (void)fmt; t_logged++; }

static int *I(char *p, int off) { return (int *)(p + off); }
static int  health(int n) { return *I(ent(n), E_HEALTH); }

static void s_player_die(char *self, char *inflictor, char *attacker, int damage, int mod,
                         int weapon, const float *dir, int hitloc)
{
    (void)inflictor; (void)attacker; (void)damage; (void)mod; (void)weapon; (void)dir; (void)hitloc;
    char *c = *(char **)(self + E_CLIENT);
    if (*I(c, C_PM_TYPE) > PM_INTERMISSION) return;
    *I(c, C_PM_TYPE) = 6;                   /* PM_DEAD */
    *I(c, C_SESS_STATE) = 1;                /* PAM: self.sessionstate = "dead" */
}

/* the method body: health down, ent->die when it reaches 0 */
static void s_finish_damage(int entnum)
{
    char *v = ent(entnum);
    *I(v, E_HEALTH) -= t_damage;
    if (*I(v, E_HEALTH) <= 0)
        hook_player_die(v, t_attacker, t_attacker, t_damage, t_mod, 0, NULL, 0);
}

static void spawn(int n, const char *name, int team)
{
    char *e = ent(n);
    unsigned char *c = t_clients[n];
    memset(c, 0, CLIENT_SIZE);
    *(e + E_INUSE) = 1;
    *(char **)(e + E_CLIENT) = (char *)c;
    *I(e, E_HEALTH) = 100;
    *I((char *)c, C_SESS_TEAM) = team;
    *I((char *)c, C_SESS_STATE) = 0;
    *I((char *)c, C_PM_TYPE) = 0;
    snprintf((char *)c + C_NAME, 32, "%s", name);
}

static void hit(int a, int v, int dmg, int mod)
{
    t_attacker = ent(a);
    t_damage = dmg;
    t_mod = mod;
    hook_finish_damage(v);
}

static void logline(const char *s) { hook_script_logprint(s); }
static void frame(void) { *I((char *)(g_base + RVA_LEVEL), L_TIME) += 50; match_stats_frame(); }
static void set_time(int ms) { *I((char *)(g_base + RVA_LEVEL), L_TIME) = ms; }
static int *score(int team) { return I((char *)(g_base + RVA_LEVEL), team == TEAM_AXIS ? L_SCORE_AXIS : L_SCORE_ALLIES); }
static rec_t *P(int n) { return &g_rec[g_slot[n].rec]; }

int main(void)
{
    mem = calloc(1, RVA_G_ENTITIES + MAX_GENTITIES * GENTITY_SIZE + 0x1000);
    assert(mem);
    snprintf(g_json_path, sizeof(g_json_path), "/tmp/test_match_stats.json");
    g_enabled = 1; g_verbose = 1;
    g_base = (uintptr_t)mem;
    p_numparam = s_numparam; p_gettype = s_gettype; p_ptrtype = s_ptrtype; p_getentity = s_getentity;
    orig_finish_damage = s_finish_damage; orig_player_die = s_player_die; real_logprintf = s_logprintf;
    snprintf(g_map, sizeof(g_map), "mp_harbor");
    reset_map();
    g_on = 1;

    /* 5v5: 0-4 allies, 5-9 axis; slot 1 has a login */
    const char *names[10] = { "^1al^7pha", "bravo", "charlie", "delta", "echo",
                              "foxtrot", "golf", "hotel", "india", "juliet" };
    for (int n = 0; n < 10; n++) spawn(n, names[n], n < 5 ? TEAM_ALLIES : TEAM_AXIS);
    snprintf(t_login[1], 64, "d21b16a8-9eeb-4215-9be5-5c63d3368deb");

    /* ready-up: no RoundStart; -> nothing counts */
    frame();
    hit(0, 5, 150, MOD_HEAD_SHOT);
    assert(P(0)->kills == 0 && P(5)->deaths == 0 && P(0)->damage == 0);
    spawn(5, "foxtrot", TEAM_AXIS);

    /* ---- round 1 ---- */
    logline("RoundStart;\n");
    assert(t_logged == 1);                       /* the text still reaches G_LogPrintf */
    set_time(10000);
    frame();
    hit(0, 5, 60, 2);                            /* alpha 60 on foxtrot */
    assert(P(0)->damage == 60 && health(5) == 40);
    frame();
    hit(1, 5, 50, MOD_HEAD_SHOT);                /* bravo finishes: 40 counted, entry, assist alpha */
    assert(P(1)->damage == 40 && P(1)->kills == 1 && P(1)->headshots == 1);
    assert(P(1)->entry_kills == 1 && P(5)->entry_deaths == 1 && P(5)->deaths == 1);
    assert(P(0)->assists == 1 && P(0)->score2 == 1 && P(1)->score2 == 2);
    frame();
    /* golf kills alpha, charlie, delta, echo with a grenade -> bravo alone vs 4 */
    hit(6, 0, 200, MOD_GRENADE_SPLASH); frame();
    hit(6, 2, 200, MOD_GRENADE_SPLASH); frame();
    hit(6, 3, 200, 2); frame();
    assert(r_clutch[TEAM_ALLIES] < 0);
    hit(6, 4, 200, 2); frame();
    assert(r_clutch[TEAM_ALLIES] == g_slot[1].rec && r_clutch_enemies[TEAM_ALLIES] == 4);
    assert(P(6)->grenades == 2 && P(6)->kills == 4 && P(6)->entry_kills == 0);
    /* second hit on a dead player: player_die ignores it, so do we */
    hit(7, 0, 50, 2);
    assert(P(0)->deaths == 1 && P(7)->kills == 0);
    /* bravo kills the 4 left -> ace (5 kills, axis size 5) + 1v4 won */
    for (int n = 6; n <= 9; n++) { hit(1, n, 120, 2); frame(); }
    assert(P(1)->kills == 5);
    *score(TEAM_ALLIES) += 1;                    /* endRound: setTeamScore, same frame */
    frame();
    assert(!r_active && g_rounds == 1);
    assert(P(1)->aces == 1 && P(1)->clutch_att[4] == 1 && P(1)->clutch_won[4] == 1);
    assert(P(6)->aces == 0);
    logline("Winners;allies;1;bravo\n");         /* already closed by the score: ignored */
    assert(g_rounds == 1);

    /* ---- round 2 (map_restart(true): rows kept, slots rebound) ---- */
    match_stats_game_init(0, 1, 1);              /* base 0: stays out of install() */
    for (int n = 0; n < 10; n++) spawn(n, names[n], n < 5 ? TEAM_ALLIES : TEAM_AXIS);
    spawn(1, "bravo^3[TAG]", TEAM_ALLIES);       /* renamed: the login keeps his row */
    logline("RoundStart;\n");
    set_time(90000);
    frame();
    assert(P(1)->kills == 5 && strcmp(P(1)->name, "bravo^3[TAG]") == 0);
    logline("Bomb;0;7;axis;hotel;bomb_plant\n");
    assert(P(7)->plants == 1 && P(7)->score2 == 1);
    hit(7, 8, 150, 2);                           /* team kill */
    assert(P(7)->score2 == -1 && P(7)->kills == 0 && P(8)->deaths == 2);
    frame();
    /* juliet switches to spectator: `self suicide()` + sessionteam */
    hook_player_die(ent(9), ent(9), ent(9), 100000, MOD_SUICIDE, 0, NULL, 0);
    assert(P(9)->deaths == 2);
    *I((char *)t_clients[9], C_SESS_TEAM) = 3;
    frame();
    assert(P(9)->deaths == 1);
    /* a plain /kill stays a death */
    hook_player_die(ent(5), ent(5), ent(5), 100000, MOD_SUICIDE, 0, NULL, 0);
    frame();
    assert(P(5)->deaths == 2);
    /* first enemy kill of the round is still the entry (a TK is not) */
    hit(2, 6, 150, 2);
    assert(P(2)->entry_kills == 1 && P(6)->entry_deaths == 1);
    frame();
    logline("Winners;axis;5;foxtrot\n");
    assert(!r_active && g_rounds == 2);

    /* what the FPSChallenge payload reads (tailer thread): login, slot, then name */
    {
        match_stats_extra_t x;
        assert(match_stats_extra("d21b16a8-9eeb-4215-9be5-5c63d3368deb", -1, "whatever", &x) &&
               x.entry_kills == 1 && x.aces == 1 && x.clutch_won[4] == 1 && x.clutch_att[4] == 1);
        assert(match_stats_extra("", 7, "", &x) && x.clutch_won[5] == 1 && x.clutch_att[5] == 1);
        assert(match_stats_extra("", -1, "^2Golf", &x) && x.entry_deaths == 1);
        assert(!match_stats_extra("nobody", -1, "nobody", &x) && x.aces == 0);
    }

    /* ---- PAM's line for the same players: identical -> 0 mismatch lines ---- */
    {
        char line[512];
        rec_t *b = P(1);
        snprintf(line, sizeof(line), "[STATS_EVENT]r=2,as=1,xs=1,rw=axis,ht=0,bp=1,ps="
                 "bravo:allies:%d:%d:%d:%d:%d:%d:%d:%d.%d:%d:0:0:1|hotel:axis:0:1:0:0:0:1:0:-0.5:0:0:0:7\n",
                 b->kills, b->deaths, b->assists, b->damage, b->grenades, b->plants, b->defuses,
                 b->score2 / 2, (b->score2 % 2) ? 5 : 0, b->headshots);
        logline(line);
    }

    /* ---- new map: everything cleared ---- */
    match_stats_game_init(0, 0, 0);
    assert(g_rounds == 0 && !g_rec[0].used);

    printf("\nALL TESTS PASSED - last JSON: %s\n", g_json_path);
    return 0;
}
