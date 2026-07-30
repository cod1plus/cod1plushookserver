/* cheat_gate.h - server side of the cvar cheat check. See cheat_gate.c.
 * Reads the client's USERINFO "cod1x_ac" verdict (published by mss32/cheat_scan.cpp).
 * DEFAULT = observe/log only. COD1RELOADED_CHEATGATE = log | kick | 0 */
#ifndef CHEAT_GATE_H
#define CHEAT_GATE_H
void cheat_gate_init(void);
int  cheat_gate_enabled(void);
/* returns 1 if the caller should drop this client */
int  cheat_gate_check(int slot, const char *name, const char *ac);
#endif
