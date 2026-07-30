/*
 * cod1reloaded.c - server-side ecosystem patches for cod_lnxded (CoD1 v1.5)
 *
 * RE'd against cod_lnxded md5 d3ac406b33acf2c9278a813d5f011b46 (ELF32 i386,
 * ImageBase 0x08048000). All VAs are absolute (the binary is non-PIE, loaded at
 * its preferred base) and live in .text/.rodata.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include "cod1reloaded.h"
#include "hooks.h"   /* hook_unprotect (mprotect RWX) */

#define TAG "[cod1reloaded]"

/* ---- engine functions (verified addresses, this binary) ---- */
typedef char *(*Cmd_Argv_t)(int n);                                   /* 0x080600F4 */
typedef char *(*Info_ValueForKey_t)(const char *info, const char *key); /* 0x08086397 */
typedef void  (*NET_OutOfBandPrint_t)(int netsrc, netadr_t adr,
                                      const char *fmt, ...);          /* 0x0808428E */

static Cmd_Argv_t           p_Cmd_Argv           = (Cmd_Argv_t)0x080600F4;
static Info_ValueForKey_t   p_Info_ValueForKey   = (Info_ValueForKey_t)0x08086397;
static NET_OutOfBandPrint_t p_NET_OutOfBandPrint = (NET_OutOfBandPrint_t)0x0808428E;

/* ---- patch sites (immediate byte 0x06 = protocol) ---- */
#define VA_PROTO_ACCEPT  0x08089EE6  /* SV_DirectConnect  cmpl $0x6  (REQUIRED: accept) */
#define VA_PROTO_CVAR    0x080913FF  /* "protocol" serverinfo cvar default (REQUIRED: advertise) */
#define VA_PROTO_REJMSG  0x08089F2C  /* reject "(should be %i)" value (cosmetic) */
#define VA_PROTO_STATUS  0x08092B30  /* SV_StatusString "protocol" value (cosmetic) */

#define VA_MASTER_STRING 0x080DFCEF  /* "codmaster.activision.com\0" */
#define MASTER_FIELD_LEN 25          /* 24 chars + NUL, in-place overwrite budget */

/* ---- snaps cap (SV_UserinfoChanged: client->snapshotMsec = 1000/clamp(snaps,1,30)) ---- */
#define VA_SNAPS_CAP_CMP 0x0808C6BF  /* cmp $0x1e (snaps > 30 ?)  30 -> N : real 40-tick */
#define VA_SNAPS_CAP_MOV 0x0808C6C5  /* mov $0x1e (snaps = 30)    30 -> N : patch both    */

/* ---- read-only tickrate diagnostic (engine globals, RE-verified) ---- */
#define VA_SVS_TIME    0x083CCD88  /* svs.time   (ms) */
#define VA_LEVEL_TIME  0x083CCD84  /* level.time (ms) */
#define VA_SVFPS_CVAR  0x0836B7D4  /* sv_fps cvar_t* ; ->integer @ +0x20 */

/* ---- config (env-overridable) ---- */
static int  g_protocol         = 10;
static int  g_min_version      = 16;  /* client "1.6" */
static int  g_allow_unversioned = 1;  /* allow cod1reloaded==0 (bots / loopback) */
static int  g_min_build        = 0;  /* 0 = build gate off; e.g. 10602 for 1.6.2 */
static char g_master[MASTER_FIELD_LEN] = "87.106.7.52";
static int  g_snaps_cap        = 40;  /* max snaps the server honors (real 40-tick, needs sv_fps 40); <=30 = vanilla */
static int  g_tickdiag         = 0;   /* COD1RELOADED_TICKDIAG=1 -> log level.time/svs.time rate (read-only) */

static void load_env(void) {
    const char *e;
    if ((e = getenv("COD1RELOADED_PROTOCOL"))    && *e) g_protocol    = atoi(e);
    if ((e = getenv("COD1RELOADED_MIN_VERSION")) && *e) g_min_version = atoi(e);
    if ((e = getenv("COD1RELOADED_ALLOW_UNVERSIONED")) && *e) g_allow_unversioned = atoi(e);
    if ((e = getenv("COD1RELOADED_MIN_BUILD")) && *e) g_min_build = atoi(e);
    if ((e = getenv("COD1RELOADED_MASTER"))      && *e) {
        strncpy(g_master, e, sizeof(g_master) - 1);
        g_master[sizeof(g_master) - 1] = 0;
    }
    if ((e = getenv("COD1RELOADED_SNAPS_CAP"))   && *e) g_snaps_cap = atoi(e);
    if ((e = getenv("COD1RELOADED_TICKDIAG"))    && *e) g_tickdiag  = atoi(e);
}

