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

#include "pfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>

#include "ps4_dumper.h"
#include "pkg.h"
#include "utils.h"

extern int decrypt_all(const char *src_game, const char *dst_game,
                       int do_elf2fself, int do_backport);

/* ----------------------------------------------------------------- */
/*  Progress callback for unpfs() – updates utils.c globals          */
/* ----------------------------------------------------------------- */
static void pfs_progress(uint64_t copied, uint64_t total, const char *current_file)
{
    total_bytes_copied = copied;
    folder_size_current = total;
    if (current_file) {
        strncpy(current_copied, current_file, sizeof(current_copied) - 1);
        current_copied[sizeof(current_copied) - 1] = '\0';
    }
}

/* ----------------------------------------------------------------- */
/*  Helper: extract title ID from folder name                        */
/* ----------------------------------------------------------------- */
static void extract_title_from_folder(const char *folder, char *out_title, size_t out_size)
{
    if (!folder || !out_title || out_size == 0) return;
    size_t i = 0;
    while (folder[i] && folder[i] != '-' && i + 1 < out_size) {
        out_title[i] = folder[i];
        i++;
    }
    out_title[i] = '\0';
}

/* ----------------------------------------------------------------- */
/*  Helper: check if path contains '/'                               */
/* ----------------------------------------------------------------- */
static int is_path_like(const char *s)
{
    return s && strchr(s, '/') != NULL;
}

/* ----------------------------------------------------------------- */
/*  Helper: copy meta files                                          */
/* ----------------------------------------------------------------- */
static void copy_meta_file(const char *tmpl, const char *title_id,
                           const char *dst_base, const char *logpath)
{
    char src[1024], dst[1024];
    snprintf(src, sizeof(src), tmpl, title_id);
    const char *fname = strrchr(tmpl, '/');
    fname = fname ? fname + 1 : tmpl;
    snprintf(dst, sizeof(dst), "%s/sce_sys/%s", dst_base, fname);

    if (fs_copy_file(src, dst) == 0) {
        write_log(logpath, "Copied meta: %s", dst);
    } else {
        write_log(logpath, "Warning: Failed to copy %s", src);
    }
}

/* ----------------------------------------------------------------- */
/*  Extract PFS image with progress                                  */
/* ----------------------------------------------------------------- */
static int extract_pfs_image(const char *pfs_path, const char *dst_dir,
                             const char *type, const char *logpath)
{
    if (!file_exists(pfs_path)) {
        write_log(logpath, "Info: No %s pfs_image.dat found: %s", type, pfs_path);
        return -1;
    }

    printf_notification("Extracting %s filesystem...", type);

    // Reset progress state
    folder_size_current = 0;
    total_bytes_copied = 0;
    copy_start_time = time(NULL);
    current_copied[0] = '\0';

    // Restart progress thread
    if (progress_thread) {
        progress_thread_run = 0;
        pthread_join(progress_thread, NULL);
        progress_thread = 0;
    }
    progress_thread_run = 1;
    if (pthread_create(&progress_thread, NULL, progress_status_func, NULL) != 0) {
        progress_thread = 0;
    }

    if (unpfs(pfs_path, dst_dir, pfs_progress) != 0) {
        write_log(logpath, "ERROR: unpfs failed for %s: %s", type, pfs_path);
        progress_thread_run = 0;
        if (progress_thread) pthread_join(progress_thread, NULL);
        return -1;
    }

    // Stop progress thread
    progress_thread_run = 0;
    if (progress_thread) {
        pthread_join(progress_thread, NULL);
        progress_thread = 0;
    }

    write_log(logpath, "Successfully extracted %s PFS: %s", type, pfs_path);
    return 0;
}

/* ----------------------------------------------------------------- */
/*  Decrypt SELFs if requested                                       */
/* ----------------------------------------------------------------- */
static int decrypt_if_needed(const char *sandbox_root, const char *title_id,
                             const char *suffix, const char *dst_dir,
                             int do_decrypt, int do_elf2fself, int do_backport,
                             const char *logpath)
{
    if (!do_decrypt) return 0;

    char src_dir[1024];
    snprintf(src_dir, sizeof(src_dir), "%s/%s-%s", sandbox_root, title_id, suffix);

    if (!dir_exists(src_dir)) {
        write_log(logpath, "Info: No sandbox dir to decrypt: %s", src_dir);
        return 0;
    }

    printf_notification("Decrypting %s SELFs...", suffix);
    if (decrypt_all(src_dir, dst_dir, do_elf2fself, do_backport) != 0) {
        write_log(logpath, "ERROR: decrypt_all failed for %s", src_dir);
        return -1;
    }

    write_log(logpath, "Decrypted %s -> %s", src_dir, dst_dir);
    return 0;
}

