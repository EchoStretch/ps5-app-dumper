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

/* The settings: config.ini in the app's data folder, and /api/config for the
   page. The core does not know what an app wants to have set - the app
   registers its keys, and gets loading, range checks, the file with its
   comments and the two routes in return. A handful of keys the core acts on
   itself; webhb.h has them ready to be put into the app's table. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "webhb.h"

#define CFG_MAX_KEYS 32

/* the keys the core looks at itself */
#define CFG_LOGGING      "enable_logging"
#define CFG_WEB_PORT     "web_port"
#define CFG_REQUIRE_CODE "require_code"
#define CFG_ACCESS_CODE  "access_code"
#define CFG_ACCESS_TOKEN "access_token"

typedef struct {
    int  i;                     /* bool and int  */
    char s[WHB_CFG_STR_MAX];    /* string        */
} cfg_val_t;

static const whb_cfg_key_t *g_keys[CFG_MAX_KEYS];
static cfg_val_t            g_vals[CFG_MAX_KEYS];
static int                  g_count = 0;
static void               (*g_on_change)(void) = NULL;
/* settings were changed while there was nowhere to store them */
static int                  g_unsaved = 0;

/* recursive: the app reads its values from inside the change callback */
static pthread_mutex_t g_mtx;
static pthread_once_t  g_mtx_once = PTHREAD_ONCE_INIT;

static void mtx_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mtx, &attr);
    pthread_mutexattr_destroy(&attr);
}

void whb_config_lock(void)
{
    pthread_once(&g_mtx_once, mtx_init);
    pthread_mutex_lock(&g_mtx);
}

void whb_config_unlock(void)
{
    pthread_mutex_unlock(&g_mtx);
}

/* ------------------------------------------------------------------ */
/*  Keys and values                                                    */
/* ------------------------------------------------------------------ */

static size_t str_size(const whb_cfg_key_t *k)
{
    return (k->size && k->size < WHB_CFG_STR_MAX) ? k->size : WHB_CFG_STR_MAX;
}

static void val_default(const whb_cfg_key_t *k, cfg_val_t *v)
{
    memset(v, 0, sizeof(*v));
    if (k->type == WHB_CFG_STRING) {
        if (k->def_str) snprintf(v->s, str_size(k), "%s", k->def_str);
    } else if (!k->def && !strcmp(k->ini_name, CFG_WEB_PORT)) {
        v->i = whb_app()->default_port;
    } else {
        v->i = k->def;
    }
}

/* What a line of config.ini says. Nothing in the file is refused: a number
   out of range is pulled into it. */
static void val_parse(const whb_cfg_key_t *k, cfg_val_t *v, const char *text)
{
    switch (k->type) {
    case WHB_CFG_BOOL:   v->i = atoi(text) ? 1 : 0; break;
    case WHB_CFG_INT:    v->i = clamp(atoi(text), k->lo, k->hi); break;
    case WHB_CFG_STRING: snprintf(v->s, str_size(k), "%s", text); break;
    }
}

static int find_key(const char *ini_name)
{
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_keys[i]->ini_name, ini_name)) return i;
    return -1;
}

void whb_config_register(const whb_cfg_key_t *keys, int count)
{
    whb_config_lock();
    for (int i = 0; i < count && g_count < CFG_MAX_KEYS; i++) {
        if (!keys[i].ini_name || find_key(keys[i].ini_name) >= 0) continue;
        g_keys[g_count] = &keys[i];
        val_default(&keys[i], &g_vals[g_count]);
        g_count++;
    }
    whb_config_unlock();
}

void whb_config_on_change(void (*fn)(void))
{
    g_on_change = fn;
}

int whb_config_int(const char *ini_name, int fallback)
{
    whb_config_lock();
    int k = find_key(ini_name);
    int v = (k >= 0 && g_keys[k]->type != WHB_CFG_STRING) ? g_vals[k].i : fallback;
    whb_config_unlock();
    return v;
}

void whb_config_str(const char *ini_name, char *out, size_t out_size)
{
    if (!out || !out_size) return;

    whb_config_lock();
    int k = find_key(ini_name);
    snprintf(out, out_size, "%s", (k >= 0 && g_keys[k]->type == WHB_CFG_STRING) ? g_vals[k].s : "");
    whb_config_unlock();
}

/* ------------------------------------------------------------------ */
/*  config.ini                                                         */
/* ------------------------------------------------------------------ */

/* Splits "  key = value  ; comment" into key/value, both trimmed.
   Returns 0 when the line carries a setting. */
