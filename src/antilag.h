/*
 * antilag.h - server-side lag compensation (Linux). See antilag.c.
 *
 * Master switch: env COD1RELOADED_ANTILAG=1 (unset/0 = not installed).
 * Live toggle:   cvar g_antilag (PAM already sets it) -> `rcon g_antilag 1`.
 * Override:      env COD1RELOADED_ANTILAG_FORCE=1|0 ignores the cvar.
 */
#ifndef ANTILAG_H
#define ANTILAG_H
void antilag_init(void);
#endif
