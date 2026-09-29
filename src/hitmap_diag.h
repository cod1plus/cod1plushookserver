/* hitmap_diag.h - DIAGNOSTIC ONLY: see hitmap_diag.c (COD1RELOADED_HITMAP=1). */
#ifndef HITMAP_DIAG_H
#define HITMAP_DIAG_H

#include <stdint.h>

/* Call on the game thread after G_RunFrame with the game module base. No-op unless
 * COD1RELOADED_HITMAP=1. */
void hitmap_frame(uintptr_t base);

/* One scan of player cn right now (the shot audit calls it on a near miss), written to
 * /tmp/hitmap_shot_<tag>_<cn>.txt (+ .bones). Independent of COD1RELOADED_HITMAP. */
void hitmap_scan_shot(uintptr_t base, int cn, const char* tag);

#endif
