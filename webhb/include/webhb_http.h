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

#ifndef WEBHB_HTTP_H
#define WEBHB_HTTP_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  String builder - for putting JSON together                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    oom;
} sb_t;

void sb_init(sb_t *sb);
void sb_free(sb_t *sb);
void sb_putm(sb_t *sb, const char *data, size_t len);
void sb_puts(sb_t *sb, const char *s);
void sb_printf(sb_t *sb, const char *fmt, ...);
/* Appends s as a quoted JSON string. */
void sb_json_str(sb_t *sb, const char *s);

/* ------------------------------------------------------------------ */
/*  Request parameters - query string and form body alike              */
/* ------------------------------------------------------------------ */

/* a queue may bring one settings parameter per title on top of its own */
#define MAX_PARAMS 40

typedef struct {
    char key[32];
    char val[192];
} param_t;

typedef struct {
    param_t items[MAX_PARAMS];
    int     count;
} params_t;

const char *param_get(const params_t *p, const char *key, const char *fallback);
int         param_get_int(const params_t *p, const char *key, int fallback);

static inline int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------------------ */
/*  Responses                                                          */
/* ------------------------------------------------------------------ */

void send_response(int fd, int code, const char *content_type,
                   const void *body, size_t len, const char *extra_headers);
/* Same, with a Cache-Control of the caller's choosing instead of no-store. */
void send_response_cc(int fd, int code, const char *content_type,
                      const void *body, size_t len,
                      const char *cache_control, const char *extra_headers);
void send_json(int fd, int code, const char *json);
/* Sends what was built and frees the builder. */
void send_sb(int fd, int code, sb_t *sb);
void send_error(int fd, int code, const char *message);
/* Streams a file from disk. */
void send_file(int fd, const char *path, const char *content_type);

/* The live console's feed: log lines newer than since, as a JSON object. */
void json_log(sb_t *sb, unsigned since);

/* ------------------------------------------------------------------ */
/*  Routes and lifecycle                                               */
/* ------------------------------------------------------------------ */

typedef void (*http_handler_t)(int fd, const params_t *p);

/* Registers a handler for an exact method and path. Call before
   http_server_run(); the strings must outlive the server. */
void http_route(const char *method, const char *path, http_handler_t fn);

/* Exempts a POST path from the access check in the dispatcher - for the
   routes a locked-out browser needs to get in. */
void http_route_open(const char *path);

/* 1 when the request on fd came in over 127.0.0.1. */
int http_peer_is_local(int fd);

/* Called once, with the port the server ended up on. */
void http_on_listening(void (*fn)(int port));

/* Serves on the given TCP port, walking up to nine ports further when it is
   taken. Blocks until the server is asked to shut down. Returns 0 on a
   clean shutdown, -1 when the socket could not be opened. */
int http_server_run(int port);

/* Asks the accept loop to return. Safe to call from a request handler. */
void http_server_stop(void);

/* The port the server actually bound to, 0 before it is listening. */
int http_server_port(void);

#endif /* WEBHB_HTTP_H */
