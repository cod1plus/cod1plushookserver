/* use_pickup.h - a weapon on the ground gives its ammo on the use key only, not by walking
 * over it. See use_pickup.c. On by default; env COD1RELOADED_USE_PICKUP=0 disables. */
#ifndef USE_PICKUP_H
#define USE_PICKUP_H
void use_pickup_init(void);
#endif
