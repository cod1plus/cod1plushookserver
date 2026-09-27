/*
 * test_crouch_right_engine.c - proves the v3 crouch-right bend (lean_controllers.c,
 * LC_CROUCH_RIGHT) against the REAL server controller function, not against a model of it.
 *
 *   gcc -m32 -O1 -o /tmp/tcr test/test_crouch_right_engine.c -ldl -lm
 *   /tmp/tcr <path to game.mp.i386.so md5 343f99cd67b79ac74aeaa5261f63c011>
 *
 * game.mp.i386.so is dlopen'ed and BG_Player_DoControllersInternal (+0x1a389) called
 * directly: it is pure (reads es / ci, writes the buffer - see pose_sync.c). For 20000
 * random poses, stand and crouch, both lean sides:
 *   A = engine as shipped (crouch-right multiplier 1.5 at .rodata 0x73d9c)
 *   B = engine with that constant patched to 1.25
 * and the check is:   lc_apply_v3(A)  ==  v2 arithmetic (top-up + reprojection) on B
 * over the whole 24-float buffer, i.e. "v3 on the stock engine is the engine with 1.25",
 * plus, for every pose that is not crouch + lean right:   lc_apply_v3(A) == v2 on A.
 * The blend is off here (it is a separate stage with its own test).
 */
#define _GNU_SOURCE
#include "../src/shared/lean_controllers.c"

#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>

typedef void (*dci_t)(void* obj, void* es, int* partBits, void* ci, float* out);

static unsigned g_rnd = 12345;
static float frand(float a, float b)
{
    g_rnd = g_rnd * 1103515245u + 12345u;
    return a + (b - a) * (float)((g_rnd >> 8) & 0xffff) / 65535.0f;
}

/* v2 = the very same lc_apply with no entityState: the v3 block is the only code that
 * reads es when the blend is off, so this IS v2's arithmetic, compiled the same way. */
static void v2_apply(float* b, const lc_ctx_t* c)
{
    lc_ctx_t c2 = *c;
    c2.es = NULL;
    lc_apply(b, &c2);
}

int main(int argc, char** argv)
{
    void* h = dlopen(argc > 1 ? argv[1] : "./game.mp.i386.so", RTLD_NOW | RTLD_LOCAL);
    uintptr_t base;
    dci_t dci;
    float* k15;
    unsigned char es[0x100], ci[0x4b0];
    int pb[4] = { -1, -1, -1, -1 };
    int trial, i, n_cr = 0, n_other = 0;
    double worst_cr = 0, worst_other = 0, head_roll_cut = 0;

    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
    base = (uintptr_t)dlsym(h, "BG_Player_DoControllers") - 0x1ad71;
    dci  = (dci_t)(base + 0x1a389);
    k15  = (float*)(base + 0x73d9c);
    if (k15[0] != LC_ENGINE_CROUCH_RIGHT || *(float*)(base + 0x73da0) != 1.25f ||
        *(float*)(base + 0x73d90) != 0.8f) {
        printf("not the expected build (0x73d9c = %f)\n", k15[0]);
        return 2;
    }
    if (mprotect((void*)((uintptr_t)k15 & ~0xfffu), 0x2000, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 2;
    }

    for (trial = 0; trial < 20000; ++trial) {
        float a[LC_FLOATS], b[LC_FLOATS], a_v2[LC_FLOATS];
        const int crouch = trial & 1;
        const float lean = frand(-0.5f, 0.5f);
        int ef = crouch ? LC_EF_CROUCH : 0, cn = 3;
        const float engine_lateral = (trial & 2) ? 7.5f : 2.5f;   /* server / client */
        lc_ctx_t c;
        float v;

        memset(es, 0, sizeof(es));
        memset(ci, 0, sizeof(ci));
        memcpy(es + 0x08, &ef, 4);
        memcpy(es + 0x90, &cn, 4);
        v = frand(-180, 180); memcpy(ci + 0x380, &v, 4);
        v = frand(-180, 180); memcpy(ci + 0x3b0, &v, 4);
        v = frand(-60, 60);   memcpy(ci + 0x3b8, &v, 4);
        v = frand(-90, 90);   memcpy(ci + 0x3e0, &v, 4);
        memcpy(ci + 0x3e4, &lean, 4);
        v = frand(-70, 70);   memcpy(ci + 0x3e8, &v, 4);
        v = frand(-180, 180); memcpy(ci + 0x3ec, &v, 4);
        v = frand(-60, 60);   memcpy(es + 0xe8, &v, 4);
        v = frand(-60, 60);   memcpy(es + 0xec, &v, 4);
        v = frand(-10, 10);   memcpy(es + 0xe4, &v, 4);

        k15[0] = 1.5f;  memset(a, 0, sizeof(a)); dci(es, es, pb, ci, a);
        k15[0] = 1.25f; memset(b, 0, sizeof(b)); dci(es, es, pb, ci, b);
        k15[0] = 1.5f;
        memcpy(a_v2, a, sizeof(a));

        memset(&c, 0, sizeof(c));
        c.side = LC_SIDE_SERVER;
        c.client_num = cn;
        c.stance = crouch ? LC_STANCE_CROUCH : LC_STANCE_STAND;
        c.engine_lateral = engine_lateral;
        c.es = es;
        lc_apply(a, &c);                 /* v3 on the stock engine */
        v2_apply(b, &c);                 /* v2 on the engine with 1.25 */
        v2_apply(a_v2, &c);              /* v2 on the stock engine */

        if (crouch && lean > 0.0f) {
            ++n_cr;
            for (i = 0; i < LC_FLOATS; ++i)
                if (fabs((double)a[i] - b[i]) > worst_cr) worst_cr = fabs((double)a[i] - b[i]);
            if (fabs((double)a_v2[LC_HEAD + 2] - a[LC_HEAD + 2]) > head_roll_cut)
                head_roll_cut = fabs((double)a_v2[LC_HEAD + 2] - a[LC_HEAD + 2]);
        } else {
            ++n_other;
            for (i = 0; i < LC_FLOATS; ++i)
                if (fabs((double)a[i] - a_v2[i]) > worst_other) worst_other = fabs((double)a[i] - a_v2[i]);
        }
    }

    printf("crouch + lean right: %d poses, max |v3(stock) - v2(engine at 1.25)| over the 24 floats = %g\n",
           n_cr, worst_cr);
    printf("  (largest head-roll change v2 -> v3 in those poses: %.2f deg)\n", head_roll_cut);
    printf("everything else: %d poses, max |v3 - v2| = %g\n", n_other, worst_other);
    printf("banner: %s\n", lc_banner());
    if (worst_cr < 1e-4 && worst_other == 0.0 && n_cr > 1000 && head_roll_cut > 1.0) {
        printf("ALL OK\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
