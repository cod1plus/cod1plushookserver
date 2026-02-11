/*
 * hooks.c - x86 function hooking mechanism
 *
 * Implements JMP detour hooking with trampoline for 32-bit x86 Linux.
 */

#include "hooks.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

#define JMP_OPCODE      0xE9
#define JMP_SIZE        5       /* 1 byte opcode + 4 bytes relative address */
#define MIN_PATCH_LEN   JMP_SIZE

/* Calculate relative JMP offset: from -> to, accounting for the 5-byte JMP instruction */
static inline int32_t calc_rel_jmp(uintptr_t from, uintptr_t to)
{
    return (int32_t)(to - from - JMP_SIZE);
}

/* Align address down to page boundary */
static inline uintptr_t page_align(uintptr_t addr)
{
    long page_size = sysconf(_SC_PAGESIZE);
    return addr & ~(page_size - 1);
}

int hook_unprotect(uintptr_t addr, int len)
{
    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t page_start = page_align(addr);
    uintptr_t page_end = page_align(addr + len - 1) + page_size;
    size_t region_size = page_end - page_start;

    if (mprotect((void *)page_start, region_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("[cod1plus] mprotect failed");
        return -1;
    }
    return 0;
}

int hook_install(hook_t *hook, uintptr_t target, uintptr_t replacement, int patch_len)
{
    if (!hook) return -1;

    memset(hook, 0, sizeof(*hook));

    if (patch_len < MIN_PATCH_LEN)
        patch_len = MIN_PATCH_LEN;

    hook->target_addr = target;
    hook->hook_addr = replacement;
    hook->patch_len = patch_len;

    /* Save original bytes */
    memcpy(hook->original_bytes, (void *)target, patch_len);

    /* Allocate trampoline: stolen bytes + JMP back to original function */
    size_t tramp_size = patch_len + JMP_SIZE;
    void *tramp = mmap(NULL, tramp_size,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        perror("[cod1plus] mmap trampoline failed");
        return -1;
    }

    /* Copy the original (stolen) bytes into the trampoline */
    memcpy(tramp, (void *)target, patch_len);

    /* Append a JMP back to target + patch_len (the rest of the original function) */
    uint8_t *tramp_jmp = (uint8_t *)tramp + patch_len;
    tramp_jmp[0] = JMP_OPCODE;
    *(int32_t *)(tramp_jmp + 1) = calc_rel_jmp((uintptr_t)tramp_jmp, target + patch_len);

    hook->trampoline = (uintptr_t)tramp;

    /* Make target memory writable */
    if (hook_unprotect(target, patch_len) != 0) {
        munmap(tramp, tramp_size);
        return -1;
    }

    /* Write JMP to our hook function at the target address */
    uint8_t *target_ptr = (uint8_t *)target;
    target_ptr[0] = JMP_OPCODE;
    *(int32_t *)(target_ptr + 1) = calc_rel_jmp(target, replacement);

    /* NOP any remaining bytes after the JMP (for cleanliness) */
    for (int i = JMP_SIZE; i < patch_len; i++) {
        target_ptr[i] = 0x90; /* NOP */
    }

    hook->active = 1;

    printf("[cod1plus] Hook installed: 0x%08x -> 0x%08x (trampoline at %p)\n",
           (unsigned)target, (unsigned)replacement, tramp);

    return 0;
}

int hook_remove(hook_t *hook)
{
    if (!hook || !hook->active)
        return -1;

    /* Make target writable again (in case protections were restored) */
    if (hook_unprotect(hook->target_addr, hook->patch_len) != 0)
        return -1;

    /* Restore original bytes */
    memcpy((void *)hook->target_addr, hook->original_bytes, hook->patch_len);

    /* Free trampoline */
    size_t tramp_size = hook->patch_len + JMP_SIZE;
    munmap((void *)hook->trampoline, tramp_size);

    hook->trampoline = 0;
    hook->active = 0;

    printf("[cod1plus] Hook removed: 0x%08x\n", (unsigned)hook->target_addr);

    return 0;
}
