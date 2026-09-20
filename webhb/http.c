/* Copyright (C) 2025 EchoStretch
   Copyright (C) 2026 slopmaster33

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

/* The web server itself: sockets, request parsing, responses and the route
   table. It knows nothing about what the routes do. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "webhb.h"

#define MAX_CONNECTIONS   8
#define REQUEST_MAX       8192
#define BODY_MAX          4096
#define LOG_LINES_PER_POLL 120

static int              g_listen_fd = -1;
static int              g_port = 0;
static volatile int     g_running = 0;
static int              g_active_conns = 0;
static pthread_mutex_t  g_conn_mtx = PTHREAD_MUTEX_INITIALIZER;

void sb_init(sb_t *sb)
{
    sb->cap = 4096;
    sb->len = 0;
    sb->oom = 0;
    sb->buf = malloc(sb->cap);
    if (!sb->buf) sb->oom = 1;
    else sb->buf[0] = '\0';
}

void sb_free(sb_t *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = sb->cap = 0;
}

static int sb_reserve(sb_t *sb, size_t extra)
{
    if (sb->oom) return -1;
    if (sb->len + extra + 1 <= sb->cap) return 0;

    size_t cap = sb->cap;
    while (cap < sb->len + extra + 1) cap *= 2;

    char *n = realloc(sb->buf, cap);
    if (!n) { sb->oom = 1; return -1; }

    sb->buf = n;
    sb->cap = cap;
    return 0;
}

void sb_putm(sb_t *sb, const char *data, size_t len)
{
    if (sb_reserve(sb, len) != 0) return;
    memcpy(sb->buf + sb->len, data, len);
    sb->len += len;
    sb->buf[sb->len] = '\0';
}

void sb_puts(sb_t *sb, const char *s)
{
    if (s) sb_putm(sb, s, strlen(s));
}

void sb_printf(sb_t *sb, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n > 0) sb_putm(sb, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* Appends s as a quoted JSON string. */
void sb_json_str(sb_t *sb, const char *s)
{
    sb_puts(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"':  sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\n': sb_puts(sb, "\\n");  break;
            case '\r': sb_puts(sb, "\\r");  break;
            case '\t': sb_puts(sb, "\\t");  break;
            default:
                if (*p < 0x20) sb_printf(sb, "\\u%04x", *p);
                else           sb_putm(sb, (const char *)p, 1);
        }
    }
    sb_puts(sb, "\"");
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *in, size_t in_len, char *out, size_t out_size)
{
    size_t o = 0;

    for (size_t i = 0; i < in_len && o + 1 < out_size; i++) {
        if (in[i] == '%' && i + 2 < in_len) {
            int hi = hex_val(in[i + 1]), lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[o++] = (in[i] == '+') ? ' ' : in[i];
    }
    out[o] = '\0';
}

/* Parses "a=1&b=hello%20world" into params. */
static void params_parse(params_t *p, const char *query)
{
    if (!query) return;

    while (*query && p->count < MAX_PARAMS) {
        const char *amp = strchr(query, '&');
        const char *end = amp ? amp : query + strlen(query);
        const char *eq  = memchr(query, '=', (size_t)(end - query));

        if (eq && eq > query) {
            param_t *item = &p->items[p->count];
            url_decode(query, (size_t)(eq - query), item->key, sizeof(item->key));
            url_decode(eq + 1, (size_t)(end - eq - 1), item->val, sizeof(item->val));
            if (item->key[0]) p->count++;
        }

        if (!amp) break;
        query = amp + 1;
    }
}

const char *param_get(const params_t *p, const char *key, const char *fallback)
{
    for (int i = 0; i < p->count; i++)
        if (strcmp(p->items[i].key, key) == 0) return p->items[i].val;
    return fallback;
}

int param_get_int(const params_t *p, const char *key, int fallback)
{
    const char *v = param_get(p, key, NULL);
    return v && *v ? atoi(v) : fallback;
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static const char *status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 429: return "Too Many Requests";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
    }
    return "OK";
}

void send_response_cc(int fd, int code, const char *content_type,
                             const void *body, size_t len,
                             const char *cache_control, const char *extra_headers)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: %s\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     code, status_text(code), content_type, len, cache_control,
                     extra_headers ? extra_headers : "");

    if (n <= 0) return;
    if (send_all(fd, head, (size_t)n) != 0) return;
    if (len) send_all(fd, body, len);
}

