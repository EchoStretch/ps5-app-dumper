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

#ifndef DUMP_STORE_H
#define DUMP_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "utils.h"

/* What is known about the dumps on a drive.

   Every dump folder gets a small file next to it - "<folder>.dump-info.json"
   - saying which title it is, with which settings it was made and whether it
   was finished. Next to the folder and not inside it, so the dump itself
   stays exactly what came off the console. It is what tells a finished dump
   from one that was cut short, and the only thing that ever licenses this
   payload to delete a folder. */

#define DUMP_INFO_SUFFIX  ".dump-info.json"
#define DUMP_FOLDERS_MAX  2
#define DUMP_LIST_MAX     32

typedef enum {
    DUMP_ABSENT = 0,     /* nothing there                                    */
    DUMP_INCOMPLETE,     /* started by us and never finished                 */
    DUMP_PRESENT         /* finished - or a folder we know nothing about     */
} dump_presence_t;

typedef struct {
    char     folder[64];    /* "CUSA12125-app0"                              */
    char     title_id[16];
    char     title[128];
    char     state[16];     /* "running", "aborted", "failed"                */
} dump_entry_t;

typedef struct {
    const char            *title_id;
    const char            *title;
    const char            *state;     /* "running" | "done" | "failed" | "aborted" */
    const dumper_config_t *cfg;
    uint64_t               bytes;
    time_t                 started;
    time_t                 finished;
} dump_info_t;

/* The folders a dump of this title writes below its destination: one for a
   PS5 title, one or two for a PS4 title depending on the split mode.
   Returns how many. */
int dump_folders(const char *title_id, int is_ps4, int split,
                 char out[DUMP_FOLDERS_MAX][64]);

dump_presence_t dump_presence(const char *dest, const char *title_id,
                              int is_ps4, int split);

/* Writes the info file of every folder of this dump. */
void dump_info_write(const char *dest, int is_ps4, int split, const dump_info_t *info);

/* Removes what exists of this dump, info files included. Returns 0 when
   nothing of it is left. */
int dump_remove_title(const char *dest, const char *title_id, int is_ps4, int split);

/* Removes one folder - but only if its info file says it is an unfinished
   dump of ours. Returns 0 on success, -1 when it is not ours to delete. */
int dump_remove_incomplete(const char *dest, const char *folder);

/* Lists the unfinished dumps below dest. Returns how many. */
int dump_list_incomplete(const char *dest, dump_entry_t *out, int max);

/* For the dump library, which finds and moves dumps wherever they are: */

/* 1 for "PPSA01234", "PPSA01234-app0", "CUSA01234-patch0" - a dump folder. */
int dump_folder_name_ok(const char *name);
/* One string value from <dest>/<folder>.dump-info.json, 0 on success. */
int dump_info_string(const char *dest, const char *folder, const char *key, char *out, size_t out_size);
/* Removes a folder and all below it. Only ever to be called on a path that
   ends in a dump folder name. */
int dump_remove_tree(const char *path);

#endif /* DUMP_STORE_H */
