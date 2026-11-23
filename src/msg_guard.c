/*
 * msg_guard.c - one client's oversized message drops THAT client, not the whole server.
 *
 * SV_SendMessageToClient(msg_t *msg, client_t *cl) (cod_lnxded 0x8097a2f) copies the 4-byte
 * header of the message into a 16 KB stack buffer (lea -0x4018(%ebp)), then Huffman-codes the
 * rest into it (0x807fc1b, static tree 0x8153d20) with NO bound check. The message itself is
 * at most 16 KB (SV_SendClientSnapshot: MSG_Init(&msg, buf, 0x4000)), but the static tree
 * codes many bytes on more than 8 bits: a full message can come out bigger than it went in,
 * and the extra bytes overwrite the frame -> signal 11 at 0x8097a8c (the client pointer read
 * back from the smashed frame).
 *
 * How a message gets that big: a client that stops acknowledging (game killed or crashed, PC
 * frozen) is sent EVERY unacknowledged reliable command again in each message. The PAM's
 * streamer feed (setClientCvar of 700-900 characters, several times a second) fills 16 KB in
 * a few seconds. Seen 2026-11-23 15:22 on the Docker test server: the streamer's game killed,
 * its messages 6.5 -> 10 -> 13.5 KB (14.8 KB max) over six seconds, then the crash.
 *
 * The guard: for a message over 8 KB, code it into a scratch buffer first (the coder only reads
 * the static tree: no state changes) and, if the result would not fit:
 *  - from SV_SendClientSnapshot (returns to 0x8097e1a): drop the client with "Server command
 *    overflow" (what the engine says when 64 commands are pending) and send nothing - the same
 *    as the engine's own overflow path there (SV_DropClient at 0x8097e03), one step earlier;
 *  - from anywhere else (SV_SendClientGameState, 0x808b0f9): send nothing, drop nobody. That
 *    caller goes on to make the client active (CS_PRIMED -> CS_ACTIVE): a client dropped there
 *    is promoted as a zombie and the next script touching it ends in signal 11 (seen with a
 *    3000-byte test limit: the 12 KB gamestate dropped a bot). The client waits for a gamestate
 *    that never comes and times out - instead of the server going down.
 * Messages under 8 KB - nearly all of them - pay one comparison. A gamestate codes to ~97 % of
 * its size (12358 -> 11929 bytes on mp_liberation): the static tree barely compresses.
 *
 * Addresses: cod_lnxded md5 d3ac406b33acf2c9278a813d5f011b46 (1.5 Linux server). Fixed VAs,
 * the executable is not PIE.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "hooks.h"
#include "msg_guard.h"

#define TAG "[msg_guard]"

#define VA_SEND_MSG      0x08097a2f   /* void SV_SendMessageToClient(msg_t*, client_t*) */
#define RET_SNAPSHOT     0x08097e1a   /* return address of its call in SV_SendClientSnapshot */
#define VA_HUFF_CODE     0x0807fc1b   /* int (const byte* in, byte* out, int len): bytes written */
#define VA_DROPCLIENT    0x0808ac11   /* void SV_DropClient(client_t*, const char* reason) */
#define VA_SVS_CLIENTS   0x083ccd90   /* client_t* svs.clients */
#define CLIENT_SIZE      0x5a9b4
#define MSG_DATA         0x04         /* byte* data */
#define MSG_CURSIZE      0x0c         /* int cursize */

#define CHECK_FROM       8192         /* smaller messages cannot outgrow the buffer */
#define OUT_LIMIT        16000        /* coded bytes + 4-byte header must stay under 16 KB */
/* COD1RELOADED_MSG_GUARD_LIMIT=<bytes> lowers the limit to test the drop path (dev only) */
static int g_limit = OUT_LIMIT, g_check_from = CHECK_FROM;
#define SCRATCH_SIZE     (256 * 1024) /* worst case: every byte on 4 bytes, 64 KB in */

