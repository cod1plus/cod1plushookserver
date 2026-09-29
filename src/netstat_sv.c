/*
 * netstat_sv.c - is the rate limit actually holding snapshots back? (COD1RELOADED_NETSTAT=N)
 *
 * The server decides, after every snapshot it sends, when it may send the next one:
 *   SV_SendMessageToClient (cod_lnxded 0x8097a..): rateMsec = SV_RateMsec(client, size)
 *   = size * 1000 / min(rate, sv_maxRate); if rateMsec < snapshotMsec (= 1000/snaps) the
 *   snaps setting wins and rateDelayed = 0, else the RATE wins and rateDelayed = 1;
 *   nextSnapshotTime = svs.time + rateMsec.
 * So "rateDelayed" is the exact answer to "did the byte budget, not sv_fps/snaps, decide
 * the interval" - and the client's last 32 messages keep their size and send time. This
 * reads those fields every game frame (nothing is written) and prints, per real client
 * (NA_IP - bots and loopback are never rate limited), every N seconds:
 *   snapshots sent, how many were rate-delayed, message size avg/max, the longest wait
 *   the rate imposed against the snaps budget.
 *
 * All addresses: cod_lnxded md5 d3ac406b33acf2c9278a813d5f011b46 (the 1.5 Linux server),
 * read out of SV_UserinfoChanged (0x808c5c4 rate, 0x808c6dd snapshotMsec) and
 * SV_SendMessageToClient (0x8097aaa..0x8097be1). Fixed VAs: the executable is not PIE.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "netstat_sv.h"

#define TAG "[netstat]"

#define VA_SVS_CLIENTS   0x083ccd90   /* client_t* svs.clients (SV_AddTestClient 0x808d096) */
#define VA_SVS_TIME      0x083ccd88   /* int svs.time (0x8097ae6) */
#define VA_SV_MAXCLIENTS 0x0836b804   /* cvar_t* sv_maxclients, integer at +0x20 (0x808d0a1) */
#define VA_SV_MAXRATE    0x083ccd4c   /* cvar_t* sv_maxRate, integer at +0x20 (0x80979cd) */
#define CLIENT_SIZE      0x5a9b4      /* stride (0x808d0cf) */
#define C_STATE          0x00000      /* 4 = CS_ACTIVE */
#define C_NEXT_SNAP      0x10c24      /* nextSnapshotTime */
#define C_RATE_DELAYED   0x10c28
#define C_RATE           0x52934
#define C_SNAPSHOT_MSEC  0x52938
#define C_ADDR_TYPE      0x52948      /* netchan.remoteAddress.type: 4 = NA_IP */
#define C_OUT_SEQ        0x52964      /* netchan.outgoingSequence */
#define C_FRAMES         0x12d0c      /* frames[seq & 31]: messageSent +0, messageAcked +4, messageSize +8 */
#define FRAME_SIZE       0x20e8
#define MAX_CL           64

typedef struct {
    unsigned last_seq;
    int      sends, delayed, size_sum, size_max, wait_max, budget;
    int      valid;
} ns_t;

static ns_t g_ns[MAX_CL];
static int  g_period = -1;          /* seconds, 0 = off */
static int  g_last_print = 0;

static int cvar_int(uintptr_t va) {
    const char* cv = *(const char**)va;
    return cv ? *(const int*)(cv + 0x20) : 0;
}

int netstat_is_bot(int cn) {
    const char* clients = *(const char**)VA_SVS_CLIENTS;
    if (!clients || cn < 0 || cn >= MAX_CL) return 0;
    return *(const int*)(clients + (size_t)cn * CLIENT_SIZE + C_ADDR_TYPE) == 0;  /* NA_BOT */
}