void send_response(int fd, int code, const char *content_type,
                          const void *body, size_t len, const char *extra_headers)
{
    send_response_cc(fd, code, content_type, body, len, "no-store", extra_headers);
}

void send_json(int fd, int code, const char *json)
{
    send_response(fd, code, "application/json; charset=utf-8",
                  json, strlen(json), NULL);
}

void send_sb(int fd, int code, sb_t *sb)
{
    if (sb->oom) send_json(fd, 500, "{\"error\":\"out of memory\"}");
    else         send_json(fd, code, sb->buf);
    sb_free(sb);
}

void send_error(int fd, int code, const char *message)
{
    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"error\":");
    sb_json_str(&sb, message);
    sb_puts(&sb, "}");
    send_sb(fd, code, &sb);
}

/* Streams a file from disk, used for the app icons. */
void send_file(int fd, const char *path, const char *content_type)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_error(fd, 404, "not found");
        return;
    }

    int in = open(path, O_RDONLY);
    if (in < 0) {
        send_error(fd, 404, "not found");
        return;
    }

    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Cache-Control: max-age=300\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     content_type, (long long)st.st_size);

    if (n > 0 && send_all(fd, head, (size_t)n) == 0) {
        char buf[16384];
        ssize_t r;
        while ((r = read(in, buf, sizeof(buf))) > 0)
            if (send_all(fd, buf, (size_t)r) != 0) break;
    }

    close(in);
}

typedef struct {
    sb_t    *sb;
    int      written;
    int      limit;
    unsigned last_seq;
} log_walk_ctx_t;

static void log_walk_cb(void *ctx, unsigned seq, const char *line)
{
    log_walk_ctx_t *c = (log_walk_ctx_t *)ctx;
    if (c->written >= c->limit) return;

    if (c->written) sb_puts(c->sb, ",");
    sb_json_str(c->sb, line);
    c->written++;
    c->last_seq = seq;
}

void json_log(sb_t *sb, unsigned since)
{
    /* A page left open across a payload restart asks for a sequence the
       fresh ring never reaches - rewind it instead of going silent. */
    unsigned newest = log_ring_seq();
    if (since > newest) since = newest;

    log_walk_ctx_t ctx = { sb, 0, LOG_LINES_PER_POLL, since };

    sb_puts(sb, "{\"lines\":[");
    log_ring_walk(since, log_walk_cb, &ctx);
    sb_printf(sb, "],\"seq\":%u}", ctx.last_seq);
}

/* ------------------------------------------------------------------ */
/*  Routes                                                             */
/* ------------------------------------------------------------------ */

#define MAX_ROUTES 64

static struct { const char *method, *path; http_handler_t fn; } g_routes[MAX_ROUTES];
static int g_route_count = 0;
static void (*g_on_listening)(int port);

void http_route(const char *method, const char *path, http_handler_t fn)
{
    if (g_route_count < MAX_ROUTES) {
        g_routes[g_route_count].method = method;
        g_routes[g_route_count].path = path;
        g_routes[g_route_count].fn = fn;
        g_route_count++;
    }
}

#define MAX_OPEN_ROUTES 8
static const char *g_open_routes[MAX_OPEN_ROUTES];
static int g_open_count = 0;

void http_route_open(const char *path)
{
    if (g_open_count < MAX_OPEN_ROUTES) g_open_routes[g_open_count++] = path;
}

int http_peer_is_local(int fd)
{
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &len) != 0) return 0;
    return peer.sin_family == AF_INET &&
           (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
}

/* Reading is free; changing something takes the token, unless the request
   comes from the console itself. See access.c. */
static int may_pass(int fd, const char *method, const char *path, const params_t *p)
{
    if (strcmp(method, "POST") != 0) return 1;
    for (int i = 0; i < g_open_count; i++)
        if (strcmp(g_open_routes[i], path) == 0) return 1;
    return http_peer_is_local(fd) || whb_access_token_ok(param_get(p, "token", NULL));
}

void http_on_listening(void (*fn)(int port))
{
    g_on_listening = fn;
}

static void dispatch(int fd, const char *method, const char *path, const params_t *p)
{
    for (int i = 0; i < g_route_count; i++) {
        if (strcmp(g_routes[i].method, method) == 0 && strcmp(g_routes[i].path, path) == 0) {
            if (!may_pass(fd, method, path, p)) {
                send_error(fd, 401, "enter the code shown on the TV first");
                return;
            }
            g_routes[i].fn(fd, p);
            return;
        }
    }
    send_error(fd, 404, "no such endpoint");
}

