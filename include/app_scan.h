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

#ifndef APP_SCAN_H
#define APP_SCAN_H

#include <stddef.h>
#include <stdint.h>

/* target_scan() and its target_entry_t live in the core now */
#include "webhb.h"

#define SANDBOX_PATH    "/mnt/sandbox/pfsmnt"
#define APP_SCAN_MAX    24

/* One mounted application, as exposed by pfsmnt while the title runs. */
typedef struct {
    char dir[128];        /* mount folder, e.g. "PPSA01234-app0"        */
    char patch_dir[128];  /* "CUSA01234-patch0", empty when there is none */
    char title_id[16];    /* "PPSA01234"                                */
    char title[128];      /* human readable name, empty when unknown    */
    char version[24];     /* content/app version, empty when unknown    */
    int  is_ps4;          /* 1 for CUSA titles                          */
    int  has_icon;
    int  on_disc;         /* served by the disc in the drive            */
    int  is_disc;         /* a disc game, inserted or not               */
} app_entry_t;

/* Fills out with up to max entries, returns the number found. */
int app_scan(app_entry_t *out, int max);

/* Looks up a single app by its pfsmnt folder name. Returns 0 on success. */
int app_find(const char *dir, app_entry_t *out);

/* Resolves the icon of an app. Returns 0 and fills out on success. */
int app_icon_path(const app_entry_t *app, char *out, size_t out_size);

/* Total size of the app payload in pfsmnt, patch folder included. */
uint64_t app_size(const app_entry_t *app);

/* One installed title, whether or not it is currently running. */
typedef struct {
    char title_id[16];
    char title[128];
    char version[24];
    char source[24];      /* "internal", "ext0", ...            */
    int  is_ps4;
    int  has_icon;
    int  has_pic;         /* wide key art is available          */
    int  is_running;      /* already mounted under pfsmnt       */
    int  on_disc;         /* served by the disc in the drive    */
    int  is_disc;         /* a disc game, inserted or not       */
    /* Set when the title is redirected to a folder instead of its package
       (ShadowMount's mount.lnk next to app.pkg): the folder it runs from.
       Such a title has nothing to dump while the redirect is in place. */
    char mounted_from[160];
    /* How much of the package is on the console, 0-100; 100 only when all
       of it is. -1 when that cannot be told. A title can be started long
       before it is complete - a dump of it would have holes. */
    int  installed_pct;
} library_entry_t;

#define LIBRARY_SCAN_MAX 128

/* 1 when the disc in the drive carries this title. */
int title_on_disc(const char *title_id);

/* 1 when the title is a disc game, whether or not its disc is in the drive.
   With the disc out that is known from having seen it before (remembered
   next to config.ini) or from the disc-copy bitmap the install leaves
   behind - a best guess, which the queue lets the user overrule. */
int title_is_disc_game(const char *title_id);

/* Records a title as a disc game, e.g. because the user said so. */
void title_remember_disc(const char *title_id);

/* Lists the titles installed on the console. */
int library_scan(library_entry_t *out, int max);

/* Looks up a single installed title by its id. Returns 0 on success. */
int library_find(const char *title_id, library_entry_t *out);

/* Resolves the icon of an installed title. Returns 0 on success. */
int library_icon_path(const char *title_id, char *out, size_t out_size);

/* Resolves the wide key art of an installed title (pic0/pic1). Returns 0 on
   success; many titles ship none. */
int library_pic_path(const char *title_id, char *out, size_t out_size);

/* A title can run without its package: ShadowMount redirects a title to a
   dump it found (seen: a mount.lnk next to app.pkg, /system_ex/app/<id>
   null-mounted from <usb>/homebrew/<id>-app0). The game then gets its
   /mnt/sandbox/<id>_000/app0 but nothing under pfsmnt - so there is nothing
   to dump, and waiting for a mount would never end. 1 when that is the case. */
int title_runs_from_folder(const char *title_id);

/* Removes a title's mount.lnk when it still points at target - the folder a
   dump has just been moved away from. The title then starts from its package
   again; ShadowMount writes a new link if it finds the dump elsewhere.
   Returns 1 when a link was removed. */
int title_drop_mount_link(const char *title_id, const char *target);

/* See library_entry_t.installed_pct. */
int title_installed_percent(const char *title_id);

#endif /* APP_SCAN_H */
