/* Copyright (C) 2025 EchoStretch

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

#include "http_server.h"
#include "web_assets.h"
#include "app_scan.h"
#include "fs_browse.h"
#include "app_launch.h"
#include "dump_job.h"
#include "utils.h"

#define MAX_CONNECTIONS   8
#define REQUEST_MAX       8192
#define BODY_MAX          4096
#define LOG_LINES_PER_POLL 120

static int              g_listen_fd = -1;
static int              g_port = 0;
static volatile int     g_running = 0;
static int              g_active_conns = 0;
static pthread_mutex_t  g_conn_mtx = PTHREAD_MUTEX_INITIALIZER;
static dumper_config_t  g_cfg;
static pthread_mutex_t  g_cfg_mtx = PTHREAD_MUTEX_INITIALIZER;
/* settings were changed while no drive was around to store them */
static int              g_cfg_unsaved = 0;

/* ------------------------------------------------------------------ */
/*  String builder                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    oom;
} sb_t;

static void sb_init(sb_t *sb)
{
    sb->cap = 4096;
    sb->len = 0;
    sb->oom = 0;
    sb->buf = malloc(sb->cap);
    if (!sb->buf) sb->oom = 1;
    else sb->buf[0] = '\0';
}

static void sb_free(sb_t *sb)
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

static void sb_putm(sb_t *sb, const char *data, size_t len)
{
    if (sb_reserve(sb, len) != 0) return;
    memcpy(sb->buf + sb->len, data, len);
    sb->len += len;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(sb_t *sb, const char *s)
{
    if (s) sb_putm(sb, s, strlen(s));
}

static void sb_printf(sb_t *sb, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n > 0) sb_putm(sb, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* Appends s as a quoted JSON string. */
static void sb_json_str(sb_t *sb, const char *s)
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

/* ------------------------------------------------------------------ */
/*  Request parsing                                                    */
/* ------------------------------------------------------------------ */

#define MAX_PARAMS 16

typedef struct {
    char key[32];
    char val[192];
} param_t;

typedef struct {
    param_t items[MAX_PARAMS];
    int     count;
} params_t;

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

static const char *param_get(const params_t *p, const char *key, const char *fallback)
{
    for (int i = 0; i < p->count; i++)
        if (strcmp(p->items[i].key, key) == 0) return p->items[i].val;
    return fallback;
}

static int param_get_int(const params_t *p, const char *key, int fallback)
{
    const char *v = param_get(p, key, NULL);
    return v && *v ? atoi(v) : fallback;
}

/* ------------------------------------------------------------------ */
/*  Socket helpers                                                     */
/* ------------------------------------------------------------------ */

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
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
    }
    return "OK";
}

static void send_response(int fd, int code, const char *content_type,
                          const void *body, size_t len, const char *extra_headers)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     code, status_text(code), content_type, len,
                     extra_headers ? extra_headers : "");

    if (n <= 0) return;
    if (send_all(fd, head, (size_t)n) != 0) return;
    if (len) send_all(fd, body, len);
}

static void send_json(int fd, int code, const char *json)
{
    send_response(fd, code, "application/json; charset=utf-8",
                  json, strlen(json), NULL);
}

static void send_sb(int fd, int code, sb_t *sb)
{
    if (sb->oom) send_json(fd, 500, "{\"error\":\"out of memory\"}");
    else         send_json(fd, code, sb->buf);
    sb_free(sb);
}

static void send_error(int fd, int code, const char *message)
{
    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"error\":");
    sb_json_str(&sb, message);
    sb_puts(&sb, "}");
    send_sb(fd, code, &sb);
}

/* Streams a file from disk, used for the app icons. */
static void send_file(int fd, const char *path, const char *content_type)
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

/* ------------------------------------------------------------------ */
/*  JSON payloads                                                      */
/* ------------------------------------------------------------------ */

