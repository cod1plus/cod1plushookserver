/*
 * perbone_hit.h - per-bone bullet hit refinement (anti "clip" fine phase).
 *
 * Layers on top of lean_hitbox.c: the AABB shift is the broad phase, this is the
 * fine phase that rejects shots that hit the shifted box but no actual bone.
 *
 * Env COD1RELOADED_PERBONE_HIT: unset/off = not installed (default);
 *   dump = log bone names + nearest-bone result without changing hits;
 *   1/on = full per-bone override.
 * Tuning: COD1RELOADED_PERBONE_LEANFRAC (default 0.5), COD1RELOADED_PERBONE_RADSCALE (1.0).
 */

#ifndef PERBONE_HIT_H
#define PERBONE_HIT_H

/* Start the watcher that installs the trap_LocationalTrace hook on
 * game.mp.i386.so once loaded (re-installs on map reload). No-op unless
 * COD1RELOADED_PERBONE_HIT is set. Safe to call once from the .so constructor. */
void perbone_hit_init(void);

#endif /* PERBONE_HIT_H */
