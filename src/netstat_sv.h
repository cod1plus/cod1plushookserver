/* netstat_sv.h - per-client snapshot / rate-limit report, see netstat_sv.c.
 * COD1RELOADED_NETSTAT=<seconds> (0 / unset = off). */
#ifndef NETSTAT_SV_H
#define NETSTAT_SV_H

/* Call once per server frame on the game thread (competitive_sv's G_RunFrame hook).
 * Read-only on the engine; prints every N seconds. */
void netstat_frame(void);

/* 1 if slot cn is a test client (netchan address type NA_BOT), 0 otherwise or when
 * the client table cannot be read. Cheap: two loads. */
int netstat_is_bot(int cn);

/* zero the ghost velocity of every test client (they are drawn pos + velocity * dt by the
 * client, 7-15 u ahead of where the server tests them). Once per frame. */
void netstat_bots_freeze(uintptr_t game_base);

#endif
