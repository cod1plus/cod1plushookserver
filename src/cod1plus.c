/*
 * cod1plus.c
 * Stats collection for Call of Duty 1 v1.5
 * Uses BSS scan to dynamically locate svs.clients pointer
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <sys/types.h>

#define COD1PLUS_TAG    "[cod1plus]"
#define BACKEND_HOST    "localhost"
#define BACKEND_PORT    3005
#define STATS_PATH      "/api/stats"

/* BSS bounds for cod_lnxded (non-PIE, fixed addresses from /proc/maps) */
#define BSS_START       0x080f7000U
#define BSS_END         0x083e9000U

/* Discovered via BSS scan: BSS[0x083CCD90] -> svs.clients */
#define ADDR_SVS_CLIENTS_HINT   0x083CCD90U

#define MAX_CLIENTS             64
#define MAX_NETNAME             36
#define CLIENT_T_SIZE           371124
#define CLIENT_T_OFF_GENTITY    0x10A40
#define PLAYERSTATE_SIZE        0x22cc   /* size of ONE playerState_t copy */
#define POFF_SESSIONSTATE       (PLAYERSTATE_SIZE * 2)  /* gc has TWO ps copies; sess is at gc+0x4598 */

typedef enum {
    CS_FREE = 0,
    CS_ZOMBIE = 1,
    CS_CONNECTED = 2,
    CS_PRIMED = 3,
    CS_ACTIVE = 4
} clientState_t;

#define CLIENT_AT(base, i)  ((uintptr_t)(base) + (uintptr_t)CLIENT_T_SIZE * (i))

/* ---- SIGSEGV-safe memory reads ---- */
static __thread sigjmp_buf   g_jmpbuf;
static __thread volatile int g_in_safe = 0;
static struct sigaction      g_old_segv;

static void segv_handler(int sig, siginfo_t *si, void *ctx) {
    if (g_in_safe) { g_in_safe = 0; siglongjmp(g_jmpbuf, 1); }
    if (g_old_segv.sa_flags & SA_SIGINFO) g_old_segv.sa_sigaction(sig, si, ctx);
    else if (g_old_segv.sa_handler != SIG_DFL && g_old_segv.sa_handler != SIG_IGN)
        g_old_segv.sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}

static int safe_read32(uintptr_t addr, uint32_t *out) {
    g_in_safe = 1;
    if (sigsetjmp(g_jmpbuf, 1)) { g_in_safe = 0; return -1; }
    *out = *(volatile uint32_t *)addr;
    g_in_safe = 0;
    return 0;
}

static int safe_readstr(uintptr_t src, char *dst, size_t sz) {
    g_in_safe = 1;
    if (sigsetjmp(g_jmpbuf, 1)) { g_in_safe = 0; dst[0] = 0; return -1; }
    size_t i;
    for (i = 0; i + 1 < sz; i++) {
        char c = *(volatile char *)(src + i);
        dst[i] = c;
        if (!c) break;
    }
    dst[i] = 0;
    g_in_safe = 0;
    return 0;
}
/* ----------------------------------- */

/* Anonymous memory regions (> 10 MB, writable) */
#define MAX_ANON 8
typedef struct { uintptr_t lo, hi; } range_t;
static range_t g_anon[MAX_ANON];
static int     g_n_anon = 0;

static void load_anon_maps(void) {
    g_n_anon = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_n_anon < MAX_ANON) {
        unsigned long lo, hi;
        char perms[8], dev[8];
        unsigned long inode;
        if (sscanf(line, "%lx-%lx %4s %*x %5s %lu", &lo, &hi, perms, dev, &inode) == 5) {
            /* Anonymous = inode 0, not a named file */
            (void)dev;
            if (inode == 0 &&
                perms[0] == 'r' && perms[1] == 'w' &&
                (hi - lo) > 10 * 1024 * 1024) {
                g_anon[g_n_anon].lo = (uintptr_t)lo;
                g_anon[g_n_anon].hi = (uintptr_t)hi;
                g_n_anon++;
            }
        }
    }
    fclose(f);
}