typedef void (*send_msg_t)(void* msg, void* cl);
typedef int  (*huff_code_t)(const unsigned char* in, unsigned char* out, int len);
typedef void (*drop_client_t)(void* cl, const char* reason);

static hook_t     h_send;
static send_msg_t o_send;
static unsigned char g_scratch[SCRATCH_SIZE];
static void*      g_dropping;          /* SV_DropClient may come back here for it: send nothing */

static int client_num(const void* cl) {
    const char* clients = *(const char* const*)VA_SVS_CLIENTS;
    if (!clients || (const char*)cl < clients) return -1;
    return (int)(((const char*)cl - clients) / CLIENT_SIZE);
}

static void hk_send(void* msg, void* cl) {
    const int cursize = *(const int*)((const char*)msg + MSG_CURSIZE);
    if (cl && cl == g_dropping) return;
    if (cursize > g_check_from) {
        const unsigned char* data = *(unsigned char* const*)((const char*)msg + MSG_DATA);
        int coded = -1;
        if (cursize - 4 <= SCRATCH_SIZE / 4)
            coded = ((huff_code_t)VA_HUFF_CODE)(data + 4, g_scratch, cursize - 4);
        if (coded < 0 || coded + 4 > g_limit) {
            const int snapshot = (uintptr_t)__builtin_return_address(0) == RET_SNAPSHOT;
            printf("%s client %d: %s of %d bytes codes to %d (> %d): %s\n", TAG, client_num(cl),
                   snapshot ? "snapshot" : "message", cursize, coded < 0 ? -1 : coded + 4, g_limit,
                   snapshot ? "dropped (\"Server command overflow\") instead of overflowing the send buffer"
                            : "not sent (would overflow the send buffer)");
            fflush(stdout);
            if (snapshot) {
                g_dropping = cl;
                ((drop_client_t)VA_DROPCLIENT)(cl, "EXE_SERVERCOMMANDOVERFLOW");
                g_dropping = NULL;
            }
            return;
        }
    }
    o_send(msg, cl);
}

void msg_guard_init(void) {
    const unsigned char* p = (const unsigned char*)VA_SEND_MSG;
    const char* e = getenv("COD1RELOADED_MSG_GUARD");
    if (e && !strcmp(e, "0")) {
        printf("%s off (COD1RELOADED_MSG_GUARD=0)\n", TAG);
        fflush(stdout);
        return;
    }
    if (o_send) return;
    e = getenv("COD1RELOADED_MSG_GUARD_LIMIT");
    if (e && atoi(e) >= 1000 && atoi(e) < OUT_LIMIT) {
        g_limit = atoi(e);
        g_check_from = g_limit / 2;
        printf("%s TEST limit %d bytes (COD1RELOADED_MSG_GUARD_LIMIT)\n", TAG, g_limit);
    }
    /* 55 89 e5 81 ec 38 40 00 00 = push ebp; mov ebp,esp; sub esp,0x4038 */
    if (p[0] != 0x55 || p[1] != 0x89 || p[2] != 0xe5 || p[3] != 0x81 || p[4] != 0xec ||
        p[5] != 0x38 || p[6] != 0x40) {
        printf("%s unexpected bytes at 0x%x - not installed\n", TAG, VA_SEND_MSG);
        fflush(stdout);
        return;
    }
    if (hook_install(&h_send, VA_SEND_MSG, (uintptr_t)hk_send, 9) == 0) {
        o_send = (send_msg_t)h_send.trampoline;
        printf("%s on: a message that would overflow the 16 KB send buffer drops its client, "
               "not the server\n", TAG);
    } else {
        printf("%s hook FAILED at 0x%x\n", TAG, VA_SEND_MSG);
    }
    fflush(stdout);
}

#ifdef MSG_GUARD_STANDALONE
/* msgguard.so preloaded next to an older cod1plus.so (the Docker test server) */
__attribute__((constructor)) static void msg_guard_standalone(void) { msg_guard_init(); }
#endif
