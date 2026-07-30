/* anim_clamp.h - clamp BG_GetAnimationForIndex out-of-range instead of dropping.
 * See anim_clamp.c. Always on; env COD1RELOADED_ANIM_CLAMP=0 disables. */
#ifndef ANIM_CLAMP_H
#define ANIM_CLAMP_H
void anim_clamp_init(void);
#endif
