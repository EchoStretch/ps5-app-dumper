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

/* The settings and the folder picker that chooses where dumps go. Neither is
   about dumping, but both still stand on the dumper's config struct and its
   drive detection; they follow the rest into webhb/ once those have moved
   (steps 4 and 6 of docs/webhb-plan.md). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "routes.h"
#include "app_scan.h"
#include "dump_queue.h"
#include "utils.h"

static dumper_config_t  g_cfg;
static pthread_mutex_t  g_cfg_mtx = PTHREAD_MUTEX_INITIALIZER;
/* settings were changed while no drive was around to store them */
static int              g_cfg_unsaved = 0;

void cfg_snapshot(dumper_config_t *out)
{
    pthread_mutex_lock(&g_cfg_mtx);
    *out = g_cfg;
    pthread_mutex_unlock(&g_cfg_mtx);
}

void cfg_drive_appeared(void)
{
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

void json_config(sb_t *sb, const dumper_config_t *cfg);

void json_config(sb_t *sb, const dumper_config_t *cfg)
{
    sb_printf(sb,
        "{\"enableDecrypter\":%d,\"enableBackport\":%d,"
        "\"ps4BackportLevel\":%d,\"ps5BackportLevel\":%d,"
        "\"enableElf2fself\":%d,\"enableLogging\":%d,\"split\":%d,"
        "\"enableWebui\":%d,\"webPort\":%d,\"autoStart\":%d,"
        "\"queueDelay\":%d,\"dumpSubdir\":",
        cfg->enable_decrypter, cfg->enable_backport,
        cfg->ps4_backport_level, cfg->ps5_backport_level,
        cfg->enable_elf2fself, cfg->enable_logging, cfg->split,
        cfg->enable_webui, cfg->web_port, cfg->auto_start,
        cfg->queue_delay);
    sb_json_str(sb, cfg->dump_subdir);
    sb_puts(sb, "}");
}

static void handle_config_get(int fd, const params_t *p)
{
    (void)p;
    sb_t sb;
    sb_init(&sb);

    pthread_mutex_lock(&g_cfg_mtx);
    json_config(&sb, &g_cfg);
    pthread_mutex_unlock(&g_cfg_mtx);

    send_sb(fd, 200, &sb);
}

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
    cfg.queue_delay        = clamp(param_get_int(p, "queueDelay", cfg.queue_delay),
                                   QUEUE_SETTLE_MIN, QUEUE_SETTLE_MAX);

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

void routes_settings_init(void)
{
    pthread_mutex_lock(&g_cfg_mtx);
    config_load(&g_cfg);
    pthread_mutex_unlock(&g_cfg_mtx);

    http_route("GET",  "/api/config",           handle_config_get);
    http_route("POST", "/api/config",           handle_config_post);
    http_route("GET",  "/api/browse",           handle_browse);
    http_route("POST", "/api/mkdir",            handle_mkdir);
}