static int in_anon(uintptr_t v) {
    for (int i = 0; i < g_n_anon; i++)
        if (v >= g_anon[i].lo && v < g_anon[i].hi) return 1;
    return 0;
}

/*
 * Scan BSS for svs.clients:
 *   - Read entire BSS via /proc/self/mem (fast, no SIGSEGV risk)
 *   - For each 4-byte word that points into a large anon region,
 *     check if dereferencing it gives a valid client state (CS_CONNECTED+)
 * Returns the BSS address that holds svs.clients, or 0.
 */
static uintptr_t find_svs_clients(void) {
    load_anon_maps();
    printf("%s Scan: %d large anon region(s) found:\n", COD1PLUS_TAG, g_n_anon);
    for (int i = 0; i < g_n_anon; i++)
        printf("%s   [0x%08X - 0x%08X] (%u MB)\n", COD1PLUS_TAG,
            (unsigned)g_anon[i].lo, (unsigned)g_anon[i].hi,
            (unsigned)((g_anon[i].hi - g_anon[i].lo) >> 20));

    size_t bss_size = BSS_END - BSS_START;
    uint8_t *buf = malloc(bss_size);
    if (!buf) { printf("%s malloc failed\n", COD1PLUS_TAG); return 0; }

    int memfd = open("/proc/self/mem", O_RDONLY);
    ssize_t r = (memfd >= 0) ? pread(memfd, buf, bss_size, (off_t)BSS_START) : -1;
    if (memfd >= 0) close(memfd);

    if (r != (ssize_t)bss_size) {
        printf("%s Failed to read BSS (r=%zd)\n", COD1PLUS_TAG, r);
        free(buf);
        return 0;
    }

    uintptr_t result = 0;
    uint32_t *words = (uint32_t *)buf;
    size_t n = bss_size / 4;

    for (size_t i = 0; i < n; i++) {
        uint32_t v = words[i];
        if (!in_anon(v)) continue;
        /* Reject non-16-byte-aligned pointers (hunk alloc is 16-byte aligned) */
        if (v & 0xF) continue;

        /* Try to read as client state */
        uint32_t state0 = 0xFF;
        if (safe_read32((uintptr_t)v, &state0) < 0) continue;
        if (state0 < CS_CONNECTED || state0 > CS_ACTIVE) continue;

        uintptr_t bss_addr = BSS_START + i * 4;
        printf("%s CANDIDATE: BSS[0x%08X] -> 0x%08X  state[0]=%d\n",
            COD1PLUS_TAG, (unsigned)bss_addr, v, (int)state0);
        /* Prefer CS_ACTIVE (4) over earlier states */
        if (!result || state0 == CS_ACTIVE) result = bss_addr;
    }

    free(buf);
    if (!result)
        printf("%s No candidates found (client not yet in CS_CONNECTED+ ?)\n", COD1PLUS_TAG);
    return result;
}

/* Periodic gc diagnostic scan (runs every 30s while a player is CS_ACTIVE) */

static void scan_gc_data(uintptr_t gc) {
    printf("%s === gc=0x%08X scan ===\n", COD1PLUS_TAG, (unsigned)gc);

    /* 1. Complete dump of gc[0x1F00..0x2500]: region around netname (found at gc+0x2128)
     *    clientPersistant_t starts somewhere here; kills/deaths should be nearby */
    printf("%s Complete dump gc[0x1F00..0x2500] (around netname at gc+0x2128):\n", COD1PLUS_TAG);
    for (uint32_t off = 0x1F00; off < 0x2500; off += 4) {
        uint32_t v = 0;
        safe_read32(gc + off, &v);
        if (v != 0)
            printf("%s   gc+0x%04X = %d (0x%08X)  NON-ZERO\n", COD1PLUS_TAG, off, (int)v, v);
        else
            printf("%s   gc+0x%04X = 0\n", COD1PLUS_TAG, off);
    }

    /* 2. Scan gc[0x1000..0x4400] for value=4 (expected deaths after 4 suicides) */
    printf("%s Scanning gc[0x1000..0x4400] for value=4 (expected deaths):\n", COD1PLUS_TAG);
    for (uint32_t off = 0x1000; off < 0x4400; off += 4) {
        uint32_t v = 0;
        if (safe_read32(gc + off, &v) < 0) continue;
        if (v == 4)
            printf("%s   gc+0x%04X = 4  <-- CANDIDATE DEATHS\n", COD1PLUS_TAG, off);
    }

    /* 3. All non-zero values in gc[0x22CC..0x4400] (after second ps copy) */
    printf("%s All non-zero in gc[0x22CC..0x4400] (after ps copies):\n", COD1PLUS_TAG);
    for (uint32_t off = 0x22CC; off < 0x4400; off += 4) {
        uint32_t v = 0;
        if (safe_read32(gc + off, &v) < 0) continue;
        if (v != 0)
            printf("%s   gc+0x%04X = %d (0x%08X)\n", COD1PLUS_TAG, off, (int)v, v);
    }

    printf("%s === end scan ===\n", COD1PLUS_TAG);
}

