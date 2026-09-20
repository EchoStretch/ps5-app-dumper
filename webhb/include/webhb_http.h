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
/*  A request, as a handler sees it                                    */
/* ------------------------------------------------------------------ */

/* Method, path, parameters and the connection. Handlers get it from the
   server and hand it back to whatever they call here; what is inside is the
   server's business. */
typedef struct whb_req whb_req_t;

/* Parameters come from the query string and from a form body alike. */
#define WHB_PARAM_MAX 192   /* the longest value, its NUL included */
const char *whb_param(const whb_req_t *req, const char *key, const char *fallback);
int         whb_param_int(const whb_req_t *req, const char *key, int fallback);

/* 1 when the request came in over 127.0.0.1 - the console's own browser. */
int whb_peer_is_local(const whb_req_t *req);

static inline int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------------------ */
/*  Responses                                                          */
/* ------------------------------------------------------------------ */

void whb_send(whb_req_t *req, int code, const char *content_type,
              const void *body, size_t len, const char *extra_headers);
/* Same, with a Cache-Control of the caller's choosing instead of no-store. */
void whb_send_cc(whb_req_t *req, int code, const char *content_type,
                 const void *body, size_t len,
                 const char *cache_control, const char *extra_headers);
void whb_send_json(whb_req_t *req, int code, const char *json);
/* Sends what was built and frees the builder. */
void whb_send_sb(whb_req_t *req, int code, sb_t *sb);
void whb_send_error(whb_req_t *req, int code, const char *message);
/* Streams a file from disk. */
void whb_send_file(whb_req_t *req, const char *path, const char *content_type);

/* The live console's feed: log lines newer than since, as a JSON object. */
void json_log(sb_t *sb, unsigned since);

/* ------------------------------------------------------------------ */
/*  Routes and lifecycle                                               */
/* ------------------------------------------------------------------ */

typedef void (*whb_handler_t)(whb_req_t *req);

/* Registers a handler for an exact method and path. Call before
   whb_serve(); the strings must outlive the server. */
void whb_route(const char *method, const char *path, whb_handler_t fn);

/* Exempts a POST path from the access check in the dispatcher - for the
   routes a locked-out browser needs to get in. */
void whb_route_open(const char *path);

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
