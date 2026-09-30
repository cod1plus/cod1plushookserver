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

/* ghost velocity: a client whose origin is pinned (script link: plant / defuse, frozen
 * bots) but who still has a velocity is drawn pos + velocity * dt by the other clients, up to
 * 9.5 u from where the server tests him. Zero his networked velocity. Once per frame. */
void netstat_bots_freeze(uintptr_t game_base);

#endif