/* ----------------------------------------------------------------- */
/*  Main dump function                                               */
/* ----------------------------------------------------------------- */
int dump_ps4_cusa_app_real(
    const char *title_id_param,
    const char *app_folder,
    const char *patch_folder,
    const char *usb_path,
    int do_decrypt,
    int do_elf2fself,
    int do_backport
)
{
    char title_id[128] = {0};
    char sandbox_root[1024] = {0};
    const char *default_sandbox_root = "/mnt/sandbox/pfsmnt";

    if (is_path_like(title_id_param)) {
        strncpy(sandbox_root, title_id_param, sizeof(sandbox_root)-1);
        if (app_folder && app_folder[0]) {
            extract_title_from_folder(app_folder, title_id, sizeof(title_id));
        } else {
            write_log(g_log_path, "ERROR: app_folder required when passing sandbox path");
            printf_notification("ERROR: app_folder required");
            return -1;
        }
    } else {
        strncpy(title_id, title_id_param ? title_id_param : "", sizeof(title_id)-1);
        strncpy(sandbox_root, default_sandbox_root, sizeof(sandbox_root)-1);
    }

    const char *usb_base = usb_path ? usb_path : "/mnt/usb0";

    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/log.txt", usb_base);
    write_log(logpath, "Starting dump for %s (split=%d) [sandbox=%s]", title_id, g_split_mode, sandbox_root);

    char base_path[1024] = {0};
    char dst_app[1024]   = {0};
    char dst_pat[1024]   = {0};

    snprintf(base_path, sizeof(base_path), "%s/%s", usb_base, title_id);

    if (g_split_mode == 0) {
        snprintf(dst_app, sizeof(dst_app), "%s", base_path);
        snprintf(dst_pat, sizeof(dst_pat), "%s", base_path);
        mkdirs(dst_app);
    } else {
        if (g_split_mode & 1) {
            snprintf(dst_app, sizeof(dst_app), "%s-app0", base_path);
            mkdirs(dst_app);
        }
        if (g_split_mode & 2) {
            snprintf(dst_pat, sizeof(dst_pat), "%s-patch0", base_path);
            mkdirs(dst_pat);
        }
    }

    /* === APP BLOCK === */
    if ((!g_split_mode) || (g_split_mode & 1)) {
        char src_pkg[1024] = {0};
        int pkg_existed = 0;

        const char *pkg_paths[] = {
            "/user/app/%s/app.pkg",
            "/mnt/ext1/user/app/%s/app.pkg",
            "/mnt/ext0/user/app/%s/app.pkg",
            NULL
        };

        for (int i = 0; pkg_paths[i]; ++i) {
            snprintf(src_pkg, sizeof(src_pkg), pkg_paths[i], title_id);
            if (file_exists(src_pkg) && isfpkg(src_pkg) == 0) {
                printf_notification("Extracting app package...");
                unpkg(src_pkg, dst_app);
                pkg_existed = 1;
                break;
            }
        }

        char sce_sys[1024];
        snprintf(sce_sys, sizeof(sce_sys), "%s/sce_sys", dst_app);
        mkdirs(sce_sys);

        copy_meta_file("/system_data/priv/appmeta/%s/nptitle.dat", title_id, dst_app, logpath);
        copy_meta_file("/system_data/priv/appmeta/%s/npbind.dat",  title_id, dst_app, logpath);

        char pfs_path[1024];
        snprintf(pfs_path, sizeof(pfs_path), "%s/%s-app0-nest/pfs_image.dat", sandbox_root, title_id);
        if (file_exists(pfs_path)) {
            write_log(logpath, "Found app PFS (PKG: %s): %s", pkg_existed ? "YES" : "NO", pfs_path);
            extract_pfs_image(pfs_path, dst_app, "app", logpath);
        } else {
            write_log(logpath, "No app PFS found: %s", pfs_path);
        }

        decrypt_if_needed(sandbox_root, title_id, "app0", dst_app,
                          do_decrypt, do_elf2fself, do_backport, logpath);
    }

    /* === PATCH BLOCK === */
    if ((!g_split_mode) || (g_split_mode & 2)) {
        char src_pkg[1024] = {0};
        int pkg_existed = 0;

        const char *pkg_paths[] = {
            "/user/patch/%s/patch.pkg",
            "/mnt/ext1/user/patch/%s/patch.pkg",
            "/mnt/ext0/user/patch/%s/patch.pkg",
            NULL
        };

        for (int i = 0; pkg_paths[i]; ++i) {
            snprintf(src_pkg, sizeof(src_pkg), pkg_paths[i], title_id);
            if (file_exists(src_pkg) && isfpkg(src_pkg) == 0) {
                printf_notification("Extracting patch package...");
                unpkg(src_pkg, dst_pat);
                pkg_existed = 1;
                break;
            }
        }

        char sce_sys[1024];
        snprintf(sce_sys, sizeof(sce_sys), "%s/sce_sys", dst_pat);
        mkdirs(sce_sys);

        copy_meta_file("/system_data/priv/appmeta/%s/nptitle.dat", title_id, dst_pat, logpath);
        copy_meta_file("/system_data/priv/appmeta/%s/npbind.dat",  title_id, dst_pat, logpath);

        char pfs_path[1024];
        snprintf(pfs_path, sizeof(pfs_path), "%s/%s-patch0-nest/pfs_image.dat", sandbox_root, title_id);
        if (file_exists(pfs_path)) {
            write_log(logpath, "Found patch PFS (PKG: %s): %s", pkg_existed ? "YES" : "NO", pfs_path);
            extract_pfs_image(pfs_path, dst_pat, "patch", logpath);
        } else {
            write_log(logpath, "No patch PFS found: %s", pfs_path);
        }

        decrypt_if_needed(sandbox_root, title_id, "patch0", dst_pat,
                          do_decrypt, do_elf2fself, do_backport, logpath);
    }

    /* === Trophy Copy === */
    char npwr_id[32] = {0};
    const char *npbind_path = NULL;
    char npbind_local[1024];

    snprintf(npbind_local, sizeof(npbind_local), "%s/sce_sys/npbind.dat", dst_app);
    if (file_exists(npbind_local)) npbind_path = npbind_local;
    else if ((g_split_mode & 2) && dst_pat[0]) {
        snprintf(npbind_local, sizeof(npbind_local), "%s/sce_sys/npbind.dat", dst_pat);
        if (file_exists(npbind_local)) npbind_path = npbind_local;
    }

    if (npbind_path && read_npwr_id(npbind_path, npwr_id, sizeof(npwr_id)) == 0 && npwr_id[0]) {
        char src_t[512];
        snprintf(src_t, sizeof(src_t), "/user/trophy/conf/%s/TROPHY.TRP", npwr_id);

        write_log(logpath, "NPWR ID: %s", npwr_id);

        if (file_exists(src_t)) {
            if (dst_app[0]) {
                char dst_dir[512], dst_file[512];
                snprintf(dst_dir, sizeof(dst_dir), "%s/sce_sys/trophy", dst_app);
                snprintf(dst_file, sizeof(dst_file), "%s/trophy00.trp", dst_dir);
                mkdirs(dst_dir);
                if (copy_file_track(src_t, dst_file) == 0) {
                    write_log(logpath, "Copied trophy to APP: %s", dst_file);
                }
            }
            if (dst_pat[0]) {
                char dst_dir[512], dst_file[512];
                snprintf(dst_dir, sizeof(dst_dir), "%s/sce_sys/trophy", dst_pat);
                snprintf(dst_file, sizeof(dst_file), "%s/trophy00.trp", dst_dir);
                mkdirs(dst_dir);
                if (copy_file_track(src_t, dst_file) == 0) {
                    write_log(logpath, "Copied trophy to PATCH: %s", dst_file);
                }
            }
        }
    }

    write_log(logpath, "Dump complete - split=%d", g_split_mode);
    printf_notification("Dump complete: %s", title_id);
    return 0;
}

/* ----------------------------------------------------------------- */
/*  Entry Points                                                     */
/* ----------------------------------------------------------------- */
int ps4_dumper_start(
    const char *title_id,
    const char *app_folder,
    const char *patch_folder,
    const char *usb_path,
    int do_decrypt,
    int do_elf2fself,
    int do_backport
)
{
    read_split_config();  // CRITICAL: Loads split=3 from config.ini
    return dump_ps4_cusa_app(title_id, app_folder, patch_folder, usb_path,
                             do_decrypt, do_elf2fself, do_backport);
}

int dump_ps4_cusa_app(
    const char *title_id,
    const char *app_folder,
    const char *patch_folder,
    const char *usb_path,
    int do_decrypt,
    int do_elf2fself,
    int do_backport
)
{
    int split = 0;
    if (app_folder && app_folder[0])   split |= 1;
    if (patch_folder && patch_folder[0]) split |= 2;

    int old_split = g_split_mode;
    g_split_mode = split;

    int ret = dump_ps4_cusa_app_real(title_id, app_folder, patch_folder, usb_path,
                                     do_decrypt, do_elf2fself, do_backport);

    g_split_mode = old_split;
    return ret;
}