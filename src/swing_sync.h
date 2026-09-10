/*
 * swing_sync.h - make the SERVER's swing state match what the CLIENT draws.
 *
 * swing_fix.cpp (client, restored 2026-07-31) patches four constants in cgame's
 * BG_PlayerAngles so the drawn legs/torso/lean track the view with no dead zone.
 * The same function is compiled into game.mp.i386.so and the server runs it in
 * ClientEndFrame to fill the clientinfo fields that BG_Player_DoControllersInternal
 * turns into the skeleton a locational bullet trace walks. Until 2026-09-09 the
 * server ran it with the VANILLA constants, so for every player who was NOT leaning
 * the tested skeleton was posed from different inputs than the drawn one. This
 * module patches the server copy the same way: same code, same constants, same
 * inputs - drawn == tested by construction, the cod2x principle.
 *
 * ON by default. COD1RELOADED_SWING_SYNC=0 disables it (pose_sync then falls back
 * to its lean-only yaw forcing).
 */
#ifndef SWING_SYNC_H
#define SWING_SYNC_H

void swing_sync_init(void);

/* Non-zero while all four sites are patched in the currently loaded game module.
 * pose_sync reads it on every controller call: when the swing constants already
 * match the client, its own yaw forcing (legs = view, valid only for a player who
 * stands still) must stay out of the way. */
int swing_sync_installed(void);

#endif
