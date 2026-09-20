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
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "dump_store.h"
#include "version.h"

/* ------------------------------------------------------------------ */
/*  Names                                                              */
/* ------------------------------------------------------------------ */

/* "PPSA01234", optionally followed by "-app0" or "-patch0" - the only kind
   of name this module will ever build a path to delete from. */
static int is_dump_folder_name(const char *name)
{
    if (!name || strlen(name) < 9) return 0;
    if (strncmp(name, "PPSA", 4) != 0 && strncmp(name, "CUSA", 4) != 0) return 0;

    for (int i = 4; i < 9; i++)
        if (name[i] < '0' || name[i] > '9') return 0;

    const char *rest = name + 9;
    return rest[0] == '\0' || strcmp(rest, "-app0") == 0 || strcmp(rest, "-patch0") == 0;
}

int dump_folders(const char *title_id, int is_ps4, int split,
                 char out[DUMP_FOLDERS_MAX][64])
{
    int n = 0;

    if (!is_ps4) {
        snprintf(out[n++], 64, "%s-app0", title_id);
    } else if (split == 0) {
        snprintf(out[n++], 64, "%s", title_id);
    } else {
        if (split & 1) snprintf(out[n++], 64, "%s-app0", title_id);
        if (split & 2) snprintf(out[n++], 64, "%s-patch0", title_id);
    }

    return n;
}

static void info_path(const char *dest, const char *folder, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s" DUMP_INFO_SUFFIX, dest, folder);
}

/* ------------------------------------------------------------------ */
/*  Info files                                                         */
/* ------------------------------------------------------------------ */

/* Pulls one string value out of an info file; they are written by us and
   flat, so a scan is all it takes. Returns 0 on success. */
static int info_read(const char *path, const char *key, char *out, size_t out_size)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\": \"", key);

    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 2 < out_size; p++) {
        if (*p == '"' || *p == '\\') out[o++] = '\\';
        out[o++] = (*p < 0x20) ? ' ' : (char)*p;
    }
    out[o] = '\0';
}

void dump_info_write(const char *dest, int is_ps4, int split, const dump_info_t *info)
{
    char folders[DUMP_FOLDERS_MAX][64];
    int count = dump_folders(info->title_id, is_ps4, split, folders);

    char title[300];
    json_escape(info->title, title, sizeof(title));

    for (int i = 0; i < count; i++) {
        char path[512], dir[512];
        info_path(dest, folders[i], path, sizeof(path));
        snprintf(dir, sizeof(dir), "%s/%s", dest, folders[i]);

        /* A folder the dump never created - a PS4 title without a patch,
           say - needs no note once the dump is over. While it runs the
           folders do not exist yet, so then the note is written regardless. */
        if (strcmp(info->state, "running") != 0 && !dir_exists(dir)) {
            unlink(path);
            continue;
        }

        FILE *f = fopen(path, "w");
        if (!f) continue;

        fprintf(f,
            "{\n"
            "  \"tool\": \"ps5-app-dumper\",\n"
            "  \"version\": \"" DUMPER_VERSION "\",\n"
            "  \"titleId\": \"%s\",\n"
            "  \"title\": \"%s\",\n"
            "  \"folder\": \"%s\",\n"
            "  \"state\": \"%s\",\n"
            "  \"started\": %lld,\n"
            "  \"finished\": %lld,\n"
            "  \"bytes\": %llu,\n"
            "  \"settings\": { \"decrypt\": %d, \"fself\": %d, \"backport\": %d, "
            "\"ps4BackportLevel\": %d, \"ps5BackportLevel\": %d, \"split\": %d }\n"
            "}\n",
            info->title_id, title, folders[i], info->state,
            (long long)info->started, (long long)info->finished,
            (unsigned long long)info->bytes,
            info->cfg->enable_decrypter, info->cfg->enable_elf2fself, info->cfg->enable_backport,
            info->cfg->ps4_backport_level, info->cfg->ps5_backport_level, info->cfg->split);

        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/*  Presence                                                           */
/* ------------------------------------------------------------------ */

dump_presence_t dump_presence(const char *dest, const char *title_id,
                              int is_ps4, int split)
{
    char folders[DUMP_FOLDERS_MAX][64];
    int count = dump_folders(title_id, is_ps4, split, folders);

    dump_presence_t result = DUMP_ABSENT;

    for (int i = 0; i < count; i++) {
        char dir[512], info[512], state[16] = {0};
        snprintf(dir, sizeof(dir), "%s/%s", dest, folders[i]);
        if (!dir_exists(dir)) continue;

        info_path(dest, folders[i], info, sizeof(info));

        /* A folder without an info file is not ours to judge: it may be a
           dump from an older version, or something else entirely. It
           counts as present and is never removed unasked. */
        if (info_read(info, "state", state, sizeof(state)) == 0 && strcmp(state, "done") != 0)
            return DUMP_INCOMPLETE;

        result = DUMP_PRESENT;
    }

    return result;
}

/* ------------------------------------------------------------------ */
/*  Removing                                                           */
/* ------------------------------------------------------------------ */

static int remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path);

    struct dirent *ent;
    int rc = 0;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);

        struct stat st;
        if (lstat(child, &st) != 0) { rc = -1; continue; }

        if (S_ISDIR(st.st_mode)) { if (remove_tree(child) != 0) rc = -1; }
        else if (unlink(child) != 0) rc = -1;
    }

    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

