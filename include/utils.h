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

extern int decrypt_all(const char *src_game, const char *dst_game,
                       int do_elf2fself, int do_backport);

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
const char* get_usb_homebrew_path(void);

const char* detect_fs_type(const char *mountpoint);
void debug_list_usbs(void);

extern int g_enable_logging;
extern char g_log_path[512];
extern int g_split_mode;               // 0-3: split mode

#endif /* UTILS_H */