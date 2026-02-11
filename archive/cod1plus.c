#include "cod1_defs.h"
#include "config.h"
#include "hooks.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef void (*SV_Frame_t)(int msec);

static hook_t sv_frame_hook;
static SV_Frame_t sv_frame_trampoline = NULL;

static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static int stats_in_flight = 0;
static int last_send_time = 0;
static int send_interval_ms = 5000;

typedef struct {
    char host[256];
    int port;
    char path[256];
} url_parts_t;

typedef struct {
    char *payload;
} post_job_t;

static int appendf(char *buf, size_t size, size_t *offset, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + *offset, size - *offset, fmt, args);
    va_end(args);
    if (written < 0) return -1;
    if ((size_t)written >= size - *offset) return -1;
    *offset += (size_t)written;
    return 0;
}

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' || c == '"') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 't';
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

static int parse_url(const char *url, url_parts_t *out)
{
    if (!url || !out) return -1;
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    const char *path = strchr(p, '/');
    size_t host_len = path ? (size_t)(path - p) : strlen(p);
    if (host_len == 0 || host_len >= sizeof(out->host)) return -1;

    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';
    if (path) {
        strncpy(out->path, path, sizeof(out->path) - 1);
        out->path[sizeof(out->path) - 1] = '\0';
    } else {
        strcpy(out->path, "/");
    }

    out->port = 80;
    char *colon = strchr(out->host, ':');
    if (colon) {
        *colon = '\0';
        int port = atoi(colon + 1);
        if (port > 0 && port < 65536) {
            out->port = port;
        }
    }
    return 0;
}

static int http_post_json(const char *url, const char *json_body)
{
    url_parts_t parts;
    if (parse_url(url, &parts) != 0) return -1;

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", parts.port);
    if (getaddrinfo(parts.host, port_str, &hints, &res) != 0) {
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval timeout;
    timeout.tv_sec = HTTP_TIMEOUT_SECS;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    size_t body_len = strlen(json_body);
    char header[512];
    int header_len = snprintf(
        header,
        sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        parts.path,
        parts.host,
        body_len
    );
    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        close(sock);
        return -1;
    }

    if (send(sock, header, (size_t)header_len, 0) < 0) {
        close(sock);
        return -1;
    }
    if (send(sock, json_body, body_len, 0) < 0) {
        close(sock);
        return -1;
    }

    char tmp[256];
    while (recv(sock, tmp, sizeof(tmp), 0) > 0) {
    }
    close(sock);
    return 0;
}

static int try_set_in_flight(void)
{
    int ok = 0;
    pthread_mutex_lock(&stats_mutex);
    if (!stats_in_flight) {
        stats_in_flight = 1;
        ok = 1;
    }
    pthread_mutex_unlock(&stats_mutex);
    return ok;
}

static void clear_in_flight(void)
{
    pthread_mutex_lock(&stats_mutex);
    stats_in_flight = 0;
    pthread_mutex_unlock(&stats_mutex);
}

static char *build_stats_payload(void)
{
    serverStatic_t *svs = (serverStatic_t *)ADDR_SVS;
    if (!svs || !svs->clients) return NULL;

    size_t buf_size = 65536;
    char *buf = (char *)malloc(buf_size);
    if (!buf) return NULL;
    size_t off = 0;

    if (appendf(buf, buf_size, &off, "{\"server_time\":%d,\"players\":[", svs->time) != 0) {
        free(buf);
        return NULL;
    }

    int first = 1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *cl = (client_t *)((char *)svs->clients + (CLIENT_T_SIZE_V15 * i));
        if (SVSCLIENT_STATE(cl) < CS_CONNECTED) {
            continue;
        }

        gentity_t *ent = *(gentity_t **)((char *)cl + CLIENT_T_OFF_GENTITY);
        if (!ent || !ent->client) {
            continue;
        }

        gclient_t *gc = ent->client;
        char escaped_name[128];
        const char *raw_name = GCLIENT_NETNAME(gc);
        if (!raw_name) raw_name = "";
        json_escape(raw_name, escaped_name, sizeof(escaped_name));

        int score = GCLIENT_SCORE(gc);
        int deaths = GCLIENT_DEATHS(gc);

        if (!first) {
            if (appendf(buf, buf_size, &off, ",") != 0) {
                free(buf);
                return NULL;
            }
        }
        first = 0;

        if (appendf(
                buf,
                buf_size,
                &off,
                "{\"name\":\"%s\",\"score\":%d,\"deaths\":%d}",
                escaped_name,
                score,
                deaths
            ) != 0) {
            free(buf);
            return NULL;
        }
    }

    if (appendf(buf, buf_size, &off, "]}") != 0) {
        free(buf);
        return NULL;
    }

    return buf;
}

static void *post_thread(void *arg)
{
    post_job_t *job = (post_job_t *)arg;
    if (job && job->payload) {
        http_post_json(STATS_FULL_URL, job->payload);
        free(job->payload);
    }
    free(job);
    clear_in_flight();
    return NULL;
}

static void maybe_send_stats(void)
{
    serverStatic_t *svs = (serverStatic_t *)ADDR_SVS;
    if (!svs) return;
    int now = svs->time;
    if (now - last_send_time < send_interval_ms) return;
    if (!try_set_in_flight()) return;

    last_send_time = now;
    char *payload = build_stats_payload();
    if (!payload) {
        clear_in_flight();
        return;
    }

    post_job_t *job = (post_job_t *)malloc(sizeof(post_job_t));
    if (!job) {
        free(payload);
        clear_in_flight();
        return;
    }
    job->payload = payload;

    pthread_t tid;
    if (pthread_create(&tid, NULL, post_thread, job) == 0) {
        pthread_detach(tid);
    } else {
        free(payload);
        free(job);
        clear_in_flight();
    }
}

static void SV_Frame_Hook(int msec)
{
    if (sv_frame_trampoline) {
        sv_frame_trampoline(msec);
    }
    maybe_send_stats();
}

static void cod1plus_init(void) __attribute__((constructor));
static void cod1plus_shutdown(void) __attribute__((destructor));

// Timer thread that sends stats every 5 seconds
static void *stats_thread(void *arg)
{
    (void)arg;

    // Wait 30 seconds for server to fully initialize
    printf("%s Stats thread: waiting 30s for server initialization...\n", COD1PLUS_TAG);
    sleep(30);
    printf("%s Stats thread: starting stats collection\n", COD1PLUS_TAG);

    while (1) {
        sleep(5);
        maybe_send_stats();
    }
    return NULL;
}

static void cod1plus_init(void)
{
    printf("%s Loaded successfully\n", COD1PLUS_TAG);
    printf("%s Memory hooking disabled - will use alternative method\n", COD1PLUS_TAG);
    // No thread, no memory access - just load successfully
}

static void cod1plus_shutdown(void)
{
    hook_remove(&sv_frame_hook);
}
