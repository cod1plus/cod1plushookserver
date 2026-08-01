/*
 * pose_sync.h - make the server's posed skeleton match the client's drawn model.
 *
 * The cod2x principle applied to CoD1: instead of compensating in the hit test for
 * what our client mod draws differently (swing_fix's view-locked torso, lean_fix's
 * body shift), reproduce both inside the server's own BG_Player_DoControllers pose.
 * The tested skeleton then IS the drawn one, and perbone's mirror/corner/union
 * compensations can be switched off (COD1RELOADED_PERBONE_LEANFRAC=0).
 *
 * Env COD1RELOADED_POSE_SYNC: unset/0 = not installed (default); 1 = enabled.
 * Sub-switches for bisecting a live problem, both on when enabled:
 *   COD1RELOADED_POSE_SYNC_YAW=0    - do not force torso/legs yaw to the view
 *   COD1RELOADED_POSE_SYNC_SHIFT=0  - do not inject the lean body shift
 *   COD1RELOADED_POSE_SYNC_DEBUG=1  - log one line per 200 synced poses
 *
 * Stand + alive + leaning only: CoD1 zeroes the lean controllers in crouch/prone on
 * BOTH sides already, and forcing anything there once rotated a hidden crouching
 * player out of his cover.
 */

#ifndef POSE_SYNC_H
#define POSE_SYNC_H

/* Start the watcher that installs the BG_Player_DoControllers hook on
 * game.mp.i386.so once loaded (re-installs after a map reload). No-op unless
 * COD1RELOADED_POSE_SYNC is set. Safe to call once from the .so constructor. */
void pose_sync_init(void);

#endif /* POSE_SYNC_H */