void netstat_frame(void) {
    const char* clients;
    int i, n, now, maxrate;

    if (g_period < 0) {
        const char* e = getenv("COD1RELOADED_NETSTAT");
        g_period = (e && *e) ? atoi(e) : 0;
        if (g_period > 0) {
            printf("%s on: per-client snapshot / rate-limit report every %d s\n", TAG, g_period);
            fflush(stdout);
        }
    }
    if (g_period <= 0) return;

    clients = *(const char**)VA_SVS_CLIENTS;
    if (!clients) return;
    n = cvar_int(VA_SV_MAXCLIENTS);
    if (n < 1 || n > MAX_CL) n = MAX_CL;
    now = *(const int*)VA_SVS_TIME;
    maxrate = cvar_int(VA_SV_MAXRATE);

    for (i = 0; i < n; ++i) {
        const char* c = clients + (size_t)i * CLIENT_SIZE;
        ns_t* s = &g_ns[i];
        unsigned seq;
        if (*(const int*)(c + C_STATE) != 4 || *(const int*)(c + C_ADDR_TYPE) != 4) {
            s->valid = 0;
            continue;
        }
        seq = *(const unsigned*)(c + C_OUT_SEQ);
        if (!s->valid) {                      /* first sight: baseline, no count */
            memset(s, 0, sizeof(*s));
            s->valid = 1;
            s->last_seq = seq;
            continue;
        }
        if (seq != s->last_seq) {            /* a message went out since the last frame */
            const char* f = c + C_FRAMES + (size_t)((seq - 1) & 31) * FRAME_SIZE;
            const int size = *(const int*)(f + 8);
            const int wait = *(const int*)(c + C_NEXT_SNAP) - *(const int*)(f + 0);
            s->last_seq = seq;
            s->sends++;
            if (*(const int*)(c + C_RATE_DELAYED)) s->delayed++;
            s->size_sum += size;
            if (size > s->size_max) s->size_max = size;
            if (wait > s->wait_max) s->wait_max = wait;
            s->budget = *(const int*)(c + C_SNAPSHOT_MSEC);
        }
    }

    if (now - g_last_print >= g_period * 1000 || now < g_last_print) {
        g_last_print = now;
        for (i = 0; i < n; ++i) {
            const char* c = clients + (size_t)i * CLIENT_SIZE;
            ns_t* s = &g_ns[i];
            int rate, eff;
            if (!s->valid || !s->sends) continue;
            rate = *(const int*)(c + C_RATE);
            eff = (maxrate > 0 && maxrate < rate) ? maxrate : rate;
            printf("%s cn=%d rate %d (sv_maxRate %d -> %d B/s, %d B per %d ms snapshot): %ds: "
                   "%d snapshots, %d rate-delayed (%d%%), size avg %d max %d B, longest wait %d ms\n",
                   TAG, i, rate, maxrate, eff, eff * s->budget / 1000, s->budget, g_period,
                   s->sends, s->delayed, 100 * s->delayed / s->sends,
                   s->size_sum / s->sends, s->size_max, s->wait_max);
            s->sends = s->delayed = s->size_sum = s->size_max = s->wait_max = 0;
        }
        fflush(stdout);
    }
}

/* Test clients never move, yet their entity state carries a velocity (150 u/s along the
 * view, from the fake usercmd pmove) and the client draws every player at
 * pos + velocity * dt (TR_LINEAR_STOP extrapolation): a bot with that ghost velocity is
 * DRAWN 7.5-15 u ahead of where the server tests it - "je vise, ca traverse". Zero it every
 * frame: ps.velocity (gclient + 0x20) and es.pos.trDelta (ent + 0x24), so the drawn bot
 * is the tested bot. Real players are not touched. */
#define RVA_G_ENTITIES_NS 0x21d6c0
#define GENTITY_SIZE_NS   0x31c
void netstat_bots_freeze(uintptr_t game_base) {
    int i;
    if (!game_base) return;
    for (i = 0; i < MAX_CL; ++i) {
        char* ge = (char*)(game_base + RVA_G_ENTITIES_NS) + (size_t)i * GENTITY_SIZE_NS;
        char* cl;
        if (*(const int*)(ge + 0x04) != 1 || *(const int*)(ge + 0x90) != i) continue;
        cl = *(char**)(ge + 0x15c);
        if (!cl || !netstat_is_bot(i)) continue;
        memset(cl + 0x20, 0, 12);      /* ps.velocity */
        memset(ge + 0x24, 0, 12);      /* es.pos.trDelta */
    }
}