/* Patch one protocol immediate byte 0x06 -> g_protocol. */
static int patch_proto_byte(uintptr_t va) {
    unsigned char *p = (unsigned char *)va;
    unsigned char want = (unsigned char)g_protocol;
    if (*p == want) return 0;            /* already patched */
    if (*p != 0x06) {
        printf("%s proto byte @0x%08x = 0x%02x (expected 0x06), skip\n",
               TAG, (unsigned)va, *p);
        return -1;
    }
    if (hook_unprotect(va, 1) != 0) return -1;
    *p = want;
    return 0;
}

/* Raise the snaps cap 30 -> newcap for real 40-tick. Patch BOTH the cmp and the mov
 * immediate. Keep newcap <= 63 so the "cmp $imm8" stays positive (it is sign-extended). */
static int patch_snaps_cap(uintptr_t va, unsigned char newcap) {
    unsigned char *p = (unsigned char *)va;
    if (*p == newcap) return 0;          /* already patched */
    if (*p != 0x1e) {                    /* expect vanilla 30 */
        printf("%s snaps cap @0x%08x = 0x%02x (expected 0x1e), skip\n",
               TAG, (unsigned)va, *p);
        return -1;
    }
    if (hook_unprotect(va, 1) != 0) return -1;
    *p = newcap;
    return 0;
}

/* Read-only: once/sec, log how fast level.time & svs.time advance. At sv_fps 40 the
 * engine (RE-verified) should show ~1000 ms/s for both -> proves the game-clock bug is
 * in game.mp.i386.so, not the engine. Pure reads + printf, no engine mutation. */
static void *tickdiag_thread(void *arg) {
    (void)arg;
    int have = 0, last_svs = 0, last_lvl = 0;
    for (;;) {
        sleep(1);
        int svs = *(volatile int *)VA_SVS_TIME;
        int lvl = *(volatile int *)VA_LEVEL_TIME;
        int fps = 0;
        void *cv = *(void **)VA_SVFPS_CVAR;            /* sv_fps cvar_t* */
        if (cv) fps = *(int *)((char *)cv + 0x20);     /* ->integer */
        if (have)
            printf("%s tickdiag: sv_fps=%d  svs.time +%d ms/s  level.time +%d ms/s  (want ~1000)\n",
                   TAG, fps, svs - last_svs, lvl - last_lvl);
        last_svs = svs; last_lvl = lvl; have = 1;
        fflush(stdout);
    }
    return NULL;
}

void cod1reloaded_start_tickdiag(void) {
    if (!g_tickdiag) return;
    pthread_t tid;
    if (pthread_create(&tid, NULL, tickdiag_thread, NULL) == 0) {
        pthread_detach(tid);
        printf("%s tickdiag: started (reads level.time/svs.time each second)\n", TAG);
    }
}

