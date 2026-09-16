/* cheat_gate.h - server side of the cvar cheat check. See cheat_gate.c.
 * Reads the client's USERINFO verdicts:
 *   "cod1x_ac"  cheat cvar names present   (mss32/cheat_scan.cpp)
 *   "cod1x_rs"  embedded ruleset report     (mss32/ruleset.cpp, 1.6.6+)
 * DEFAULT = observe/log only. COD1RELOADED_CHEATGATE = log | kick | 0 */
#ifndef CHEAT_GATE_H
#define CHEAT_GATE_H
void cheat_gate_init(void);
int  cheat_gate_enabled(void);
/* returns 1 if the caller should drop this client */
int  cheat_gate_check(int slot, const char *name, const char *ac);
/* `rs` = raw "cod1x_rs" value (NULL/empty = no report), `want` = the ruleset id the
 * server publishes ("" = none). Returns 1 if the caller should drop this client. */
int  cheat_gate_check_ruleset(int slot, const char *name, const char *rs, const char *want);
/* The userinfo sweep runs on its own thread, so it only QUEUES the drop here ... */
void cheat_gate_request_drop(int slot, const char *name, const char *why);
/* ... and the GAME thread executes it (competitive_sv.c calls this from G_RunFrame). */
void cheat_gate_run_pending(void);
#endif
