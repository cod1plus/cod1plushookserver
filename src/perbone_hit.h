/*
 * perbone_hit.h - per-bone bullet hit refinement (anti "clip" fine phase).
 *
 * Layers on top of lean_hitbox.c: the AABB shift is the broad phase, this is the
 * fine phase that rejects shots that hit the shifted box but no actual bone.
 *
 * Env COD1RELOADED_PERBONE_HIT: unset/0/off = not installed (default);
 *   dump  = log bone names + nearest-bone result without changing hits;
 *   pose  = fresh-pose leaners, engine still decides;
 *   grant = ADD-ONLY. Never rejects, downgrades or re-attributes an engine hit;
 *           only upgrades a total miss (ENTITYNUM_WORLD/NONE) on the ONE trace
 *           Bullet_Fire_Extended makes, against the DRAWN position of a standing,
 *           living, leaning victim. This is the tournament-safe mode.
 *   1     = full per-bone override (contains the reject path that ate bullets).
 * NOTE: do NOT pass "on" - the off-test matches its leading 'o'.
 * Tuning: COD1RELOADED_PERBONE_LEANFRAC (default 0.5), COD1RELOADED_PERBONE_RADSCALE (1.0).
 * grant:  COD1RELOADED_PERBONE_GRANT_MAXDIST (64), _GRANT_STANDONLY (1),
 *         _GRANT_MINENT (1022), _SHIFT_SL/_SR (5.0/5.0), _SHIFT_TAPER (1).
 */

#ifndef PERBONE_HIT_H
#define PERBONE_HIT_H

/* Start the watcher that installs the trap_LocationalTrace hook on
 * game.mp.i386.so once loaded (re-installs on map reload). No-op unless
 * COD1RELOADED_PERBONE_HIT is set. Safe to call once from the .so constructor. */
void perbone_hit_init(void);

#endif /* PERBONE_HIT_H */
