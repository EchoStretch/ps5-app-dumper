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

/* Who may change things.

   Everybody on the network may look; whoever wants to start, stop or delete
   something has to prove they can see the TV. The start notification shows a
   six-digit code, the page trades it for a token once and sends that along
   with every POST from then on. The console's own browser comes in over
   127.0.0.1 and is trusted as it is - so is the next copy of this payload
   asking the running one to quit.

   The code is for people, the token is what actually opens the door: six
   digits could be guessed in an afternoon, so guessing is slowed down to a
   handful of tries a minute. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "webhb.h"

#define CODE_LEN        6
#define TOKEN_LEN       32
/* wrong codes in a row before guessing is put on hold, and for how long */
#define GUESS_LIMIT     5
#define GUESS_PAUSE     60
/* the least time between two "show the code" notifications */
#define SHOW_PAUSE      10

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int    g_required = 1;
static char   g_code[CODE_LEN + 1];
static char   g_token[TOKEN_LEN + 1];
static int    g_wrong;
static time_t g_hold_until;
static time_t g_shown_at;

static int all_of(const char *s, size_t len, const char *set)
{
    return s && strlen(s) == len && strspn(s, set) == len;
}

int whb_access_init(const char *code, const char *token, int required)
{
    int made_up = 0;

    pthread_mutex_lock(&g_mtx);
    g_required = required ? 1 : 0;

    /* What is stored wins, so the code a user knows stays good. Without a
       stored one - no drive yet, or a drive that never saw this payload -
       this session's is kept rather than replaced by yet another. */
    if (all_of(code, CODE_LEN, "0123456789")) {
        memcpy(g_code, code, CODE_LEN + 1);
    } else {
        if (!g_code[0])
            snprintf(g_code, sizeof(g_code), "%06u", (unsigned)arc4random_uniform(1000000));
        made_up = 1;
    }

    if (all_of(token, TOKEN_LEN, "0123456789abcdef")) {
        memcpy(g_token, token, TOKEN_LEN + 1);
    } else {
        if (!g_token[0]) {
            unsigned char raw[TOKEN_LEN / 2];
            arc4random_buf(raw, sizeof(raw));
            for (size_t i = 0; i < sizeof(raw); i++)
                snprintf(g_token + 2 * i, 3, "%02x", raw[i]);
        }
        made_up = 1;
    }

    pthread_mutex_unlock(&g_mtx);
    return made_up;
}

void whb_access_get(char *code, size_t code_size, char *token, size_t token_size)
{
    pthread_mutex_lock(&g_mtx);
    if (code && code_size)   snprintf(code, code_size, "%s", g_code);
    if (token && token_size) snprintf(token, token_size, "%s", g_token);
    pthread_mutex_unlock(&g_mtx);
}

/* compares all of it, however early the first difference comes */
static int same_secret(const char *given, const char *secret)
{
    size_t n = strlen(secret);
    if (!given || !n || strlen(given) != n) return 0;

    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(given[i] ^ secret[i]);
    return diff == 0;
}

int whb_access_token_ok(const char *token)
{
    pthread_mutex_lock(&g_mtx);
    int ok = !g_required || same_secret(token, g_token);
    pthread_mutex_unlock(&g_mtx);
    return ok;
}

int whb_access_required(void)
{
    pthread_mutex_lock(&g_mtx);
    int required = g_required;
    pthread_mutex_unlock(&g_mtx);
    return required;
}

void whb_access_notify(const char *ip, int port)
{
    char code[CODE_LEN + 1];
    whb_access_get(code, sizeof(code), NULL, 0);

    /* quiet: the live console is readable without the code */
    if (whb_access_required())
        printf_notification_quiet("%s: open http://%s:%d\nCode for phones and PCs: %s",
                                  whb_app()->name, ip, port, code);
    else
        printf_notification_quiet("%s: open http://%s:%d", whb_app()->name, ip, port);
}

/* ------------------------------------------------------------------ */
/*  Routes                                                             */
/* ------------------------------------------------------------------ */

/* Where this browser stands. The code itself is only told to those who
   could change it anyway. */
static void handle_access(int fd, const params_t *p)
{
    int local    = http_peer_is_local(fd);
    int trusted  = local || !whb_access_required();
    int unlocked = trusted || whb_access_token_ok(param_get(p, "token", NULL));

    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"required\":%s,\"local\":%s,\"trusted\":%s,\"unlocked\":%s",
              whb_access_required() ? "true" : "false", local ? "true" : "false",
              trusted ? "true" : "false", unlocked ? "true" : "false");
    if (unlocked && whb_access_required()) {
        char code[CODE_LEN + 1];
        whb_access_get(code, sizeof(code), NULL, 0);
        sb_puts(&sb, ",\"code\":");
        sb_json_str(&sb, code);
    }
    sb_puts(&sb, "}");
    send_sb(fd, 200, &sb);
}

static void handle_unlock(int fd, const params_t *p)
{
    const char *given = param_get(p, "code", "");
    time_t now = time(NULL);

    pthread_mutex_lock(&g_mtx);
    if (now < g_hold_until) {
        int wait = (int)(g_hold_until - now);
        pthread_mutex_unlock(&g_mtx);

        char msg[96];
        snprintf(msg, sizeof(msg), "too many wrong codes - try again in %d s", wait);
        send_error(fd, 429, msg);
        return;
    }

    int ok = same_secret(given, g_code);
    if (ok) {
        g_wrong = 0;
    } else if (++g_wrong >= GUESS_LIMIT) {
        g_wrong = 0;
        g_hold_until = now + GUESS_PAUSE;
    }

    char token[TOKEN_LEN + 1];
    snprintf(token, sizeof(token), "%s", g_token);
    pthread_mutex_unlock(&g_mtx);

    if (!ok) {
        write_log(g_log_path, "Web UI: a wrong code was entered");
        send_error(fd, 401, "wrong code");
        return;
    }

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"token\":");
    sb_json_str(&sb, token);
    sb_puts(&sb, "}");
    send_sb(fd, 200, &sb);
}

/* For a browser that is locked out: puts the code on the TV again. Open to
   everybody by necessity, so it is kept from being used to flood the screen. */
static void handle_show_code(int fd, const params_t *p)
{
    (void)p;
    time_t now = time(NULL);

    pthread_mutex_lock(&g_mtx);
    int too_soon = (now - g_shown_at) < SHOW_PAUSE;
    if (!too_soon) g_shown_at = now;
    pthread_mutex_unlock(&g_mtx);

    if (!too_soon) {
        char code[CODE_LEN + 1];
        whb_access_get(code, sizeof(code), NULL, 0);
        printf_notification_quiet("%s\nCode: %s", whb_app()->name, code);
    }
    send_json(fd, 200, "{\"shown\":true}");
}

void whb_access_routes_init(void)
{
    /* an app that stores nothing still gets a code, new with every start */
    if (!g_code[0]) whb_access_init(NULL, NULL, 1);

    http_route("GET",  "/api/access",      handle_access);
    http_route("POST", "/api/unlock",      handle_unlock);
    http_route("POST", "/api/access/show", handle_show_code);
    http_route_open("/api/unlock");
    http_route_open("/api/access/show");
}
