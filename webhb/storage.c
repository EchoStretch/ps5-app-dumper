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

/* Where a payload keeps its own files, and the few file system basics the
   rest of the core needs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

#include "webhb.h"

static char g_usb_homebrew[128] = {0};   /* <drive>/homebrew: where a headless dump goes */
static char g_app_data[128] = {0};      /* <root>/homebrew/<data_dirname>: settings, logs */

int dir_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

int file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

void mkdirs(const char *path)
{
    if (!path || !*path) return;

    char tmp[512];
    strncpy(tmp, path, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* ------------------------------------------------------------------ */
/*  Where our own files live                                           */
/* ------------------------------------------------------------------ */

/* <root>/homebrew/<data_dirname>, the usual place for a homebrew's files, on
   a USB drive or on the console itself. A drive wins when it carries a
   config.ini: that one travels with the stick and can be edited on a PC.
   Without such a drive the console's own storage is used, so settings, the
   access code and logs no longer depend on something being plugged in. */
#ifndef INTERNAL_ROOT          /* the host harness points this at a scratch folder */
#define INTERNAL_ROOT "/data"
#endif

static const char *const g_usb_mounts[] = {
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
    "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"
};
#define USB_MOUNTS ((int)(sizeof(g_usb_mounts) / sizeof(g_usb_mounts[0])))

static int g_data_on_usb = -1;   /* index into g_usb_mounts, -1: not on a drive */

const char *storage_internal_root(void) { return INTERNAL_ROOT; }

static void data_dir_of(const char *root, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/homebrew/%s", root, whb_app()->data_dirname);
}

static int can_write_in(const char *dir)
{
    char probe[256];
    snprintf(probe, sizeof(probe), "%s/.probe", dir);

    int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return 0;
    int ok = (write(fd, "PROBE", 5) == 5);
    close(fd);
    unlink(probe);
    return ok;
}

/* Moves a file an older version kept elsewhere on the same drive into our
   folder. A rename within one drive; on failure the old file simply stays
   where it is. */
static void adopt_legacy(const char *legacy_dir, const char *name, const char *appdir)
{
    char from[256], to[256];
    snprintf(from, sizeof(from), "%s/%s", legacy_dir, name);
    snprintf(to,   sizeof(to),   "%s/%s", appdir, name);

    struct stat st;
    if (stat(to, &st) == 0 || stat(from, &st) != 0) return;
    if (rename(from, to) == 0)
        write_log(g_log_path, "Moved %s into %s", from, appdir);
}

/* Two older layouts: everything in <drive>/homebrew (up to v1.11), then a
   folder of our own at the top of the drive. */
static void adopt_older_layouts(const char *root, const char *appdir)
{
    char top[128], homebrew[128];
    snprintf(top,      sizeof(top),      "%s/%s", root, whb_app()->data_dirname);
    snprintf(homebrew, sizeof(homebrew), "%s/homebrew", root);

    /* only a drive that has something to move gets our folder */
    char probe[256];
    int found = 0;
    const char *places[] = { top, homebrew };
    for (int k = 0; k < 2 && !found; k++) {
        snprintf(probe, sizeof(probe), "%s/config.ini", places[k]);
        if (file_exists(probe)) found = 1;
        snprintf(probe, sizeof(probe), "%s/disc_titles.txt", places[k]);
        if (file_exists(probe)) found = 1;
    }
    if (!found) return;
    mkdirs(appdir);

    adopt_legacy(top, "config.ini", appdir);
    adopt_legacy(top, "disc_titles.txt", appdir);
    adopt_legacy(top, "logs", appdir);
    rmdir(top);   /* goes only when nothing else was in it */

    adopt_legacy(homebrew, "config.ini", appdir);
    adopt_legacy(homebrew, "disc_titles.txt", appdir);
}

static void use_data_dir(const char *appdir, int usb_index, int writable)
{
    snprintf(g_app_data, sizeof(g_app_data), "%s", appdir);
    g_data_on_usb = usb_index;

    if (!writable) { g_log_path[0] = '\0'; return; }   /* nowhere to write a log to */

    char logs[192];
    snprintf(logs, sizeof(logs), "%s/logs", appdir);
    mkdirs(logs);
    log_use_general();
}

/* Which USB drive brings settings of its own, -1 for none. ro_dir gets the
   folder to read them from when the drive cannot be written to. */
static int usb_with_config(char *appdir, size_t appdir_size, int *writable)
{
    for (int i = 0; i < USB_MOUNTS; i++) {
        const char *root = g_usb_mounts[i];
        if (!dir_exists(root)) continue;

        char dir[128], config[256];
        data_dir_of(root, dir, sizeof(dir));
        adopt_older_layouts(root, dir);

        snprintf(config, sizeof(config), "%s/config.ini", dir);
        if (file_exists(config)) {
            snprintf(appdir, appdir_size, "%s", dir);
            *writable = 1;   /* as far as known; checked when it is taken up */
            return i;
        }

        /* a drive we cannot write to can still hand us its settings, from
           wherever an older version left them */
        const char *old[2]; char top[128], homebrew[128];
        snprintf(top,      sizeof(top),      "%s/%s", root, whb_app()->data_dirname);
        snprintf(homebrew, sizeof(homebrew), "%s/homebrew", root);
        old[0] = top; old[1] = homebrew;
        for (int k = 0; k < 2; k++) {
            snprintf(config, sizeof(config), "%s/config.ini", old[k]);
            if (!file_exists(config)) continue;
            snprintf(appdir, appdir_size, "%s", old[k]);
            *writable = 0;
            return i;
        }
    }
    return -1;
}

/* Decides where our files live right now. Returns 1 when that is somewhere
   else than before - the settings then want reading again. */
int storage_refresh(void)
{
    char before[sizeof(g_app_data)];
    snprintf(before, sizeof(before), "%s", g_app_data);

    /* the first drive found stays the home of headless dumps */
    g_usb_homebrew[0] = '\0';
    for (int i = 0; i < USB_MOUNTS && !g_usb_homebrew[0]; i++)
        if (dir_exists(g_usb_mounts[i]))
            snprintf(g_usb_homebrew, sizeof(g_usb_homebrew), "%s/homebrew", g_usb_mounts[i]);

    char appdir[128];
    int writable = 0;
    int usb = usb_with_config(appdir, sizeof(appdir), &writable);

    if (usb >= 0) {
        if (strcmp(appdir, before) != 0) {
            if (writable) writable = can_write_in(appdir);
            use_data_dir(appdir, usb, writable);
            if (!writable) printf_notification("USB (read-only fallback): %s", g_usb_mounts[usb]);
            write_log(g_log_path, "Settings and logs: %s (the drive's own config.ini wins)", appdir);
        }
        return strcmp(g_app_data, before) != 0;
    }

    /* no drive with settings: the console itself */
    data_dir_of(INTERNAL_ROOT, appdir, sizeof(appdir));
    if (strcmp(appdir, before) == 0) return 0;   /* polled: no probe writes */
    mkdirs(appdir);
    if (can_write_in(appdir)) {
        if (strcmp(appdir, before) != 0) {
            use_data_dir(appdir, -1, 1);
            write_log(g_log_path, "Settings and logs: %s", appdir);
        }
        return strcmp(g_app_data, before) != 0;
    }

    /* no usable internal storage either: the first drive we can write to */
    for (int i = 0; i < USB_MOUNTS; i++) {
        if (!dir_exists(g_usb_mounts[i])) continue;
        data_dir_of(g_usb_mounts[i], appdir, sizeof(appdir));
        mkdirs(appdir);
        if (!can_write_in(appdir)) continue;
        if (strcmp(appdir, before) != 0) use_data_dir(appdir, i, 1);
        return strcmp(g_app_data, before) != 0;
    }

    g_app_data[0] = '\0';
    g_data_on_usb = -1;
    return before[0] != '\0';
}

int storage_is_internal(void)
{
    return g_app_data[0] && g_data_on_usb < 0;
}

const char* get_app_data_path(void) {
    return g_app_data;
}

const char* get_usb_homebrew_path(void) {
    return g_usb_homebrew;
}

/* The index of the first USB drive that is plugged in, -1 for none. */
int storage_first_usb(void)
{
    for (int i = 0; i < USB_MOUNTS; i++)
        if (dir_exists(g_usb_mounts[i])) return i;
    return -1;
}

/* <internal root>/homebrew/<data_dirname>, whether or not it is in use. */
void storage_internal_data_dir(char *out, size_t out_size)
{
    data_dir_of(INTERNAL_ROOT, out, out_size);
}
