/*
 * pose_sync.h - make the server's posed skeleton match the client's drawn model.
 *
 * The cod2x principle applied to CoD1: rather than compensating in the hit test for what
 * our client mod draws differently, the two sides run THE SAME CODE on the controller
 * buffer. That code is src/shared/lean_controllers.c, compiled into cod1plus.so and into
 * the client's mss32.dll from a byte-identical copy. This module is only the server's
 * hook and the two facts the shared code cannot know on its own (stance, and how many
 * lateral units this binary's engine already applied). The tested skeleton then IS the
 * drawn one, and perbone's mirror/corner/union compensations can stay off
 * (COD1RELOADED_PERBONE_LEANFRAC=0).
 *
 * ON BY DEFAULT since 2026-08-10, with the validated settings compiled in: a server
 * started with NO environment at all is the correct one. Every variable is an override.
 *   COD1RELOADED_POSE_SYNC=0       - disable entirely (drawn and tested stop agreeing).
 *   COD1RELOADED_POSE_SYNC_YAW=0   - stop forcing torso/legs yaw to the view before
 *                                    posing. On by default because swing_fix.cpp patches
 *                                    BG_PlayerAngles CLIENT-SIDE ONLY: without the
 *                                    forcing the server's legs stay planted up to 40 deg
 *                                    behind the view while the client's track it, and the
 *                                    back-bone reprojection turns that into up to 7.6 deg
 *                                    of pitch on the bone carrying the head. Measured.
 *   COD1RELOADED_LEAN_CONST=0      - leave the .rodata constant at its stock 2.5. The
 *                                    shared code then contributes the missing 5.0 itself,
 *                                    so the final pose is the same either way; this only
 *                                    chooses where the constant lives.
 *   COD1RELOADED_CTRL_DUMP=N       - dump N controller-buffer samples per client, in the
 *                                    same format the client writes to cod1reloaded.log,
 *                                    so the two can be diffed line against line.
 *
 * RETIRED (warned about at startup if still set): POSE_SYNC_SHIFT, POSE_SYNC_UL,
 * POSE_SYNC_UR, POSE_SYNC_UL_CROUCH, POSE_SYNC_UR_CROUCH, POSE_SYNC_DIAG,
 * POSE_SYNC_DIAG_K, POSE_SYNC_DEBUG, POSE_SYNC_PROBE. They tuned this module's private
 * copy of the client's adjustments; there is no private copy any more.
 *
 * NOTE on an old claim in this header: "CoD1 zeroes the lean controllers in crouch/prone
 * on both sides" is FALSE and cost several days. The engine's memset at 0x1a39f is gated
 * on es->eFlags & 0xc000, which is not the crouch bit - server logs on 2026-08-10 show a
 * full lean fraction (+/-0.750) reaching the buffer at box height 50. Crouch is a live
 * case, not a dead one.
 */

#ifndef POSE_SYNC_H
#define POSE_SYNC_H

/* Start the watcher that installs the BG_Player_DoControllers hook on
 * game.mp.i386.so once loaded (re-installs after a map reload). No-op unless
 * COD1RELOADED_POSE_SYNC is set. Safe to call once from the .so constructor. */
void pose_sync_init(void);

#endif /* POSE_SYNC_H */