static void json_config(sb_t *sb, const dumper_config_t *cfg)
{
    sb_printf(sb,
        "{\"enableDecrypter\":%d,\"enableBackport\":%d,"
        "\"ps4BackportLevel\":%d,\"ps5BackportLevel\":%d,"
        "\"enableElf2fself\":%d,\"enableLogging\":%d,\"split\":%d,"
        "\"enableWebui\":%d,\"webPort\":%d,\"autoStart\":%d,\"dumpSubdir\":",
        cfg->enable_decrypter, cfg->enable_backport,
        cfg->ps4_backport_level, cfg->ps5_backport_level,
        cfg->enable_elf2fself, cfg->enable_logging, cfg->split,
        cfg->enable_webui, cfg->web_port, cfg->auto_start);
    sb_json_str(sb, cfg->dump_subdir);
    sb_puts(sb, "}");
}

static void json_job(sb_t *sb, const job_status_t *job)
{
    sb_puts(sb, "{\"state\":");
    sb_json_str(sb, job_state_name(job->state));
    sb_puts(sb, ",\"appDir\":");
    sb_json_str(sb, job->app_dir);
    sb_puts(sb, ",\"title\":");
    sb_json_str(sb, job->title);
    sb_puts(sb, ",\"titleId\":");
    sb_json_str(sb, job->title_id);
    sb_puts(sb, ",\"dest\":");
    sb_json_str(sb, job->dest);
    sb_puts(sb, ",\"stage\":");
    sb_json_str(sb, job->stage);
    sb_puts(sb, ",\"currentFile\":");
    sb_json_str(sb, job->current_file);
    sb_puts(sb, ",\"message\":");
    sb_json_str(sb, job->message);
    sb_printf(sb, ",\"totalBytes\":%llu,\"copiedBytes\":%llu,"
                  "\"started\":%lld,\"finished\":%lld}",
              (unsigned long long)job->total_bytes,
              (unsigned long long)job->copied_bytes,
              (long long)job->started, (long long)job->finished);
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

static void json_log(sb_t *sb, unsigned since)
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
/*  Route handlers                                                     */
/* ------------------------------------------------------------------ */

static void handle_status(int fd, const params_t *q)
{
    job_status_t job;
    job_get_status(&job);

    sb_t sb;
    sb_init(&sb);

    sb_puts(&sb, "{\"job\":");
    json_job(&sb, &job);
    sb_puts(&sb, ",\"log\":");
    json_log(&sb, (unsigned)param_get_int(q, "since", 0));
    sb_printf(&sb, ",\"busy\":%s}", job_is_active() ? "true" : "false");

    send_sb(fd, 200, &sb);
}

static void handle_devices(int fd)
{
    /* Pick up a drive that was plugged in after the payload started. Only a
       drive that actually turned up may touch the settings - rereading them
       on every poll would throw away what the user just changed. */
    if (!get_usb_homebrew_path()[0] && find_usb_and_setup() >= 0) {
        pthread_mutex_lock(&g_cfg_mtx);
        if (g_cfg_unsaved) {
            /* carry this session's changes onto the drive that just appeared */
            if (config_save(&g_cfg) == 0) g_cfg_unsaved = 0;
        } else {
            config_load(&g_cfg);
        }
        g_enable_logging = g_cfg.enable_logging;
        g_split_mode = g_cfg.split;
        pthread_mutex_unlock(&g_cfg_mtx);
    }

    app_entry_t *apps = calloc(APP_SCAN_MAX, sizeof(*apps));
    target_entry_t *targets = calloc(TARGET_SCAN_MAX, sizeof(*targets));
    if (!apps || !targets) {
        free(apps); free(targets);
        send_error(fd, 500, "out of memory");
        return;
    }

    int app_count = app_scan(apps, APP_SCAN_MAX);
    int target_count = target_scan(targets, TARGET_SCAN_MAX);

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"apps\":[");

    for (int i = 0; i < app_count; i++) {
        if (i) sb_puts(&sb, ",");
        sb_puts(&sb, "{\"dir\":");
        sb_json_str(&sb, apps[i].dir);
        sb_puts(&sb, ",\"titleId\":");
        sb_json_str(&sb, apps[i].title_id);
        sb_puts(&sb, ",\"title\":");
        sb_json_str(&sb, apps[i].title[0] ? apps[i].title : apps[i].title_id);
        sb_puts(&sb, ",\"version\":");
        sb_json_str(&sb, apps[i].version);
        sb_puts(&sb, ",\"patchDir\":");
        sb_json_str(&sb, apps[i].patch_dir);
        sb_printf(&sb, ",\"isPs4\":%s,\"hasIcon\":%s,\"media\":\"%s\"}",
                  apps[i].is_ps4 ? "true" : "false",
                  apps[i].has_icon ? "true" : "false",
                  apps[i].on_disc ? "disc" : "pkg");
    }

    sb_puts(&sb, "],\"targets\":[");

    for (int i = 0; i < target_count; i++) {
        if (i) sb_puts(&sb, ",");
        sb_puts(&sb, "{\"mount\":");
        sb_json_str(&sb, targets[i].mount);
        sb_puts(&sb, ",\"fs\":");
        sb_json_str(&sb, targets[i].fs);
        sb_printf(&sb, ",\"writable\":%s,\"totalBytes\":%llu,\"freeBytes\":%llu}",
                  targets[i].writable ? "true" : "false",
                  (unsigned long long)targets[i].total_bytes,
                  (unsigned long long)targets[i].free_bytes);
    }

    sb_puts(&sb, "],\"config\":");
    pthread_mutex_lock(&g_cfg_mtx);
    json_config(&sb, &g_cfg);
    pthread_mutex_unlock(&g_cfg_mtx);

    sb_puts(&sb, ",\"configPath\":");
    const char *hb = get_usb_homebrew_path();
    if (hb && hb[0]) {
        char path[256];
        snprintf(path, sizeof(path), "%s/config.ini", hb);
        sb_json_str(&sb, path);
    } else {
        sb_puts(&sb, "null");
    }

    sb_puts(&sb, "}");

    free(apps);
    free(targets);
    send_sb(fd, 200, &sb);
}

