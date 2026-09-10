/* gear_force.h - g_useGear forced to 0 from the game module's entry (see gear_force.c).
 * No init function: the interposed dlsym is live as soon as the module is preloaded. */
#ifndef GEAR_FORCE_H
#define GEAR_FORCE_H

/* 1 unless COD1RELOADED_GEAR=1 (leave the cvar to the server config). Shared with
 * competitive_sv.c, whose per-frame check uses the same switch. */
int gear_force_enabled(void);

#endif
