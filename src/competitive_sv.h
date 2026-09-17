/* competitive_sv.h - publish competitive.cfg as CVAR_SYSTEMINFO cvars so clients lock /
 * clamp their fair-play cvars, plus the ruleset id (sv_competitive_ruleset, from
 * ruleset.txt re-read live, else env COD1RELOADED_RULESET, default codbase-2023-05;
 * "<id>@<version>" makes clients behind that version re-download). See competitive_sv.c.
 * Env COD1RELOADED_COMPETITIVE=0 disables both. */
#ifndef COMPETITIVE_SV_H
#define COMPETITIVE_SV_H
void        competitive_sv_init(void);
/* id published to clients, "<base>" or "<base>@<version>" ("" when off / none) */
const char *competitive_sv_ruleset_id(void);
#endif
