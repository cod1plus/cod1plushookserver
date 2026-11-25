#ifndef MATCH_TEAMS_H
#define MATCH_TEAMS_H

#include <stddef.h>
#include <stdint.h>

/* The FPSChallenge teams of the match, published for the PAM (match_teams.c). */
void match_teams_game_init(uintptr_t game_base);   /* GAME_INIT, from the vmMain wrapper */
void match_teams_frame(void);                      /* GAME_RUN_FRAME, same thread */

/* What cod1plus.c knows of the match (matchdata.cfg), read by match_teams.c.
 * cod1plus_match_team: name and tag of team 1 or 2; 0 = no match config.
 * cod1plus_match_team1_allies: the payload's side tracker (configured side, half-times,
 *   the players of the last round event): 1 = team 1 plays allies.
 * cod1plus_roster_team: 1 / 2 = the roster team of a player (login first, then name), 0 = none. */
int cod1plus_match_team(int t, char *name, size_t nsz, char *tag, size_t tsz);
int cod1plus_match_team1_allies(void);
int cod1plus_roster_team(const char *login, const char *name);

#endif
