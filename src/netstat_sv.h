/* netstat_sv.h - per-client snapshot / rate-limit report, see netstat_sv.c.
 * COD1RELOADED_NETSTAT=<seconds> (0 / unset = off). */
#ifndef NETSTAT_SV_H
#define NETSTAT_SV_H

/* Call once per server frame on the game thread (competitive_sv's G_RunFrame hook).
 * Read-only on the engine; prints every N seconds. */
void netstat_frame(void);

#endif
