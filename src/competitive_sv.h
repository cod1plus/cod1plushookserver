/* competitive_sv.h - publish competitive.cfg as CVAR_SYSTEMINFO cvars so clients lock /
 * clamp their fair-play cvars, plus the embedded-ruleset id (sv_competitive_ruleset).
 * See competitive_sv.c. Env COD1RELOADED_COMPETITIVE=0 disables, COD1RELOADED_RULESET
 * picks the ruleset id (default codbase-2023-05, 0 = none). */
#ifndef COMPETITIVE_SV_H
#define COMPETITIVE_SV_H
void        competitive_sv_init(void);
/* id published to clients ("" when the module is off or COD1RELOADED_RULESET=0) */
const char *competitive_sv_ruleset_id(void);
#endif
