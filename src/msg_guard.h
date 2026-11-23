#ifndef MSG_GUARD_H
#define MSG_GUARD_H

/* A message that would overflow SV_SendMessageToClient's stack buffer drops that client
 * instead of crashing the server (see msg_guard.c). COD1RELOADED_MSG_GUARD=0 turns it off. */
void msg_guard_init(void);

#endif
