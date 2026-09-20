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

#ifndef WEBHB_H
#define WEBHB_H

#include <stddef.h>
#include <stdint.h>
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
    const char *busy_with;       /* "a dump" - what that work is, for messages      */

    /* Called by whb_start() once the process has its name, before anything
       else happens - for what has to be set up first. May be NULL. */
    void (*on_start)(void);
} whb_app_t;

/* Tells the core who it is working for; app must outlive the server.
   whb_start() calls it. */
void whb_app_set(const whb_app_t *app);

/* What whb_app_set() was given. Never NULL. */
const whb_app_t *whb_app(void);

int whb_busy(void);

/* The file name a copy of this payload is stored under, version included. */
const char *whb_elf_name(void);

/* The start on the console: tells the core who it is working for, names the
   process, says hello, reads the settings and replaces an idle older copy.
   Register the app's settings first. Returns 0 when the way is free, and 1
   when a busy copy stays as it is and this one should end. */
int whb_start(const whb_app_t *app);

/* Registers the built-in routes and serves until asked to shut down - on
   the given port, or on the one from the settings when port is 0. Register
   the app's own routes first. Returns what http_server_run() returns. */
int whb_serve(int port);

/* Registers the routes every app gets: the page and its cache manifest, the
   icons, the web manifest, /api/status (unless the app brought its own),
   /api/self*, /api/tile, /api/quit, the settings
   (/api/config*), the folder picker (/api/browse, /api/mkdir) and the access
   check. Call after whb_config_init() and before http_server_run(). */
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
/*  Settings - see config.c                                            */
/* ------------------------------------------------------------------ */

typedef enum { WHB_CFG_BOOL, WHB_CFG_INT, WHB_CFG_STRING } whb_cfg_type_t;

#define WHB_CFG_SECRET  0x1   /* never served, never taken from a request       */
#define WHB_CFG_LOCAL   0x2   /* only the console's own browser may change it   */
#define WHB_CFG_SUBDIR  0x4   /* a folder below a mount point: stays relative,
                                 no "..", slashes at the end are dropped        */

#define WHB_CFG_STR_MAX 64    /* the longest string value, its NUL included     */

typedef struct {
    const char    *ini_name;  /* "queue_delay" - the line in config.ini         */
    const char    *web_name;  /* "queueDelay" - the name in /api/config         */
    whb_cfg_type_t type;
    unsigned       flags;
    int            lo, hi;    /* WHB_CFG_INT: what is outside is pulled inside  */
    int            def;       /* default of a bool or an int                    */
    const char    *def_str;   /* default of a string                            */
    size_t         size;      /* string: bytes it may take, 0 = WHB_CFG_STR_MAX */
    const char    *if_empty;  /* string: what config.ini gets for an empty one  */
    /* Lines put in front of the key in config.ini, each starting with ';'
       and ending in '\n'. A blank line goes before them, so the first key of
       a group carries the group's heading. */
    const char    *comment;
} whb_cfg_key_t;

/* The keys the core acts on by itself. An app puts the ones it wants into
   its own table, where it wants them: the table's order is the order of
   config.ini and of /api/config. Left out, logging stays on, the web UI
   takes the app's default_port and the access code is new with every start. */
#define WHB_CFG_STD_LOGGING \
    { .ini_name = "enable_logging", .web_name = "enableLogging", .type = WHB_CFG_BOOL, .def = 1, \
      .comment = "; === Logging ===\n" \
                 "; enable_logging = 1 -> write log.txt (default)\n" \
                 "; enable_logging = 0 -> disable logging\n" }
#define WHB_CFG_STD_WEB_PORT \
    { .ini_name = "web_port", .web_name = "webPort", .type = WHB_CFG_INT, .lo = 1024, .hi = 65535, \
      .def = 0 /* the app's default_port */, \
      .comment = "; web_port -> first TCP port tried for the web interface\n" }
#define WHB_CFG_STD_ACCESS \
    { .ini_name = "require_code", .web_name = "requireCode", .type = WHB_CFG_BOOL, .def = 1, \
      .flags = WHB_CFG_LOCAL, \
      .comment = "; === Access ===\n" \
                 "; Phones and PCs have to enter access_code once before they may change anything;\n" \
                 "; the console's own browser never has to. access_token is what they keep afterwards -\n" \
                 "; delete both lines to lock every device out again and get a new code.\n" \
                 "; require_code = 0 -> anyone on the network may use the web UI\n" }, \
    { .ini_name = "access_code",  .type = WHB_CFG_STRING, .flags = WHB_CFG_SECRET, .size = 8 }, \
    { .ini_name = "access_token", .type = WHB_CFG_STRING, .flags = WHB_CFG_SECRET, .size = 40 }

/* Adds keys to the store. The table must outlive the server; a name that is
   registered already is skipped. Call before whb_config_init(). */
void whb_config_register(const whb_cfg_key_t *keys, int count);

/* Called whenever values may have changed - after a load and after the page
   stored new ones - with the store locked. */
void whb_config_on_change(void (*fn)(void));

/* Finds the data folder, reads config.ini and writes one with the defaults
   where there is none yet. */
void whb_config_init(void);

/* The data folder moved (storage_refresh() returned 1): settings changed
   while there was nowhere to store them are written to the new place,
   otherwise its own are taken over. */
void whb_config_storage_changed(void);

int  whb_config_int(const char *ini_name, int fallback);
void whb_config_str(const char *ini_name, char *out, size_t out_size);
/* For reading several values that belong together. Recursive. */
void whb_config_lock(void);
void whb_config_unlock(void);

/* The settings as /api/config serves them. */
void whb_config_json(sb_t *sb);

/* Hands the stored access code to the access check and registers
   /api/config and /api/config/console. whb_routes_init() calls it. */
void whb_config_routes_init(void);

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
/*  Drives - see drives.c                                              */
/* ------------------------------------------------------------------ */

#define TARGET_SCAN_MAX 16

/* A mount point files can be written to. */
typedef struct {
    char     mount[64];   /* "/mnt/usb0"                */
    char     fs[24];      /* "exfatfs", "ufs", ...      */
    int      writable;
    int      internal;    /* the console's own storage, not a drive */
    uint64_t total_bytes;
    uint64_t free_bytes;
} target_entry_t;

/* Fills out with up to max mount points, returns the number found. */
int target_scan(target_entry_t *out, int max);

/* Returns 0 when mount is one of the mount points target_scan() reports. */
int target_is_known(const char *mount);

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