static int remove_folder(const char *dest, const char *folder)
{
    /* belt and braces: every path built here ends in a title folder name */
    if (!dest || !dest[0] || !is_dump_folder_name(folder)) return -1;

    char dir[512], info[512];
    snprintf(dir, sizeof(dir), "%s/%s", dest, folder);
    info_path(dest, folder, info, sizeof(info));

    int rc = dir_exists(dir) ? remove_tree(dir) : 0;
    unlink(info);
    return rc;
}

int dump_remove_title(const char *dest, const char *title_id, int is_ps4, int split)
{
    char folders[DUMP_FOLDERS_MAX][64];
    int count = dump_folders(title_id, is_ps4, split, folders);
    int rc = 0;

    for (int i = 0; i < count; i++)
        if (remove_folder(dest, folders[i]) != 0) rc = -1;

    return rc;
}

int dump_remove_incomplete(const char *dest, const char *folder)
{
    if (!is_dump_folder_name(folder)) return -1;

    char info[512], tool[32] = {0}, state[16] = {0};
    info_path(dest, folder, info, sizeof(info));

    if (info_read(info, "tool", tool, sizeof(tool)) != 0 || strcmp(tool, "ps5-app-dumper") != 0) return -1;
    if (info_read(info, "state", state, sizeof(state)) != 0 || strcmp(state, "done") == 0) return -1;

    return remove_folder(dest, folder);
}

/* ------------------------------------------------------------------ */
/*  Listing                                                            */
/* ------------------------------------------------------------------ */

int dump_list_incomplete(const char *dest, dump_entry_t *out, int max)
{
    DIR *d = opendir(dest);
    if (!d) return 0;

    const size_t suffix_len = strlen(DUMP_INFO_SUFFIX);
    struct dirent *ent;
    int count = 0;

    while ((ent = readdir(d)) != NULL && count < max) {
        size_t len = strlen(ent->d_name);
        if (len <= suffix_len || strcmp(ent->d_name + len - suffix_len, DUMP_INFO_SUFFIX) != 0) continue;

        char folder[64];
        size_t flen = len - suffix_len;
        if (flen >= sizeof(folder)) continue;
        memcpy(folder, ent->d_name, flen);
        folder[flen] = '\0';
        if (!is_dump_folder_name(folder)) continue;

        char info[512], dir[512];
        snprintf(info, sizeof(info), "%s/%s", dest, ent->d_name);
        snprintf(dir, sizeof(dir), "%s/%s", dest, folder);

        /* the folder was removed by hand: the note about it goes too */
        if (!dir_exists(dir)) { unlink(info); continue; }

        dump_entry_t *e = &out[count];
        memset(e, 0, sizeof(*e));
        if (info_read(info, "state", e->state, sizeof(e->state)) != 0) continue;
        if (strcmp(e->state, "done") == 0) continue;

        snprintf(e->folder, sizeof(e->folder), "%s", folder);
        info_read(info, "titleId", e->title_id, sizeof(e->title_id));
        info_read(info, "title", e->title, sizeof(e->title));
        count++;
    }

    closedir(d);
    return count;
}
