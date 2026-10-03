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
/* <region>-<TITLEID>_00-<LABEL>-ac: a DLC, as pfsmnt names it */
int dump_is_dlc_folder_name(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len < 22 || len > 90) return 0;
    if (strcmp(name + len - 3, "-ac") != 0) return 0;
    if (name[6] != '-' || (strncmp(name + 7, "PPSA", 4) != 0 && strncmp(name + 7, "CUSA", 4) != 0)) return 0;
    if (strncmp(name + 16, "_00-", 4) != 0) return 0;
    for (size_t i = 0; i < len; i++)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '-' || name[i] == '_')) return 0;
    return 1;
}

/* what a title suffix is made of */
static int suffix_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || c == '(' || c == ')' || c == '&' || c == '+' ||
           c == '!' || c == '\'';
}

#define TITLE_SUFFIX_MAX 40

void dump_title_suffix(const char *title, char *out, size_t out_size)
{
    char buf[TITLE_SUFFIX_MAX + 1];
    size_t n = 0;
    const char *p = title ? title : "";

    for (; *p && n < TITLE_SUFFIX_MAX; p++) {
        char c = (*p == ' ') ? '_' : *p;
        if (!suffix_char(c)) continue;          /* colons, question marks, the TM sign, ... */
        if (c == '_' && (n == 0 || buf[n - 1] == '_')) continue;
        buf[n++] = c;
    }
    /* cut short: at the end of the last whole word, if there is one */
    if (*p && *p != ' ' && n == TITLE_SUFFIX_MAX) {
        size_t w = n;
        while (w && buf[w - 1] != '_') w--;
        if (w > TITLE_SUFFIX_MAX / 2) n = w;
    }
    while (n && (buf[n - 1] == '_' || buf[n - 1] == '.')) n--;
    buf[n] = '\0';

    if (out_size) snprintf(out, out_size, n ? "_%s" : "%s", buf);
}

/* The plain name inside a folder name: "PPSA01234-app0" for both
   "PPSA01234-app0" and "PPSA01234-app0_ASTROs_PLAYROOM". -1 when the name
   is no dump folder of ours. */
static int plain_name(const char *name, char *out, size_t out_size)
{
    if (!name || strlen(name) < 9) return -1;
    if (strncmp(name, "PPSA", 4) != 0 && strncmp(name, "CUSA", 4) != 0) return -1;

    for (int i = 4; i < 9; i++)
        if (name[i] < '0' || name[i] > '9') return -1;

    const char *rest = name + 9;
    size_t base = 9;
    if (strncmp(rest, "-app0", 5) == 0)        base += 5;
    else if (strncmp(rest, "-patch0", 7) == 0) base += 7;

    const char *tail = name + base;
    if (tail[0] && tail[0] != '_') return -1;
    for (const char *p = tail; *p; p++)
        if (!suffix_char(*p)) return -1;

    if (base >= out_size) return -1;
    memcpy(out, name, base);
    out[base] = '\0';
    return 0;
}

int dump_plain_name(const char *folder, char *out, size_t out_size)
{
    return plain_name(folder, out, out_size);
}

static int is_dump_folder_name(const char *name)
{
    if (dump_is_dlc_folder_name(name)) return 1;
    char plain[32];
    return plain_name(name, plain, sizeof(plain)) == 0;
}

/* The folders below dest that belong to the plain name, with a title behind
   it or without - whatever folder_titles was when they were dumped. */
static int folders_named(const char *dest, const char *plain, char out[][64], int max)
{
    DIR *d = opendir(dest);
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max) {
        char p[32];
        if (plain_name(ent->d_name, p, sizeof(p)) != 0 || strcmp(p, plain) != 0) continue;
        if (strlen(ent->d_name) >= 64) continue;
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/%s", dest, ent->d_name);
        if (!dir_exists(dir)) continue;
        snprintf(out[n++], 64, "%s", ent->d_name);
    }
    closedir(d);
    return n;
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
    for (int i = 0; i < count && info->suffix && info->suffix[0]; i++)
        strncat(folders[i], info->suffix, sizeof(folders[i]) - strlen(folders[i]) - 1);

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
      char found[4][64];
      int nf = folders_named(dest, folders[i], found, 4);
      for (int k = 0; k < nf; k++) {
        char info[512], state[16] = {0};
        info_path(dest, found[k], info, sizeof(info));

        /* A folder without an info file is not ours to judge: it may be a
           dump from an older version, or something else entirely. It
           counts as present and is never removed unasked. */
        if (info_read(info, "state", state, sizeof(state)) == 0 && strcmp(state, "done") != 0)
            return DUMP_INCOMPLETE;

        result = DUMP_PRESENT;
      }
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

    for (int i = 0; i < count; i++) {
        char found[4][64];
        int nf = folders_named(dest, folders[i], found, 4);
        for (int k = 0; k < nf; k++)
            if (remove_folder(dest, found[k]) != 0) rc = -1;
        /* a note whose folder is gone already */
        char info[512];
        info_path(dest, folders[i], info, sizeof(info));
        unlink(info);
    }

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

int dump_folder_name_ok(const char *name) { return is_dump_folder_name(name); }

int dump_info_string(const char *dest, const char *folder, const char *key, char *out, size_t out_size)
{
    char path[512];
    info_path(dest, folder, path, sizeof(path));
    return info_read(path, key, out, out_size);
}

int dump_info_int(const char *dest, const char *folder, const char *key, int *out)
{
    char path[512], buf[2048], needle[48];
    info_path(dest, folder, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    snprintf(needle, sizeof(needle), "\"%s\": ", key);
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    if (*p < '0' || *p > '9') return -1;
    *out = atoi(p);
    return 0;
}

int dump_info_set_string(const char *dest, const char *folder, const char *key, const char *value)
{
    char path[512], buf[2048], needle[48], tail[2048], esc[300];
    info_path(dest, folder, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    snprintf(needle, sizeof(needle), "\"%s\": \"", key);
    char *p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    char *rest = p;
    while (*rest && !(*rest == '"' && rest[-1] != '\\')) rest++;
    snprintf(tail, sizeof(tail), "%s", rest);
    json_escape(value, esc, sizeof(esc));
    snprintf(p, sizeof(buf) - (size_t)(p - buf), "%s%s", esc, tail);

    f = fopen(path, "w");
    if (!f) return -1;
    fputs(buf, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return 0;
}

int dump_info_set_int(const char *dest, const char *folder, const char *key, int value)
{
    char path[512], buf[2048], needle[48], tail[2048];
    info_path(dest, folder, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    snprintf(needle, sizeof(needle), "\"%s\": ", key);
    char *p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    char *rest = p;
    while (*rest >= '0' && *rest <= '9') rest++;
    snprintf(tail, sizeof(tail), "%s", rest);
    snprintf(p, sizeof(buf) - (size_t)(p - buf), "%d%s", value, tail);

    f = fopen(path, "w");
    if (!f) return -1;
    fputs(buf, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return 0;
}

int dump_remove_tree(const char *path)
{
    const char *name = path ? strrchr(path, '/') : NULL;
    if (!name || !is_dump_folder_name(name + 1)) return -1;
    return remove_tree(path);
}

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
