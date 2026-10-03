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

#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>

/* log, notifications, storage and the file basics live in the core now */
#include "webhb.h"

int read_npwr_id(const char *npbind_path, char *npwr_out, size_t out_size);
int fs_copy_file(const char *src, const char *dst);
int copy_file_track(const char *src, const char *dst);
void copy_dir_recursive_tracked(const char *src, const char *dst);
void size_walker(const char *path, size_t *acc);
void *progress_status_func(void *arg);

extern size_t folder_size_current;
extern size_t total_bytes_copied;
extern char current_copied[256];
extern int progress_thread_run;
extern time_t copy_start_time;
extern int copy_directory(const char *src, const char *dst);
extern pthread_t progress_thread;

int  read_decrypter_config(void);
int  read_logging_config(void); 
int  read_elf2fself_config(void);
int  read_backport_config(void);
int  read_split_config(void);          // NEW: 0-3 split mode
const char* detect_fs_type(const char *mountpoint);
void debug_list_usbs(void);

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

/* What a dump is told. The values live in the core's config store
   (source/dumper_config.c registers them); this is a copy of them. */

typedef struct {
    int  enable_decrypter;
    int  enable_backport;
    int  ps4_backport_level;   /* 1-6  */
    int  ps5_backport_level;   /* 1-10 */
    int  enable_elf2fself;
    int  enable_logging;
    int  split;                /* 0-3, PS4 only */
    int  enable_webui;         /* 1 -> serve the web UI instead of dumping right away */
    int  web_port;
    int  auto_start;           /* 1 -> legacy behaviour: dump the running app and exit */
    char dump_subdir[64];      /* dump folder below a drive's mount point */
    char dump_subdir_console[64]; /* the same below /data, the console's own storage */
    int  queue_delay;          /* seconds a queued title gets to load before its dump */
    int  dump_dlc;             /* 1 -> the DLC mounted with a title go along with it   */
    int  folder_titles;        /* 1 -> the game's title behind its dump folders' names */
} dumper_config_t;

/* ------------------------------------------------------------------ */
/*  Cooperative abort, honoured by the copy routines                   */
/* ------------------------------------------------------------------ */

void request_abort(void);
void clear_abort(void);
int  abort_requested(void);

extern int g_split_mode;               // 0-3: split mode
/* what goes behind the dump folders' names, "_ASTROs_PLAYROOM" or "" -
   set per job like g_split_mode (dump_title_suffix, folder_titles) */
extern char g_folder_suffix[48];
/* Backport targets of the dump in progress. 0 leaves the choice to
   config.ini; the web UI sets them per job so a queue can dump each title
   with its own settings, and so they hold without a drive to save them on. */
extern int g_ps4_backport_level;
extern int g_ps5_backport_level;

#endif /* UTILS_H */