static int split_line(char *line, char **key, char **value)
{
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ';' || *p == '#' || *p == '\n' || *p == '\r' || *p == '\0') return -1;

    char *eq = strchr(p, '=');
    if (!eq) return -1;
    *eq = '\0';

    char *k_end = eq - 1;
    while (k_end >= p && (*k_end == ' ' || *k_end == '\t')) *k_end-- = '\0';

    char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;

    char *v_end = v + strlen(v) - 1;
    while (v_end >= v && (*v_end == '\n' || *v_end == '\r' ||
                          *v_end == ' '  || *v_end == '\t')) *v_end-- = '\0';

    /* strip a trailing inline comment */
    char *c = strpbrk(v, ";#");
    if (c) {
        *c = '\0';
        char *e = c - 1;
        while (e >= v && (*e == ' ' || *e == '\t')) *e-- = '\0';
    }

    *key = p;
    *value = v;
    return 0;
}

static int ini_path(char *out, size_t out_size)
{
    if (get_app_data_path()[0] == '\0') { out[0] = '\0'; return -1; }
    snprintf(out, out_size, "%s/config.ini", get_app_data_path());
    return 0;
}

/* Defaults first, then whatever the file says. Call with the lock held. */
static void load(void)
{
    for (int i = 0; i < g_count; i++) val_default(g_keys[i], &g_vals[i]);

    char path[512];
    if (ini_path(path, sizeof(path)) != 0) return;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256], *key, *val;
    while (fgets(line, sizeof(line), f)) {
        if (split_line(line, &key, &val) != 0) continue;
        int k = find_key(key);
        if (k >= 0) val_parse(g_keys[k], &g_vals[k], val);
    }
    fclose(f);
}

static int write_ini(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f,
        "; %s Config\n"
        "; Managed by the web UI - hand edits are picked up on the next start.\n",
        whb_app()->name);

    for (int i = 0; i < g_count; i++) {
        const whb_cfg_key_t *k = g_keys[i];
        if (k->comment) fprintf(f, "\n%s", k->comment);

        if (k->type == WHB_CFG_STRING) {
            const char *s = g_vals[i].s;
            if (!s[0] && k->if_empty) s = k->if_empty;
            fprintf(f, "%s = %s\n", k->ini_name, s);
        } else {
            fprintf(f, "%s = %d\n", k->ini_name, g_vals[i].i);
        }
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return 0;
}

static int save(void)
{
    char path[512];
    if (ini_path(path, sizeof(path)) != 0) return -1;
    return write_ini(path);
}

/* ------------------------------------------------------------------ */
/*  What the core does with its own keys                               */
/* ------------------------------------------------------------------ */

/* Call with the lock held, whenever values may have changed. */
static void apply(void)
{
    int k = find_key(CFG_LOGGING);
    if (k >= 0) g_enable_logging = g_vals[k].i;

    if (g_on_change) g_on_change();
}

static void access_values(const char **code, const char **token, int *required)
{
    int c = find_key(CFG_ACCESS_CODE), t = find_key(CFG_ACCESS_TOKEN), r = find_key(CFG_REQUIRE_CODE);
    *code     = c >= 0 ? g_vals[c].s : NULL;
    *token    = t >= 0 ? g_vals[t].s : NULL;
    *required = r >= 0 ? g_vals[r].i : 1;
}

/* Hands the stored code and token to the access check, and takes back what
   it had to make up. Call with the lock held, after every load(). */
static void access_adopt(void)
{
    const char *code, *token;
    int required;
    access_values(&code, &token, &required);

    if (!whb_access_init(code, token, required)) return;

    int c = find_key(CFG_ACCESS_CODE), t = find_key(CFG_ACCESS_TOKEN);
    if (c < 0 || t < 0) return;   /* an app that stores nothing: new with every start */

    whb_access_get(g_vals[c].s, str_size(g_keys[c]), g_vals[t].s, str_size(g_keys[t]));
    if (save() != 0) g_unsaved = 1;
}

void whb_config_init(void)
{
    whb_config_lock();
    storage_refresh();
    load();

    /* a first start leaves a file behind that can be edited on a PC */
    char path[512];
    if (ini_path(path, sizeof(path)) == 0 && !file_exists(path)) save();

    apply();
    whb_config_unlock();
}

void whb_config_storage_changed(void)
{
    whb_config_lock();
    if (g_unsaved) {
        /* carry this session's changes onto the drive that just appeared */
        if (save() == 0) g_unsaved = 0;
    } else {
        load();
        access_adopt();
    }
    apply();
    whb_config_unlock();
}

/* ------------------------------------------------------------------ */
/*  /api/config                                                        */
/* ------------------------------------------------------------------ */

static void json_values(sb_t *sb, const cfg_val_t *vals)
{
    int shown = 0;
    sb_puts(sb, "{");
    for (int i = 0; i < g_count; i++) {
        const whb_cfg_key_t *k = g_keys[i];
        if ((k->flags & WHB_CFG_SECRET) || !k->web_name) continue;

        sb_printf(sb, "%s\"%s\":", shown++ ? "," : "", k->web_name);
        if (k->type == WHB_CFG_STRING) sb_json_str(sb, vals[i].s);
        else                           sb_printf(sb, "%d", vals[i].i);
    }
    sb_puts(sb, "}");
}