/* Runtime-discovered address of svs.clients in BSS */
static uintptr_t g_addr_svs_clients = ADDR_SVS_CLIENTS_HINT;
static int       g_scan_done = 0;
static uint32_t  g_loop_tick = 0;    /* incremented each 5-second loop */
static uint32_t  g_gc_scan_tick = 0; /* g_loop_tick when last gc scan ran */

/* Simple HTTP POST */
static int http_post(const char *data) {
    struct hostent *srv = gethostbyname(BACKEND_HOST);
    if (!srv) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(BACKEND_PORT);
    memcpy(&addr.sin_addr.s_addr, srv->h_addr, srv->h_length);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(sock); return -1; }

    char req[8192];
    snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\nHost: %s:%d\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        STATS_PATH, BACKEND_HOST, BACKEND_PORT, strlen(data), data);
    send(sock, req, strlen(req), 0);
    close(sock);
    return 0;
}

static void json_escape(const char *src, char *dst, size_t sz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
        else if (c == '\n')        { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c >= 32 && c < 127) dst[j++] = c;
    }
    dst[j] = 0;
}

static void *stats_loop(void *arg) {
    (void)arg;
    printf("%s Stats thread started, waiting 30s...\n", COD1PLUS_TAG);
    sleep(30);
    printf("%s Starting stats collection\n", COD1PLUS_TAG);

    while (1) {
        sleep(5);
        g_loop_tick++;

        /* Refresh anon regions every tick - they grow as maps load */
        load_anon_maps();

        /* Step 1: read svs.clients pointer */
        uint32_t clients_raw = 0;
        safe_read32(g_addr_svs_clients, &clients_raw);

        /* Reset scan flags if pointer is null (server restart / map change) */
        if (!clients_raw) { g_scan_done = 0; g_gc_scan_tick = 0; continue; }

        /* If pointer not in known regions, try a BSS scan */
        if (!in_anon(clients_raw) && !g_scan_done) {
            printf("%s 0x%08X is not in any anon region - scanning BSS...\n",
                COD1PLUS_TAG, clients_raw);
            g_scan_done = 1;
            uintptr_t found = find_svs_clients();
            if (found) {
                g_addr_svs_clients = found;
                safe_read32(g_addr_svs_clients, &clients_raw);
                printf("%s Using svs.clients @ BSS[0x%08X] = 0x%08X\n",
                    COD1PLUS_TAG, (unsigned)found, clients_raw);
            }
        }

        if (!clients_raw || !in_anon(clients_raw)) continue;

        /* Step 2: iterate client slots */
        char json[8192];
        int pos = snprintf(json, sizeof(json), "{\"players\":[");
        int count = 0;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            uintptr_t slot = CLIENT_AT(clients_raw, i);

            uint32_t state_v = 0;
            if (safe_read32(slot, &state_v) < 0) continue;
            /* Only accept valid states: CS_CONNECTED(2), CS_PRIMED(3), CS_ACTIVE(4) */
            if (state_v < CS_CONNECTED || state_v > CS_ACTIVE) continue;

            /* gentity pointer - must be in anon region to be valid */
            uint32_t gent = 0;
            int gent_ret = safe_read32(slot + CLIENT_T_OFF_GENTITY, &gent);
            if (gent_ret < 0 || !gent || !in_anon((uintptr_t)gent)) continue;

            /* gclient pointer at gentity+0x15C (discovered via scan) */
            uint32_t gc = 0;
            int gc_ret = safe_read32((uintptr_t)gent + 0x15C, &gc);

            /* Debug: gc scan every ~60s while CS_ACTIVE (suicide first, then wait for output) */
            if (i == 0 && state_v == CS_ACTIVE &&
                (!g_gc_scan_tick || (g_loop_tick - g_gc_scan_tick) >= 6)) {
                g_gc_scan_tick = g_loop_tick;
                printf("%s slot[0] state=%d gent=0x%08X gc=0x%08X (tick=%u)\n",
                    COD1PLUS_TAG, (int)state_v, gent, gc, g_loop_tick);
                scan_gc_data((uintptr_t)gc);

                /* Scan client_t (slot) for name - it's stored here, not in gclient */
                printf("%s client_t strings (slot=0x%08X, first 0x1400 bytes):\n",
                    COD1PLUS_TAG, (unsigned)slot);
                for (uint32_t soff = 0; soff < 0x1400; soff++) {
                    char sbuf[64] = {0};
                    if (safe_readstr(slot + soff, sbuf, sizeof(sbuf)) < 0) continue;
                    int slen = 0;
                    for (int k = 0; sbuf[k]; k++) {
                        unsigned char sc = (unsigned char)sbuf[k];
                        if (sc >= 0x20 && sc < 0x7F) slen++;
                        else break;
                    }
                    if (slen >= 3) {
                        printf("%s   slot+0x%04X: '%s'\n", COD1PLUS_TAG, soff, sbuf);
                        soff += (uint32_t)(slen > 1 ? slen - 1 : 0);
                    }
                }
            }

            if (gc_ret < 0 || !gc || !in_anon((uintptr_t)gc)) continue;

            /* Confirmed offsets (CoD1 v1.5 gclient_t, FFA/DM):
             *   gc+0x20DC = score/frags (net: +1 per kill, -1 per suicide)
             *   gc+0x20E0 = deaths (total deaths including suicides) */
            uint32_t kills = 0, deaths = 0;
            safe_read32((uintptr_t)gc + 0x20DC, &kills);
            safe_read32((uintptr_t)gc + 0x20E0, &deaths);

            /* Read name from client_t userinfo (\name\VALUE\ at slot+0x000C) */
            char raw[MAX_NETNAME * 2] = {0};
            {
                char info[512] = {0};
                safe_readstr(slot + 0x000C, info, sizeof(info));
                char *p = strstr(info, "\\name\\");
                if (p) {
                    p += 6;
                    size_t ni = 0;
                    while (p[ni] && p[ni] != '\\' && ni + 1 < sizeof(raw)) {
                        raw[ni] = p[ni];
                        ni++;
                    }
                    raw[ni] = 0;
                }
            }

            char name[128] = {0};
            json_escape(raw, name, sizeof(name));

            char buf[256];
            int n = snprintf(buf, sizeof(buf),
                "%s{\"id\":%d,\"name\":\"%s\",\"kills\":%d,\"deaths\":%d,\"state\":%d}",
                count ? "," : "", i, name, (int)kills, (int)deaths, (int)state_v);
            if (pos + n < (int)sizeof(json) - 8) {
                memcpy(json + pos, buf, n);
                pos += n;
            }
            count++;
        }

        snprintf(json + pos, sizeof(json) - pos, "]}");
        printf("%s %d player(s): %s\n", COD1PLUS_TAG, count, json);
        if (count > 0) http_post(json);
    }
    return NULL;
}

static void __attribute__((constructor)) init(void) {
    printf("%s Loaded\n", COD1PLUS_TAG);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);

    pthread_t tid;
    if (pthread_create(&tid, NULL, stats_loop, NULL) == 0) {
        pthread_detach(tid);
        printf("%s Stats thread started\n", COD1PLUS_TAG);
    }
}

static void __attribute__((destructor)) fini(void) {
    sigaction(SIGSEGV, &g_old_segv, NULL);
    printf("%s Unloaded\n", COD1PLUS_TAG);
}