/* Reads the request head plus body. Returns the number of bytes read or -1. */
static int read_request(int fd, char *buf, size_t buf_size, size_t *head_len)
{
    size_t len = 0;
    char *end = NULL;

    while (len + 1 < buf_size) {
        ssize_t n = recv(fd, buf + len, buf_size - len - 1, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }

        len += (size_t)n;
        buf[len] = '\0';

        end = strstr(buf, "\r\n\r\n");
        if (end) break;
    }

    if (!end) return -1;
    *head_len = (size_t)(end - buf) + 4;

    /* pull in the body when the client announced one */
    const char *cl = strcasestr(buf, "\r\nContent-Length:");
    if (cl) {
        long want = strtol(cl + 17, NULL, 10);
        if (want > BODY_MAX) want = BODY_MAX;

        while (want > 0 && len < *head_len + (size_t)want && len + 1 < buf_size) {
            ssize_t n = recv(fd, buf + len, buf_size - len - 1, 0);
            if (n <= 0) break;
            len += (size_t)n;
            buf[len] = '\0';
        }
    }

    return (int)len;
}

static void handle_connection(int fd)
{
    char buf[REQUEST_MAX];
    size_t head_len = 0;

    if (read_request(fd, buf, sizeof(buf), &head_len) < 0) return;

    /* request line: METHOD SP PATH SP VERSION */
    char *sp1 = strchr(buf, ' ');
    if (!sp1) { send_error(fd, 400, "malformed request"); return; }
    *sp1 = '\0';

    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2) { send_error(fd, 400, "malformed request"); return; }
    *sp2 = '\0';

    const char *method = buf;

    char *query = strchr(target, '?');
    if (query) *query++ = '\0';

    char path[256];
    url_decode(target, strlen(target), path, sizeof(path));

    params_t params = { .count = 0 };
    params_parse(&params, query);
    if (head_len && buf[head_len]) params_parse(&params, buf + head_len);

    dispatch(fd, method, path, &params);
}

static void *connection_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;

    handle_connection(fd);
    close(fd);

    pthread_mutex_lock(&g_conn_mtx);
    g_active_conns--;
    pthread_mutex_unlock(&g_conn_mtx);

    return NULL;
}

/* Finds the address the console can be reached on, so the notification can
   show a URL the user can type into a phone. */
static void local_ipv4(char *out, size_t out_size)
{
    struct ifaddrs *list = NULL;

    snprintf(out, out_size, "%s", "ps5");

    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;

            struct sockaddr_in *in = (struct sockaddr_in *)ifa->ifa_addr;
            if (inet_ntop(AF_INET, &in->sin_addr, out, (socklen_t)out_size)) {
                freeifaddrs(list);
                return;
            }
        }
        freeifaddrs(list);
    }

    /* fallback: ask the routing table which source address it would use */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;

    struct sockaddr_in probe;
    memset(&probe, 0, sizeof(probe));
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    probe.sin_addr.s_addr = inet_addr("1.1.1.1");

    if (connect(fd, (struct sockaddr *)&probe, sizeof(probe)) == 0) {
        struct sockaddr_in me;
        socklen_t len = sizeof(me);
        if (getsockname(fd, (struct sockaddr *)&me, &len) == 0)
            inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)out_size);
    }
    close(fd);
}

int http_server_port(void)
{
    return g_port;
}

void http_server_stop(void)
{
    g_running = 0;
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
}

/* Opens the listening socket on port, or on one of the span - 1 ports above
   it when that one is taken. Returns the descriptor and the port it got, or
   -1 with what failed in *what. */
