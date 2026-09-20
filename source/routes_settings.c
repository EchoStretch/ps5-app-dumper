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

/* The folder picker that chooses where dumps go. It is not about dumping, but
   still stands on the dumper's drive detection; it follows the rest into
   webhb/ once that has moved (docs/webhb-plan.md). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "routes.h"
#include "app_scan.h"
#include "utils.h"

/* Resolves the drive to browse: the caller-named mount if it is one of ours,
   otherwise the drive holding config.ini. Returns NULL when none is usable. */
static const char *browse_mount(const params_t *p)
{
    const char *mount = param_get(p, "mount", NULL);
    if (mount && *mount && target_is_known(mount) == 0) return mount;
    return NULL;
}

/* What is on a stick is no secret to the network it is plugged into; what is
   on the console is only shown to those who may change things anyway. */
static int may_browse(int fd, const params_t *p, const char *mount)
{
    if (strcmp(mount, storage_internal_root()) != 0) return 1;
    return http_peer_is_local(fd) || whb_access_token_ok(param_get(p, "token", NULL));
}

static void handle_browse(int fd, const params_t *p)
{
    const char *mount = browse_mount(p);
    if (!mount) { send_error(fd, 400, "unknown drive"); return; }
    if (!may_browse(fd, p, mount)) { send_error(fd, 401, "enter the code shown on the TV first"); return; }

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
    http_route("GET",  "/api/browse",           handle_browse);
    http_route("POST", "/api/mkdir",            handle_mkdir);
}