static void handle_config_get(int fd)
{
    sb_t sb;
    sb_init(&sb);

    pthread_mutex_lock(&g_cfg_mtx);
    json_config(&sb, &g_cfg);
    pthread_mutex_unlock(&g_cfg_mtx);

    send_sb(fd, 200, &sb);
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void handle_config_post(int fd, const params_t *p)
{
    pthread_mutex_lock(&g_cfg_mtx);
    dumper_config_t cfg = g_cfg;

    cfg.enable_decrypter   = param_get_int(p, "enableDecrypter",  cfg.enable_decrypter) ? 1 : 0;
    cfg.enable_backport    = param_get_int(p, "enableBackport",   cfg.enable_backport) ? 1 : 0;
    cfg.ps4_backport_level = clamp(param_get_int(p, "ps4BackportLevel", cfg.ps4_backport_level), 1, 6);
    cfg.ps5_backport_level = clamp(param_get_int(p, "ps5BackportLevel", cfg.ps5_backport_level), 1, 10);
    cfg.enable_elf2fself   = param_get_int(p, "enableElf2fself",  cfg.enable_elf2fself) ? 1 : 0;
    cfg.enable_logging     = param_get_int(p, "enableLogging",    cfg.enable_logging) ? 1 : 0;
    cfg.split              = clamp(param_get_int(p, "split",      cfg.split), 0, 3);
    cfg.enable_webui       = param_get_int(p, "enableWebui",      cfg.enable_webui) ? 1 : 0;
    cfg.web_port           = clamp(param_get_int(p, "webPort",    cfg.web_port), 1024, 65535);
    cfg.auto_start         = param_get_int(p, "autoStart",        cfg.auto_start) ? 1 : 0;

    const char *subdir = param_get(p, "dumpSubdir", NULL);
    if (subdir) {
        /* a destination folder must stay below the mount point */
        if (strstr(subdir, "..") || subdir[0] == '/') {
            pthread_mutex_unlock(&g_cfg_mtx);
            send_error(fd, 400, "invalid destination folder");
            return;
        }
        strncpy(cfg.dump_subdir, subdir, sizeof(cfg.dump_subdir) - 1);
        cfg.dump_subdir[sizeof(cfg.dump_subdir) - 1] = '\0';
    }

    g_cfg = cfg;
    g_enable_logging = cfg.enable_logging;
    g_split_mode = cfg.split;

    int saved = config_save(&cfg);
    g_cfg_unsaved = (saved == 0) ? 0 : 1;
    pthread_mutex_unlock(&g_cfg_mtx);

    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"saved\":%s,\"config\":", saved == 0 ? "true" : "false");
    json_config(&sb, &cfg);
    sb_puts(&sb, ",\"note\":");
    sb_json_str(&sb, saved == 0
                ? "settings written to config.ini"
                : "no drive connected - settings stay active until the payload restarts, "
                  "and are written as soon as a drive shows up");
    sb_puts(&sb, "}");
    send_sb(fd, 200, &sb);
}

