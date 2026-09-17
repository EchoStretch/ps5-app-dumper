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
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "fs_browse.h"

/* The dump folder is stored in a 64-byte config field, so a browsed path
   must leave room for it plus a NUL. */
#define FS_REL_MAX  63

int fs_name_is_safe(const char *name)
{
    if (!name || !name[0]) return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;

    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') return 0;

    return strlen(name) <= FS_NAME_MAX - 1;
}

int fs_path_is_safe(const char *rel)
{
    if (!rel) return 0;
    if (!rel[0]) return 1;                 /* the mount root */
    if (rel[0] == '/') return 0;           /* must stay relative */
    size_t len = strlen(rel);
    if (len > FS_REL_MAX) return 0;
    if (strchr(rel, '\\')) return 0;
    if (strstr(rel, "//")) return 0;      /* no empty segment */
    if (rel[len - 1] == '/') return 0;    /* no trailing slash */

    /* every segment must be an ordinary name */
    char tmp[FS_REL_MAX + 1];
    strncpy(tmp, rel, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *seg = strtok(tmp, "/"); seg; seg = strtok(NULL, "/")) {
        if (!strcmp(seg, ".") || !strcmp(seg, "..")) return 0;
        if (!seg[0]) return 0;             /* empty segment, e.g. "a//b" */
    }
    return 1;
}

/* Joins mount + rel into full, guarding the buffer. Returns 0 on success. */
static int join(const char *mount, const char *rel, char *full, size_t size)
{
    int n = rel && rel[0]
          ? snprintf(full, size, "%s/%s", mount, rel)
          : snprintf(full, size, "%s", mount);
    return (n > 0 && (size_t)n < size) ? 0 : -1;
}

/* Folders the picker should not show: dotfiles, Windows recycle/$ folders,
   and the volume system folder. The dump never wants to live in these. */
static int is_hidden_dir(const char *name)
{
    if (name[0] == '.' || name[0] == '$') return 1;
    if (!strcasecmp(name, "System Volume Information")) return 1;
    if (!strcasecmp(name, "FOUND.000")) return 1;
    return 0;
}

static int name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

int fs_list_dirs(const char *mount, const char *rel,
                 char out[][FS_NAME_MAX], int max)
{
    if (!mount || !out || max <= 0) return -1;
    if (!fs_path_is_safe(rel)) return -1;

    char full[1024];
    if (join(mount, rel, full, sizeof(full)) != 0) return -1;

    DIR *d = opendir(full);
    if (!d) return -1;

    int count = 0;
    struct dirent *ent;
    char child[1200];

    while ((ent = readdir(d)) && count < max) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (strlen(ent->d_name) >= FS_NAME_MAX) continue;
        if (is_hidden_dir(ent->d_name)) continue;

        /* stat rather than trust d_type: some filesystems report DT_UNKNOWN */
        struct stat st;
        if (snprintf(child, sizeof(child), "%s/%s", full, ent->d_name) >= (int)sizeof(child))
            continue;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        strncpy(out[count], ent->d_name, FS_NAME_MAX - 1);
        out[count][FS_NAME_MAX - 1] = '\0';
        count++;
    }

    closedir(d);
    qsort(out, count, FS_NAME_MAX, name_cmp);
    return count;
}

int fs_make_subdir(const char *mount, const char *rel, const char *name,
                   char *out_rel, size_t out_size)
{
    if (!mount || !name || !out_rel || out_size == 0) return -1;
    if (!fs_path_is_safe(rel)) return -1;
    if (!fs_name_is_safe(name)) return -1;

    /* the resulting relative path must still fit the config field */
    int rel_len = (rel && rel[0])
                ? snprintf(out_rel, out_size, "%s/%s", rel, name)
                : snprintf(out_rel, out_size, "%s", name);
    if (rel_len <= 0 || (size_t)rel_len >= out_size || rel_len > FS_REL_MAX) {
        out_rel[0] = '\0';
        return -1;
    }

    char full[1024];
    if (join(mount, out_rel, full, sizeof(full)) != 0) { out_rel[0] = '\0'; return -1; }

    if (mkdir(full, 0777) != 0) {
        /* an existing folder is fine - the user just selected it */
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
            out_rel[0] = '\0';
            return -1;
        }
    }
    return 0;
}