void whb_config_json(sb_t *sb)
{
    whb_config_lock();
    json_values(sb, g_vals);
    whb_config_unlock();
}

static void handle_config_get(int fd, const params_t *p)
{
    (void)p;
    sb_t sb;
    sb_init(&sb);
    whb_config_json(&sb);
    send_sb(fd, 200, &sb);
}

/* Takes one key's value out of a request. Returns 0, or the HTTP status to
   refuse the whole request with and why. */
static int val_from_request(int fd, const params_t *p, const whb_cfg_key_t *k,
                            cfg_val_t *v, const char **why)
{
    const char *given = param_get(p, k->web_name, NULL);

    /* Whether other devices need the code is for the console's own browser to
       say: a phone that got in must not be able to leave the door open. */
    if ((k->flags & WHB_CFG_LOCAL) && given && !http_peer_is_local(fd)) {
        *why = "this can only be changed in the console's own browser";
        return 403;
    }

    switch (k->type) {
    case WHB_CFG_BOOL:
        v->i = param_get_int(p, k->web_name, v->i) ? 1 : 0;
        break;
    case WHB_CFG_INT:
        v->i = clamp(param_get_int(p, k->web_name, v->i), k->lo, k->hi);
        break;
    case WHB_CFG_STRING:
        if (!given) break;
        if (k->flags & WHB_CFG_SUBDIR) {
            /* typed by hand: a slash at the end means nothing */
            char subdir[WHB_CFG_STR_MAX];
            snprintf(subdir, str_size(k), "%s", given);
            for (size_t n = strlen(subdir); n && subdir[n - 1] == '/'; n--) subdir[n - 1] = '\0';

            /* a destination folder must stay below the mount point */
            if (given[0] == '/' || !fs_path_is_safe(subdir)) {
                *why = "invalid destination folder";
                return 400;
            }
            snprintf(v->s, str_size(k), "%s", subdir);
        } else {
            snprintf(v->s, str_size(k), "%s", given);
        }
        break;
    }
    return 0;
}

static void handle_config_post(int fd, const params_t *p)
{
    whb_config_lock();

    /* all of it or nothing: a refused value leaves the others alone too */
    cfg_val_t vals[CFG_MAX_KEYS];
    memcpy(vals, g_vals, sizeof(vals));

    for (int i = 0; i < g_count; i++) {
        const whb_cfg_key_t *k = g_keys[i];
        if ((k->flags & WHB_CFG_SECRET) || !k->web_name) continue;

        const char *why = NULL;
        int refused = val_from_request(fd, p, k, &vals[i], &why);
        if (refused) {
            whb_config_unlock();
            send_error(fd, refused, why);
            return;
        }
    }

    memcpy(g_vals, vals, sizeof(vals));
    apply();

    const char *code, *token;
    int required;
    access_values(&code, &token, &required);
    whb_access_init(code, token, required);

    int saved = save();
    g_unsaved = (saved == 0) ? 0 : 1;

    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"saved\":%s,\"config\":", saved == 0 ? "true" : "false");
    json_values(&sb, g_vals);
    whb_config_unlock();

    sb_puts(&sb, ",\"note\":");
    sb_json_str(&sb, saved == 0
                ? "settings written to config.ini"
                : "no drive connected - settings stay active until the payload restarts, "
                  "and are written as soon as a drive shows up");
    sb_puts(&sb, "}");
    send_sb(fd, 200, &sb);
}

/* Puts a copy of the settings on the console, for the day the drive that
   carries them is not plugged in. While it is, the drive's own still win. */
static void handle_config_to_console(int fd, const params_t *p)
{
    (void)p;
    char dir[160], path[200];
    storage_internal_data_dir(dir, sizeof(dir));
    mkdirs(dir);
    snprintf(path, sizeof(path), "%s/config.ini", dir);

    whb_config_lock();
    int written = write_ini(path);
    whb_config_unlock();

    if (written != 0) {
        send_error(fd, 500, "could not write the settings to the console");
        return;
    }
    write_log(g_log_path, "Settings copied to %s", path);

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"saved\":true,\"path\":");
    sb_json_str(&sb, path);
    sb_puts(&sb, "}");
    send_sb(fd, 200, &sb);
}

void whb_config_routes_init(void)
{
    whb_config_lock();
    access_adopt();
    whb_config_unlock();

    http_route("GET",  "/api/config",         handle_config_get);
    http_route("POST", "/api/config",         handle_config_post);
    http_route("POST", "/api/config/console", handle_config_to_console);
}
