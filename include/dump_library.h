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

#ifndef DUMP_LIBRARY_H
#define DUMP_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

/* The dumps that exist, wherever they are: on the drives and on the console
   itself, in the configured dump folders or anywhere else within a few
   levels of a destination's root. And moving them about. */

#define DUMPLIB_MAX 96

typedef struct {
    char     mount[64];     /* the destination it is on, "/mnt/usb0" or "/data" */
    int      internal;
    char     dir[128];      /* the folder that holds it, relative to mount; "" = root */
    char     folder[64];    /* "PPSA25428-app0"                                  */
    char     title_id[16];
    char     title[128];    /* from the info file, else from the installed title */
    char     state[16];     /* "done", "running", "failed", "aborted"; "" when no
                               info file sits next to it (an older or foreign dump) */
    uint64_t bytes;         /* from the info file, 0 when not known              */
    int      in_use;        /* an installed title is redirected to this folder   */
    int      has_icon;      /* the dump carries its sce_sys/icon0.png            */
    int      fself;         /* 1: executables are FSELF, 0: plain, -1: not known  */
} dumplib_entry_t;

/* Finds them. with_internal = 0 leaves the console's storage out - its folder
   names are not for everybody on the network. Returns how many. */
int dumplib_scan(dumplib_entry_t *out, int max, int with_internal);

typedef struct {
    int renamed, unchanged, in_use, taken, failed;
} dumplib_rename_report_t;

/* Renames every game dump the library finds to the plain name, or to it with
   the game's title behind (dump_title_suffix) - folder and info file alike.
   DLC folders keep their names. A dump an installed title is redirected to
   (ShadowMount) is left alone, since the link names its path; so is one
   whose new name is taken already. with_internal as for dumplib_scan. */
void dumplib_rename_all(int with_titles, int with_internal, dumplib_rename_report_t *r);

/* The picture of a dump: <mount>/<dir>/<folder>/sce_sys/icon0.png, checked the
   way a move checks its source. Returns 0 and the path, -1 when there is none
   or the folder is not a dump. */
int dumplib_icon_path(const char *mount, const char *dir, const char *folder,
                      char *out, size_t out_size);

typedef enum { MOVE_IDLE = 0, MOVE_RUNNING, MOVE_DONE, MOVE_FAILED, MOVE_ABORTED } move_state_t;

typedef struct {
    move_state_t state;
    int      deleting;      /* this "move" takes the dump nowhere: it is being deleted */
    char     folder[64];
    char     from[256];     /* the folder that held it  */
    char     to[256];       /* the folder that gets it  */
    uint64_t total_bytes;   /* 0 for a move within one drive - that is a rename */
    uint64_t copied_bytes;
    char     message[192];
} move_status_t;

/* Moves <mount>/<dir>/<folder> to <to_mount>/<to_dir>/<folder>, its info file
   with it. Within one drive that is done when this returns; across drives it
   runs in the background - copy, compare, and only then delete the original.
   Returns 0 when done or started, -1 with a reason in err. */
int  dumplib_move(const char *mount, const char *dir, const char *folder,
                  const char *to_mount, const char *to_dir, char *err, size_t err_size);
/* Deletes <mount>/<dir>/<folder> and its info file, in the background - a
   dump is tens of thousands of files. Same checks as a move's source; confirm
   has to repeat the folder's name, so that no stray request deletes anything.
   Reported through the move status, with deleting set. */
int  dumplib_delete(const char *mount, const char *dir, const char *folder,
                    const char *confirm, char *err, size_t err_size);
/* Turns the decrypted executables of a finished dump into FSELF files in
   place - what enable_elf2fself would have done while dumping. Files that
   are not plain ELF (still encrypted, or FSELF already) are left alone, and
   so is the decrypted/ folder. Returns 0 with the counts, -1 with a reason. */
int  dumplib_fself(const char *mount, const char *dir, const char *folder,
                   int *converted, int *skipped, char *err, size_t err_size);
int  dumplib_move_active(void);
/* The way back, from the plain copies in decrypted/. */
int  dumplib_unfself(const char *mount, const char *dir, const char *folder,
                     int *restored, int *skipped, char *err, size_t err_size);
int  dumplib_fself_active(void);
void dumplib_move_cancel(void);
void dumplib_move_status(move_status_t *out);
/* Forgets the result of the last move. */
void dumplib_move_clear(void);

#endif /* DUMP_LIBRARY_H */