static void handle_dump(int fd, const params_t *p)
{
    const char *app = param_get(p, "app", NULL);
    const char *target = param_get(p, "target", NULL);

    if (!app || !*app)       { send_error(fd, 400, "no app selected"); return; }
    if (!target || !*target) { send_error(fd, 400, "no destination selected"); return; }

    pthread_mutex_lock(&g_cfg_mtx);
    dumper_config_t cfg = g_cfg;
    pthread_mutex_unlock(&g_cfg_mtx);

    char err[160] = {0};
    if (job_start(app, target, &cfg, err, sizeof(err)) != 0) {
        send_error(fd, 409, err[0] ? err : "could not start the dump");
        return;
    }

    sb_t sb;
    sb_init(&sb);
    job_status_t job;
    job_get_status(&job);
    sb_puts(&sb, "{\"started\":true,\"job\":");
    json_job(&sb, &job);
    sb_puts(&sb, "}");
    send_sb(fd, 200, &sb);
}

static void handle_abort(int fd)
{
    if (!job_is_active()) {
        send_error(fd, 409, "no dump is running");
        return;
    }

    job_abort();
    send_json(fd, 200, "{\"stopping\":true}");
}

static void handle_icon(int fd, const params_t *p)
{
    const char *dir = param_get(p, "app", NULL);
    if (!dir || !*dir) { send_error(fd, 400, "no app given"); return; }

    app_entry_t app;
    char path[512];

    if (app_find(dir, &app) != 0 || app_icon_path(&app, path, sizeof(path)) != 0) {
        send_error(fd, 404, "no icon");
        return;
    }

    send_file(fd, path, "image/png");
}

static void handle_size(int fd, const params_t *p)
{
    const char *dir = param_get(p, "app", NULL);
    if (!dir || !*dir) { send_error(fd, 400, "no app given"); return; }

    app_entry_t app;
    if (app_find(dir, &app) != 0) { send_error(fd, 404, "app is not mounted"); return; }

    char json[128];
    snprintf(json, sizeof(json), "{\"bytes\":%llu}", (unsigned long long)app_size(&app));
    send_json(fd, 200, json);
}

static void handle_library(int fd)
{
    /* A console with a full library needs ~25 KB here, which is more than a
       connection thread's stack can take. */
    library_entry_t *lib = calloc(LIBRARY_SCAN_MAX, sizeof(*lib));
    if (!lib) { send_error(fd, 500, "out of memory"); return; }

    int count = library_scan(lib, LIBRARY_SCAN_MAX);

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"titles\":[");

    for (int i = 0; i < count; i++) {
        if (i) sb_puts(&sb, ",");
        sb_puts(&sb, "{\"titleId\":");
        sb_json_str(&sb, lib[i].title_id);
        sb_puts(&sb, ",\"title\":");
        sb_json_str(&sb, lib[i].title[0] ? lib[i].title : lib[i].title_id);
        sb_puts(&sb, ",\"version\":");
        sb_json_str(&sb, lib[i].version);
        sb_puts(&sb, ",\"source\":");
        sb_json_str(&sb, lib[i].source);
        sb_printf(&sb, ",\"isPs4\":%s,\"hasIcon\":%s,\"isRunning\":%s,\"media\":\"%s\"}",
                  lib[i].is_ps4 ? "true" : "false",
                  lib[i].has_icon ? "true" : "false",
                  lib[i].is_running ? "true" : "false",
                  lib[i].on_disc ? "disc" : "pkg");
    }

    /* Whether a title is up is already known from the pfsmnt scan, so the
       polling listing never asks the system service - that call reaches
       into SceLncUtil and is not worth doing every few seconds while a
       game is running. */
    int running = 0;
    for (int i = 0; i < count; i++)
        if (lib[i].is_running) running = 1;

    sb_printf(&sb, "],\"aTitleIsRunning\":%s,\"canLaunch\":%s}",
              running ? "true" : "false",
              app_launch_probably_available() ? "true" : "false");

    free(lib);
    send_sb(fd, 200, &sb);
}