static int open_listener(int *port, int span, const char **what)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *what = "socket()"; return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int bound = 0;
    for (int attempt = 0; attempt < span && !bound; attempt++) {
        addr.sin_port = htons((uint16_t)(*port + attempt));
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            *port += attempt;
            bound = 1;
        }
    }

    if (!bound || listen(fd, 8) != 0) {
        int err = errno;
        *what = bound ? "listen()" : "bind()";
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

/* What accept() says on the console when the network is switched off under
   it - rest mode does that. Known to psdevwiki as
   SCE_NET_ERROR_EINACTIVEDISABLED (0x804101A3); the libc has no name for it. */
#define ERRNO_NET_INACTIVE 163
/* How often a lost network is looked for again. */
#define NET_RETRY_SECONDS  3

/* The listening socket died with the network. Waits for the network to come
   back and listens on the same port again, so that the tile, bookmarks and the
   cached page still find the web UI. Returns 0, or -1 when asked to stop. */
static int listen_again(void)
{
    close(g_listen_fd);
    g_listen_fd = -1;

    while (g_running) {
        sleep(NET_RETRY_SECONDS);

        int port = g_port;
        const char *what = NULL;
        int fd = open_listener(&port, 1, &what);
        if (fd < 0) continue;

        g_listen_fd = fd;
        write_log(g_log_path, "Web UI: the network is back - listening on port %d again", g_port);

        /* said on the TV as well, as other payloads do after rest mode: it
           tells that this one came through, and where it is */
        char ip[64];
        local_ipv4(ip, sizeof(ip));
        printf_notification_quiet("%s is back\nhttp://%s:%d", whb_app()->name, ip, g_port);
        return 0;
    }
    return -1;
}

int http_server_run(int port)
{
    /* a client that disappears mid-response must not take the payload down */
    signal(SIGPIPE, SIG_IGN);

    /* the homebrew launcher usually holds 8080, so walk a few ports up */
    const char *what = NULL;
    g_listen_fd = open_listener(&port, 10, &what);
    if (g_listen_fd < 0) {
        if (!strcmp(what, "bind()"))
            printf_notification("Web UI: ports %d-%d are busy (%s)", port, port + 9, strerror(errno));
        else
            printf_notification("Web UI: %s failed (%s)", what, strerror(errno));
        return -1;
    }

    int one = 1;
    g_running = 1;
    g_port = port;

    char ip[64];
    local_ipv4(ip, sizeof(ip));
    /* the toast carries the access code, the log - open to all - does not */
    whb_access_notify(ip, port);
    char line[160];
    snprintf(line, sizeof(line), "%s: open http://%s:%d", whb_app()->name, ip, port);
    log_ring_push(line);
    write_log(g_log_path, "Web UI listening on http://%s:%d", ip, port);

    if (g_on_listening) g_on_listening(port);

    while (g_running) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        int fd = accept(g_listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            /* Only a dead listening socket is worth giving up for. Anything
               transient - an interrupted call, a client that vanished, a
               momentary shortage of descriptors - used to end the accept
               loop, which ended the payload along with it. */
            if (errno == EINTR || errno == ECONNABORTED ||
                errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
                if (errno == EMFILE || errno == ENFILE || errno == ENOMEM)
                    usleep(100000);   /* give the system a moment to recover */
                continue;
            }

            /* The network went away, and the socket with it: the console
               was put into rest mode, or its network was restarted. That
               used to end the payload; now it waits for the network. */
            if (g_running && errno == ERRNO_NET_INACTIVE) {
                write_log(g_log_path, "Web UI: the network was switched off (rest mode?) - "
                                      "waiting for it to come back");
                if (listen_again() == 0) continue;
                break;
            }

            if (g_running)
                write_log(g_log_path, "Web UI: accept() failed: %s - shutting down",
                          strerror(errno));
            break;
        }

        pthread_mutex_lock(&g_conn_mtx);
        int busy = (g_active_conns >= MAX_CONNECTIONS);
        if (!busy) g_active_conns++;
        pthread_mutex_unlock(&g_conn_mtx);

        if (busy) {
            send_error(fd, 503, "too many connections");
            close(fd);
            continue;
        }

        /* Browsers open spare connections and send nothing on them, so the
           read side gives up quickly; sending may legitimately take longer. */
        struct timeval rcv = { .tv_sec = 5,  .tv_usec = 0 };
        struct timeval snd = { .tv_sec = 20, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        /* the default is too small for the request buffers plus whatever
           the dumper modules put on the stack below us */
        pthread_attr_setstacksize(&attr, 512 * 1024);

        if (pthread_create(&tid, &attr, connection_thread, (void *)(intptr_t)fd) != 0) {
            pthread_mutex_lock(&g_conn_mtx);
            g_active_conns--;
            pthread_mutex_unlock(&g_conn_mtx);
            close(fd);
        }

        pthread_attr_destroy(&attr);
    }

    if (g_listen_fd >= 0) close(g_listen_fd);
    g_listen_fd = -1;

    /* If this shows up without the user asking for a shutdown, something
       outside the payload took the socket away. */
    printf_notification("%s: web UI stopped", whb_app()->name);
    write_log(g_log_path, "Web UI: server loop ended");
    return 0;
}
