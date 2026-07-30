/*
 * lean_hitbox.h - server-side: make the bullet hitbox follow the lean.
 *
 * Fixes the CoD1 "clip"/corner-peek exploit where a crouch+lean+strafe player
 * is visible but un-hittable. In CoD1 the bullet hitbox is a single upright box
 * anchored at r.currentOrigin; the lean shifts the visible body / eye / muzzle
 * ~20u sideways but NOT the box, so the leaning body sits off its hitbox.
 *
 * We hook the tail of the game's ClientThink_real and, for a leaning player,
 * shift r.currentOrigin by the SAME offset the engine gives the eye (the game's
 * own AddLeanToPosition) then re-link, so the collision box follows the lean.
 * Render-safe: the networked es.pos.trBase comes from ps.origin, not
 * r.currentOrigin, so the rendered model is untouched (visual lean is applied
 * client-side from es.leanf) -> no double-lean.
 *
 * Disable at runtime with COD1RELOADED_LEAN_HITBOX=0.
 */

#ifndef LEAN_HITBOX_H
#define LEAN_HITBOX_H

/* Start the watcher that installs the hook on game.mp.i386.so once it is loaded
 * and re-installs it whenever the game module is reloaded (map change). Safe to
 * call once from the .so constructor. */
void lean_hitbox_init(void);

#endif /* LEAN_HITBOX_H */
