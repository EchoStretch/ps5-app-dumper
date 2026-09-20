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

/* Full PS5 notification struct */
typedef struct {
    int type;                //0x00
    int req_id;              //0x04
    int priority;            //0x08
    int msg_id;              //0x0C
    int target_id;           //0x10
    int user_id;             //0x14
    int unk1;                //0x18
    int unk2;                //0x1C
    int app_id;              //0x20
    int error_num;           //0x24
    int unk3;                //0x28
    char use_icon_image_uri; //0x2C
    char message[1024];      //0x2D
    char uri[1024];          //0x42D
    char unkstr[1024];       //0x82D
} SceNotificationRequest;   //Size = 0xC30

int dir_exists(const char *path);
int file_exists(const char *path);
void mkdirs(const char *path);
int write_log(const char *log_file_path, const char *fmt, ...);
void printf_notification(const char *fmt, ...);
/* Same toast on the console, but kept out of the web UI's live console. */
void printf_notification_quiet(const char *fmt, ...);
int sceKernelSendNotificationRequest(int device, SceNotificationRequest *req, size_t size, int blocking);

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

int  find_usb_and_setup(void);
int  read_decrypter_config(void);
int  read_logging_config(void); 
int  read_elf2fself_config(void);
int  read_backport_config(void);
int  read_split_config(void);          // NEW: 0-3 split mode
const char* get_usb_homebrew_path(void);   /* <drive>/homebrew - where a headless dump goes */

/* <drive>/<the app's data_dirname>: config.ini, the disc list and logs/.
   Empty until a drive turned up. Files older versions kept in
   <drive>/homebrew are moved over the first time. */
const char* get_app_data_path(void);

/* Which file write_log() goes to: the general one, or one per dump. */
void log_use_general(void);
void log_use_dump(const char *title_id);

const char* detect_fs_type(const char *mountpoint);
void debug_list_usbs(void);

/* ------------------------------------------------------------------ */
/*  In-memory log ring (feeds the live console of the web UI)          */
/* ------------------------------------------------------------------ */

#define LOG_RING_CAPACITY 400
#define LOG_LINE_MAX      320

typedef void (*log_line_cb)(void *ctx, unsigned seq, const char *line);

void     log_ring_push(const char *line);
void     log_ring_walk(unsigned since, log_line_cb cb, void *ctx);
unsigned log_ring_seq(void);

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

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
    char dump_subdir[64];      /* folder below the mount point, e.g. "homebrew" */
    int  queue_delay;          /* seconds a queued title gets to load before its dump */
    /* access control of the web UI (webhb/access.c); never served over HTTP
       with the rest of the settings */
    int  require_code;         /* 0 -> anyone on the network may change things */
    char access_code[8];
    char access_token[40];
} dumper_config_t;

void config_defaults(dumper_config_t *cfg);
void config_load(dumper_config_t *cfg);
int  config_save(const dumper_config_t *cfg);
int  config_path(char *out, size_t out_size);

/* ------------------------------------------------------------------ */
/*  Cooperative abort, honoured by the copy routines                   */
/* ------------------------------------------------------------------ */

void request_abort(void);
void clear_abort(void);
int  abort_requested(void);

extern int g_enable_logging;
extern char g_log_path[512];
extern int g_split_mode;               // 0-3: split mode
/* Backport targets of the dump in progress. 0 leaves the choice to
   config.ini; the web UI sets them per job so a queue can dump each title
   with its own settings, and so they hold without a drive to save them on. */
extern int g_ps4_backport_level;
extern int g_ps5_backport_level;

#endif /* UTILS_H */