static void handle_launch(int fd, const params_t *p)
{
    const char *title = param_get(p, "title", NULL);
    if (!title || !*title) { send_error(fd, 400, "no title given"); return; }

    if (job_is_active()) {
        send_error(fd, 409, "a dump is running");
        return;
    }

    int close_running = param_get_int(p, "force", 0);
    char err[192] = {0};

    if (app_launch_title(title, close_running, err, sizeof(err)) != 0) {
        /* the caller may retry with force=1 once the player agrees */
        int running = (strstr(err, "another game is running") != NULL);
        sb_t sb;
        sb_init(&sb);
        sb_puts(&sb, "{\"error\":");
        sb_json_str(&sb, err[0] ? err : "could not start the title");
        sb_printf(&sb, ",\"needsClose\":%s}", running ? "true" : "false");
        send_sb(fd, 409, &sb);
        return;
    }

    send_json(fd, 200, "{\"launched\":true}");
}

static void handle_library_icon(int fd, const params_t *p)
{
    const char *title = param_get(p, "title", NULL);
    if (!title || !*title) { send_error(fd, 400, "no title given"); return; }

    /* library_icon_path only accepts a title id, so no path can be injected */
    char path[512];
    if (library_icon_path(title, path, sizeof(path)) != 0) {
        send_error(fd, 404, "no icon");
        return;
    }

    send_file(fd, path, "image/png");
}

/* Resolves the drive to browse: the caller-named mount if it is one of ours,
   otherwise the drive holding config.ini. Returns NULL when none is usable. */
static const char *browse_mount(const params_t *p)
{
    const char *mount = param_get(p, "mount", NULL);
    if (mount && *mount && target_is_known(mount) == 0) return mount;
    return NULL;
}

static void handle_browse(int fd, const params_t *p)
{
    const char *mount = browse_mount(p);
    if (!mount) { send_error(fd, 400, "unknown drive"); return; }

    const char *rel = param_get(p, "path", "");
    if (!fs_path_is_safe(rel)) { send_error(fd, 400, "invalid path"); return; }

    /* the listing can be large; keep it off the connection thread's stack */
    char (*names)[FS_NAME_MAX] = calloc(FS_BROWSE_MAX, FS_NAME_MAX);
    if (!names) { send_error(fd, 500, "out of memory"); return; }

    int count = fs_list_dirs(mount, rel, names, FS_BROWSE_MAX);
    if (count < 0) { free(names); send_error(fd, 404, "cannot open folder"); return; }

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"mount\":");
    sb_json_str(&sb, mount);
    sb_puts(&sb, ",\"path\":");
    sb_json_str(&sb, rel);
    sb_puts(&sb, ",\"dirs\":[");
    for (int i = 0; i < count; i++) {
        if (i) sb_puts(&sb, ",");
        sb_json_str(&sb, names[i]);
    }
    sb_puts(&sb, "]}");

    free(names);
    send_sb(fd, 200, &sb);
}

static void handle_mkdir(int fd, const params_t *p)
{
    const char *mount = browse_mount(p);
    if (!mount) { send_error(fd, 400, "unknown drive"); return; }

    const char *rel  = param_get(p, "path", "");
    const char *name = param_get(p, "name", NULL);
    if (!name || !*name)          { send_error(fd, 400, "no folder name"); return; }
    if (!fs_name_is_safe(name))   { send_error(fd, 400, "invalid folder name"); return; }
    if (!fs_path_is_safe(rel))    { send_error(fd, 400, "invalid path"); return; }

    char newrel[128];
    if (fs_make_subdir(mount, rel, name, newrel, sizeof(newrel)) != 0) {
        send_error(fd, 400, "could not create the folder (name too long or not writable)");
        return;
    }

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"path\":");
    sb_json_str(&sb, newrel);
    sb_puts(&sb, "}");
    send_sb(fd, 200, &sb);
}

