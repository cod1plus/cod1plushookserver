/* lean_probe.h - DIAGNOSTIC ONLY: see lean_probe.c (COD1RELOADED_LEAN_PROBE=1). */
#ifndef LEAN_PROBE_H
#define LEAN_PROBE_H

#include <stdint.h>

/* Start the hook watcher. No-op unless COD1RELOADED_LEAN_PROBE=1. */
void lean_probe_init(void);

/* Game thread, after G_RunFrame: advance the script, scan at the right moments. */
void lean_probe_frame(uintptr_t base);

#endif
