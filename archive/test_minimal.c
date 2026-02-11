#include <stdio.h>

static void test_init(void) __attribute__((constructor));

static void test_init(void)
{
    printf("[TEST] Minimal .so loaded successfully\n");
}
