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

#ifndef WEBHB_H
#define WEBHB_H

#include <stddef.h>
#include <time.h>

#include "webhb_http.h"

/* ------------------------------------------------------------------ */
/*  The app, as far as the core needs to know it                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;            /* "PS5 App Dumper" - notifications, web manifest  */
    const char *short_name;      /* "App Dumper" - what fits under an icon: the
                                    home-screen tile and a phone's home screen      */
    const char *version;         /* "1.12"                                          */
    /* Tools that list payloads, pldmgr among them, only count processes whose
       name ends in ".elf"; the kernel keeps 19 characters of it. */
    const char *process_name;    /* "ps5-app-dumper.elf"                            */
    const char *data_dirname;    /* "ps5-app-dumper" -> <drive>/ps5-app-dumper      */
    /* Payload managers show no version of their own for an uploaded file -
       pldmgr reads it out of the file name ("_v1.12"), so the stored name is
       <elf_basename>_v<version>.elf. */
    const char *elf_basename;    /* "ps5-app-dumper"                                */
    const char *tile_title_id;   /* "APDU00001", NULL for an app without a tile     */
    int         default_port;    /* first TCP port tried for the web UI             */

    const unsigned char *page;      size_t page_len;   /* the bin2c'd page          */
    const unsigned char *icon_png;  size_t icon_len;   /* 512x512: tile and favicon */

    /* 1 while work is going on that must not be interrupted. Quitting, a
       takeover by the next copy and the tile install all ask this instead of
       knowing what the app does. NULL means never busy. */
    int (*busy)(void);
} whb_app_t;

/* Tells the core who it is working for. Call first thing in main(); app
   must outlive the server. */
void whb_app_set(const whb_app_t *app);

/* What whb_app_set() was given. Never NULL. */
const whb_app_t *whb_app(void);

int whb_busy(void);

/* The file name a copy of this payload is stored under, version included. */
const char *whb_elf_name(void);

/* Registers the routes every app gets: the page and its cache manifest, the
   icons, the web manifest, /api/self*, /api/tile and /api/quit. Call before
   http_server_run(). */
void whb_routes_init(void);

/* ------------------------------------------------------------------ */
/*  Access control - see access.c                                      */
/* ------------------------------------------------------------------ */

/* Takes the stored code (6 digits) and token (32 hex digits); whatever is
   missing or malformed is made up, once per session. required = 0 switches
   the check off. Returns 1 when something was made up and wants storing -
   fetch it with whb_access_get(). */
int  whb_access_init(const char *code, const char *token, int required);
void whb_access_get(char *code, size_t code_size, char *token, size_t token_size);
int  whb_access_required(void);
int  whb_access_token_ok(const char *token);
/* The start notification: where to find the web UI, and the code. */
void whb_access_notify(const char *ip, int port);
void whb_access_routes_init(void);

/* ------------------------------------------------------------------ */
/*  One running copy                                                   */
/* ------------------------------------------------------------------ */

/* Names the process, so it can be found - by others and by the next copy of
   this payload. */
void instance_claim_name(void);

/* Makes this the only running copy: an idle older instance is asked to
   quit, and ended by force if it does not. web_port is where the search
   for its web UI starts.

   A copy that is busy is never touched. Returns 0 when the way is free, and
   -1 with the port of the busy instance in busy_port when this copy should
   step aside instead. */
int instance_take_over(int web_port, int *busy_port);

/* ------------------------------------------------------------------ */
/*  A place in Payload Manager                                         */
/* ------------------------------------------------------------------ */

/* A payload sent over the network exists in memory only: there is no file
   to hand to a payload manager. So the build embeds a copy of the ELF - the
   same program, built one step earlier without this blob. A copy started
   from that stored file has nothing embedded, and needs nothing: it is
   stored already. Generated by the Makefile; empty in a plain build. */
extern const unsigned char self_elf[];
extern const size_t        self_elf_len;

/* 1 when this copy carries an ELF it can store. */
int self_store_available(void);

/* Tells whether a stored file is this very build: 1 when it equals the
   embedded ELF or contains it (the file a release ships does - it is this
   build one stage later), 0 when it is some other build, -1 when it cannot
   be read or there is nothing to compare with. path must name a
   whb_elf_name() inside a payload manager's storage. */
int self_store_matches(const char *path);

/* Uploads the embedded ELF to pldmgr on this console under whb_elf_name(),
   replacing a file of that name. Returns 0 on success, -1 with a reason a
   user can act on in err. */
int self_store_to_pldmgr(char *err, size_t err_size);

/* ------------------------------------------------------------------ */
/*  Home-screen tile                                                   */
/* ------------------------------------------------------------------ */