void cod1reloaded_apply_patches(void) {
    load_env();

    int req = 0;
    req += (patch_proto_byte(VA_PROTO_ACCEPT) == 0);
    req += (patch_proto_byte(VA_PROTO_CVAR)   == 0);
    patch_proto_byte(VA_PROTO_REJMSG);  /* cosmetic */
    patch_proto_byte(VA_PROTO_STATUS);  /* cosmetic */
    printf("%s protocol -> %d (%d/2 required sites patched)\n", TAG, g_protocol, req);

    /* Master: overwrite "codmaster.activision.com" in place (the resolver reads
     * this copy). No ':' in g_master -> default UDP port 20510; append ":PORT"
     * to override. Heartbeat name is "COD-1" (dpmaster must accept it). */
    if (g_master[0]) {
        const unsigned char *cur = (const unsigned char *)VA_MASTER_STRING;
        if (cur[0] != 'c' || cur[1] != 'o' || cur[2] != 'd') {
            printf("%s master string @0x%08x unexpected (0x%02x%02x%02x), skip\n",
                   TAG, (unsigned)VA_MASTER_STRING, cur[0], cur[1], cur[2]);
        } else if (hook_unprotect(VA_MASTER_STRING, MASTER_FIELD_LEN) == 0) {
            char  *dst = (char *)VA_MASTER_STRING;
            size_t n = strlen(g_master);
            if (n >= MASTER_FIELD_LEN) n = MASTER_FIELD_LEN - 1;
            memset(dst, 0, MASTER_FIELD_LEN);
            memcpy(dst, g_master, n);
            printf("%s master -> \"%s\" (heartbeat COD-1, default port 20510)\n", TAG, g_master);
        }
    }

    /* Real 40-tick: raise the snaps cap so the server honors clients requesting snaps>30.
     * Requires sv_fps 40 in the server cfg; clients set snaps 40 via the competitive netconfig. */
    if (g_snaps_cap > 30) {
        if (g_snaps_cap > 63) g_snaps_cap = 63;   /* keep the cmp imm8 positive */
        unsigned char nc = (unsigned char)g_snaps_cap;
        int sn = 0;
        sn += (patch_snaps_cap(VA_SNAPS_CAP_CMP, nc) == 0);
        sn += (patch_snaps_cap(VA_SNAPS_CAP_MOV, nc) == 0);
        printf("%s snaps cap -> %d (%d/2 sites; set sv_fps 40)\n", TAG, g_snaps_cap, sn);
    } else {
        printf("%s snaps cap left vanilla (30)\n", TAG);
    }

    if (g_min_build > 0)
        printf("%s version gate: min cod1reloaded=%d, min build=%d.%d.%d, "
               "allow_unversioned=%d\n",
               TAG, g_min_version, g_min_build / 10000, (g_min_build / 100) % 100,
               g_min_build % 100, g_allow_unversioned);
    else
        printf("%s version gate: min cod1reloaded=%d, build gate OFF "
               "(set COD1RELOADED_MIN_BUILD, e.g. 10602), allow_unversioned=%d\n",
               TAG, g_min_version, g_allow_unversioned);
}

int cod1reloaded_allow_connect(netadr_t from) {
    char  buf[1024];
    char *uinfo = p_Cmd_Argv(1);          /* connect packet userinfo (already tokenized) */
    if (uinfo) {
        strncpy(buf, uinfo, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
    } else {
        buf[0] = 0;
    }

    int ver = atoi(p_Info_ValueForKey(buf, "cod1reloaded"));
    if (ver < g_min_version && !(ver == 0 && g_allow_unversioned)) {
        p_NET_OutOfBandPrint(NS_SERVER, from,
            "error\nUpdate cod1reloaded to v%d to join this server.", g_min_version);
        printf("%s rejected connect (cod1reloaded=%d < %d)\n", TAG, ver, g_min_version);
        return 0;
    }

    /* BUILD gate. "cod1reloaded" only says "this is a 1.6 client": it is the NET
     * version and never moves between releases, so on its own it lets a player sit on
     * an old build forever. "cod1x_build" is the actual release, encoded two digits per
     * component (1.6.2 -> 10602), so plain < ordering works and 1.6.2 < 1.6.10 < 1.7.0.
     *
     * OFF unless COD1RELOADED_MIN_BUILD is set. Arming it on release day would lock out
     * everyone who has not updated yet; set it once the build has been out long enough.
     * Clients older than this feature report nothing -> build 0, so they are refused
     * only when the gate is armed, and the message tells them what to do. */
    if (g_min_build > 0) {
        int build = atoi(p_Info_ValueForKey(buf, "cod1x_build"));
        if (build < g_min_build) {
            p_NET_OutOfBandPrint(NS_SERVER, from,
                "error\nThis server requires COD1.6X %d.%d.%d or newer.\n"
                "Launch the game once to auto-update.",
                g_min_build / 10000, (g_min_build / 100) % 100, g_min_build % 100);
            printf("%s rejected connect (cod1x_build=%d < %d)\n",
                   TAG, build, g_min_build);
            return 0;
        }
    }
    return 1;
}
