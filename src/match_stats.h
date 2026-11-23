/*
 * match_stats.h - zPAM-style per-player match stats computed by the server module itself
 * (no gametype script involved). See match_stats.c.
 */
#ifndef MATCH_STATS_H
#define MATCH_STATS_H

#include <stdint.h>
#include <stddef.h>

/* COD1PLUS_STATS=0 switches the whole module off (no hook, no file). */
int  match_stats_enabled(void);

/* gear_force.c's vmMain wrapper, on the game thread:
 *   GAME_INIT, before G_InitGame(levelTime, randomSeed, restart, savepersist)
 *   GAME_RUN_FRAME, after G_RunFrame */
void match_stats_game_init(uintptr_t game_base, int restart, int savepersist);
void match_stats_frame(void);

/* The round-based stats PAM does not compute, for the FPSChallenge payload (cod1plus.c, tailer
 * thread). Values as of the last finished round. Looked up by login uuid, else client slot, else
 * name; returns 0 (and zeros) when the player has no row. clutch_*[1..5] = 1v1..1v5. */
typedef struct {
    int entry_kills, entry_deaths, aces;
    int clutch_won[6], clutch_att[6];
} match_stats_extra_t;
int  match_stats_extra(const char *uuid, int slot, const char *name, match_stats_extra_t *out);

/* cod1plus.c: identity and team the same way the FPSChallenge payload resolves them.
 * cod1plus_client_uuid: the login uuid the client in this slot sent ("" = none).
 * cod1plus_player_identity: uuid (login, else roster by name, else the name itself),
 * "team1"/"team2" and that team's name. side_allies = his side in his last live round. */
int  cod1plus_client_uuid(int slot, char *out, size_t sz);
void cod1plus_player_identity(const char *login, const char *name, int side_allies,
                              char *uuid, size_t usz, char *label, size_t lsz,
                              char *team_name, size_t tsz);

#endif /* MATCH_STATS_H */
