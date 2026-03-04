/*
 * hooks.h - x86 function hooking mechanism
 *
 * JMP detour hooking with trampoline for x86 (32-bit) Linux.
 * Patches the first 5 bytes of a target function with a relative JMP
 * to our hook function. A trampoline is allocated that contains the
 * original stolen bytes + a JMP back to the rest of the original function.
 */

#ifndef HOOKS_H
#define HOOKS_H

#include <stdint.h>

/* A hook instance tracks one detoured function */
typedef struct {
    uintptr_t target_addr;      /* Address of the function we're hooking */
    uintptr_t hook_addr;        /* Address of our replacement function */
    uintptr_t trampoline;       /* Allocated trampoline (original bytes + JMP back) */
    uint8_t   original_bytes[16]; /* Saved original bytes */
    int       patch_len;        /* Number of bytes overwritten (>= 5) */
    int       active;           /* Whether the hook is currently installed */
} hook_t;

/*
 * hook_install - Install a JMP detour hook
 *
 * @hook:       Pointer to hook_t structure (will be filled in)
 * @target:     Address of the function to hook
 * @replacement: Address of the hook function
 * @patch_len:  Number of bytes to overwrite (minimum 5). If 0, defaults to 5.
 *
 * Returns 0 on success, -1 on error.
 *
 * After successful install:
 *   - hook->trampoline can be cast to the original function type and called
 *   - The target function will redirect to replacement
 */
int hook_install(hook_t *hook, uintptr_t target, uintptr_t replacement, int patch_len);

/*
 * hook_remove - Remove a previously installed hook
 *
 * Restores the original bytes at the target address and frees the trampoline.
 */
int hook_remove(hook_t *hook);

/*
 * hook_unprotect - Make a memory region writable
 *
 * Uses mprotect to set RWX on the page(s) containing [addr, addr+len).
 * Returns 0 on success, -1 on error.
 */
int hook_unprotect(uintptr_t addr, int len);

#endif /* HOOKS_H */