static void handle_quit(int fd)
{
    if (job_is_active()) {
        send_error(fd, 409, "a dump is running");
        return;
    }

    send_json(fd, 200, "{\"stopping\":true}");
    printf_notification("PS5 App Dumper: web UI closed");
    http_server_stop();
}

static void handle_index(int fd)
{
    send_response(fd, 200, "text/html; charset=utf-8",
                  web_index_html, web_index_html_len, NULL);
}

static void route(int fd, const char *method, const char *path, const params_t *p)
{
    int is_get  = (strcmp(method, "GET") == 0);
    int is_post = (strcmp(method, "POST") == 0);

    if (is_get && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) handle_index(fd);
    else if (is_get  && !strcmp(path, "/api/status"))  handle_status(fd, p);
    else if (is_get  && !strcmp(path, "/api/devices")) handle_devices(fd);
    else if (is_get  && !strcmp(path, "/api/config"))  handle_config_get(fd);
    else if (is_post && !strcmp(path, "/api/config"))  handle_config_post(fd, p);
    else if (is_post && !strcmp(path, "/api/dump"))    handle_dump(fd, p);
    else if (is_post && !strcmp(path, "/api/abort"))   handle_abort(fd);
    else if (is_post && !strcmp(path, "/api/quit"))    handle_quit(fd);
    else if (is_get  && !strcmp(path, "/api/icon"))    handle_icon(fd, p);
    else if (is_get  && !strcmp(path, "/api/library")) handle_library(fd);
    else if (is_get  && !strcmp(path, "/api/libicon")) handle_library_icon(fd, p);
    else if (is_post && !strcmp(path, "/api/launch"))  handle_launch(fd, p);
    else if (is_get  && !strcmp(path, "/api/size"))    handle_size(fd, p);
    else if (is_get  && !strcmp(path, "/api/browse"))  handle_browse(fd, p);
    else if (is_post && !strcmp(path, "/api/mkdir"))   handle_mkdir(fd, p);
    else send_error(fd, 404, "no such endpoint");
}

/* ------------------------------------------------------------------ */
/*  Connection handling                                                */
/* ------------------------------------------------------------------ */

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

    route(fd, method, path, &params);
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

/* ------------------------------------------------------------------ */
/*  Server lifecycle                                                   */
/* ------------------------------------------------------------------ */

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

int http_server_run(int port)
{
    /* a client that disappears mid-response must not take the payload down */
    signal(SIGPIPE, SIG_IGN);

    pthread_mutex_lock(&g_cfg_mtx);
    config_load(&g_cfg);
    pthread_mutex_unlock(&g_cfg_mtx);

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        printf_notification("Web UI: socket() failed (%s)", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    /* the homebrew launcher usually holds 8080, so walk a few ports up */
    int bound = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        addr.sin_port = htons((uint16_t)(port + attempt));
        if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            port += attempt;
            bound = 1;
            break;
        }
    }

    if (!bound) {
        printf_notification("Web UI: ports %d-%d are busy (%s)",
                            port, port + 9, strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    if (listen(g_listen_fd, 8) != 0) {
        printf_notification("Web UI: listen() failed (%s)", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    g_running = 1;
    g_port = port;

    char ip[64];
    local_ipv4(ip, sizeof(ip));
    printf_notification("PS5 App Dumper: open http://%s:%d", ip, port);
    write_log(g_log_path, "Web UI listening on http://%s:%d", ip, port);

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

    close(g_listen_fd);
    g_listen_fd = -1;

    /* If this shows up without the user asking for a shutdown, something
       outside the payload took the socket away. */
    printf_notification("PS5 App Dumper: web UI stopped");
    write_log(g_log_path, "Web UI: server loop ended");
    return 0;
}
