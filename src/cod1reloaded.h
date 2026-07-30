/*
 * cod1reloaded.h - server-side ecosystem patches for cod_lnxded (CoD1 v1.5)
 *
 * Bumps the network protocol to 10 (separate from vanilla CoD1 = 6), repoints
 * the dead master server, and gates clients by a "cod1reloaded" userinfo version.
 * Pairs with the Windows client (mss32.dll proxy).
 *
 * Env overrides (optional):
 *   COD1RELOADED_PROTOCOL          (default 10)
 *   COD1RELOADED_MASTER            (default "87.106.7.52")
 *   COD1RELOADED_MIN_VERSION       (default 16 = client "1.6")
 *   COD1RELOADED_ALLOW_UNVERSIONED (default 1 = don't reject cod1reloaded==0, e.g. bots)
 */

#ifndef COD1RELOADED_H
#define COD1RELOADED_H

#include "cod1_defs.h"

/* Byte-patch protocol (->10) + master string, from env or defaults. Call once
 * from the .so constructor, before the engine's main() runs. */
void cod1reloaded_apply_patches(void);

/* Call at the top of the SV_DirectConnect hook. Returns 1 to allow the connect
 * (call the trampoline), 0 if the client was rejected (an error packet was
 * already sent; do NOT call the trampoline). */
int cod1reloaded_allow_connect(netadr_t from);

/* Read-only tickrate diagnostic. If COD1RELOADED_TICKDIAG=1, spawns a thread that
 * logs the per-second advance of level.time & svs.time (confirms whether the engine
 * clock is real-time at sv_fps 40). No engine mutation. Call once from the ctor. */
void cod1reloaded_start_tickdiag(void);

#endif /* COD1RELOADED_H */
