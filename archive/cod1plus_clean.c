#include <stdio.h>

#define COD1PLUS_TAG "[cod1plus]"

static void cod1plus_init(void) __attribute__((constructor));
static void cod1plus_shutdown(void) __attribute__((destructor));

static void cod1plus_init(void)
{
    printf("%s Library loaded successfully (clean version)\n", COD1PLUS_TAG);
}

static void cod1plus_shutdown(void)
{
    printf("%s Library unloaded\n", COD1PLUS_TAG);
}
