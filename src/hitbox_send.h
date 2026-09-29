/* hitbox_send.h - stream the SERVER's tested hit boxes of the player a client is looking at
 * to that client, so its cod1reloaded can draw them over the model (server cvar
 * cod1x_drawhitbox 1 via rcon; the client draws with its own cod1x_drawhitbox 1).
 * See hitbox_send.c. */
#ifndef HITBOX_SEND_H
#define HITBOX_SEND_H

#include <stdint.h>

/* Call once per server frame on the game thread after the pose pass (competitive_sv's
 * G_RunFrame hook). Does nothing unless the server cvar cod1x_drawhitbox is non-zero. */
void hitbox_send_frame(uintptr_t game_base);

#endif
