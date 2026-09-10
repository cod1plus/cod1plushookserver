/*
 * glibc_compat.h - force the classic libc symbols, whatever the build host's glibc is.
 *
 * THE TRAP. glibc 2.38 (Ubuntu 23.10+, WSL 24.04, most current build hosts) added the C23
 * "0b" prefix to the strtol family and redirects, in its own headers, every call to
 *     strtol strtoul sscanf fscanf
 * onto NEW symbols __isoc23_* versioned GLIBC_2.38. _GNU_SOURCE - which this project needs
 * for RTLD_NEXT and friends - switches that redirect ON, and there is no supported macro to
 * switch it back off (features.h #undefs __GLIBC_USE_C2X_STRTOL before computing it from
 * __GLIBC_USE (ISOC2X), so -D on it is ignored).
 *
 * The result is a module that compiles and links without a word of warning and then refuses
 * to load on the game server:
 *     ERROR: ld.so: object './cod1plus.so' cannot be preloaded: symbol __isoc23_strtol,
 *     version GLIBC_2.38 not defined
 * The server starts with NO hooks at all - no version gate, no stats, no hitbox work - and
 * nothing in the game log says why. That is worth a header to prevent.
 *
 * THE FIX. Declare our own names bound, by asm label, to the UNVERSIONED classic symbols,
 * then macro our calls onto them. The header redirect never gets a chance to apply, the
 * binary imports strtol@GLIBC_2.0 like it always did, and the same source still builds on
 * an old glibc where none of this exists (there the asm label just names the same function
 * the header would have given us).
 *
 * Included ahead of every translation unit from scripts/build.sh (-include), so it must
 * pull in the real headers itself before renaming anything.
 */

#ifndef COD1PLUS_GLIBC_COMPAT_H
#define COD1PLUS_GLIBC_COMPAT_H

#ifdef __linux__

#include <stdio.h>
#include <stdlib.h>

/* The asm label is what the linker records; the redirecting declaration in the glibc
 * header applies to `strtol`, not to these names, so it cannot reach them. */
extern long int      cod1plus_strtol(const char *, char **, int) __asm__("strtol");
extern unsigned long cod1plus_strtoul(const char *, char **, int) __asm__("strtoul");
extern int           cod1plus_sscanf(const char *, const char *, ...) __asm__("sscanf");
extern int           cod1plus_fscanf(FILE *, const char *, ...) __asm__("fscanf");

#define strtol  cod1plus_strtol
#define strtoul cod1plus_strtoul
#define sscanf  cod1plus_sscanf
#define fscanf  cod1plus_fscanf

#endif /* __linux__ */

#endif /* COD1PLUS_GLIBC_COMPAT_H */