/* Installs or refreshes a home-screen tile that opens the web UI on the
   given port in the console browser. The tile is a browser deeplink: it
   does not start this payload.

   Only ever called because the user asked for it. The system libraries it
   needs are loaded at that moment and never linked, so a console that
   takes the install badly cannot keep the payload from starting.

   Returns 0 when the tile is installed and current. Otherwise returns -1
   and puts a reason a user can act on into err. */
int tile_install(int port, char *err, size_t err_size);

/* 1 when a tile pointing at this port is already in place. Touches the
   file system only, so it is safe to call from a polled endpoint. */
int tile_is_current(int port);

/* 1 when a tile is on the console at all, current or not. Together with
   tile_is_current() that tells a tile left behind by an older build - or
   pointing at another port - from no tile. */
int tile_exists(void);

/* ------------------------------------------------------------------ */
/*  Folder picker backend                                              */
/* ------------------------------------------------------------------ */

#define FS_BROWSE_MAX   256   /* directories returned per listing */
#define FS_NAME_MAX     256   /* one directory name               */

/* A relative path is safe to join below a mount when it has no leading
   slash, no "." or ".." segment, no backslash, and fits the config field.
   An empty string is safe and means "the mount root". Returns 1 / 0. */
int fs_path_is_safe(const char *rel);

/* A single folder name is safe when it is non-empty, holds no slash, no
   backslash, and is not "." or "..". Returns 1 / 0. */
int fs_name_is_safe(const char *name);

/* Lists the sub-directories of <mount>/<rel>, sorted, names only (no "."
   or ".."). Returns the count, or -1 when the path is unsafe or cannot be
   opened. rel may be "" for the mount root. */
int fs_list_dirs(const char *mount, const char *rel,
                 char out[][FS_NAME_MAX], int max);

/* Creates <mount>/<rel>/<name>. Both rel and name are validated. On success
   writes the new path relative to the mount into out_rel and returns 0;
   returns -1 on validation failure or if the directory cannot be created. */
int fs_make_subdir(const char *mount, const char *rel, const char *name,
                   char *out_rel, size_t out_size);

/* ------------------------------------------------------------------ */
/*  File basics                                                        */
/* ------------------------------------------------------------------ */

int dir_exists(const char *path);
int file_exists(const char *path);
void mkdirs(const char *path);
int write_log(const char *log_file_path, const char *fmt, ...);
void printf_notification(const char *fmt, ...);
/* Same toast on the console, but kept out of the web UI's live console. */
void printf_notification_quiet(const char *fmt, ...);


/* ------------------------------------------------------------------ */
/*  Notifications                                                      */
/* ------------------------------------------------------------------ */

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

int sceKernelSendNotificationRequest(int device, SceNotificationRequest *req, size_t size, int blocking);

/* ------------------------------------------------------------------ */
/*  Where the payload's own files live - see storage.c                 */
/* ------------------------------------------------------------------ */

const char* get_usb_homebrew_path(void);   /* <drive>/homebrew - where a headless dump goes */

/* <root>/homebrew/<the app's data_dirname>: config.ini, the disc list and
   logs/. On the USB drive that carries a config.ini, otherwise on the console
   itself (/data). Files older versions kept elsewhere on a drive are moved
   over the first time. */
const char* get_app_data_path(void);

/* Looks again where our files live - a drive may have come or gone. Returns
   1 when the place changed, and the settings want reading again. */
int storage_refresh(void);
/* 1 when settings and logs are on the console rather than on a drive. */
int storage_is_internal(void);
/* "/data": the console's own storage, offered as a dump destination too. */
const char *storage_internal_root(void);

/* Which file write_log() goes to: the general one, or one per dump. */
void log_use_general(void);
void log_use_dump(const char *title_id);

/* The index of the first USB drive that is plugged in, -1 for none. */
int  storage_first_usb(void);
/* <internal root>/homebrew/<data_dirname>, whether or not it is in use. */
void storage_internal_data_dir(char *out, size_t out_size);

extern int  g_enable_logging;
extern char g_log_path[512];

/* ------------------------------------------------------------------ */
/*  In-memory log ring (feeds the live console of the web UI)          */
/* ------------------------------------------------------------------ */

#define LOG_RING_CAPACITY 400
#define LOG_LINE_MAX      320

typedef void (*log_line_cb)(void *ctx, unsigned seq, const char *line);

void     log_ring_push(const char *line);
void     log_ring_walk(unsigned since, log_line_cb cb, void *ctx);
unsigned log_ring_seq(void);


#endif /* WEBHB_H */
