/* competitive_sv.h - publish g_competitive as CVAR_SYSTEMINFO so clients lock their
 * fair-play cvars (com_maxfps/snaps/cl_maxpackets/rate). See competitive_sv.c.
 * Env COD1RELOADED_COMPETITIVE=0 disables. */
#ifndef COMPETITIVE_SV_H
#define COMPETITIVE_SV_H
void competitive_sv_init(void);
#endif
