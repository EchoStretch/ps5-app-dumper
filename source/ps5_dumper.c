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

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "ps5_dumper.h"
#include "utils.h"

extern int decrypt_all(const char *src_game, const char *dst_game,
                       int do_elf2fself, int do_backport);

/* --------------------------------------------------------------------- */
/*  dump_ps5_ppsa_app() – main entry point                               */
/* --------------------------------------------------------------------- */
int dump_ps5_ppsa_app(
    const char *sandbox,          /* e.g. "/mnt/sandbox/pfsmnt"                */
    const char *app_folder,       /* e.g. "PPSA12345-app0"                     */
    const char *usb_path,         /* e.g. "/mnt/usb0"                          */
    int do_decrypt,
    int do_elf2fself,
    int do_backport)
{
    /* ------------------- 1. LOG & PATH SETUP ------------------- */
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/log.txt", usb_path);
    write_log(logpath, "=== dump_ps5_ppsa_app (FLAT) started === App: %s", app_folder);

    char src_game[1024], dst_game[1024];
    snprintf(src_game, sizeof(src_game), "%s/%s", sandbox, app_folder);
    snprintf(dst_game, sizeof(dst_game), "%s/%s", usb_path, app_folder);

    mkdirs(dst_game);  // void return

    /* ------------------- 2. RESET PROGRESS GLOBALS ------------------- */
    progress_thread_run = 0;
    if (progress_thread) {
        pthread_join(progress_thread, NULL);
        progress_thread = 0;
    }
    folder_size_current = 0;
    total_bytes_copied = 0;
    copy_start_time = 0;
    current_copied[0] = '\0';

    /* ------------------- 3. CALCULATE TOTAL SIZE ------------------- */
    size_walker(src_game, &folder_size_current);
    if (folder_size_current == 0) {
        write_log(logpath, "Warning: No files found in %s", src_game);
        return -1;
    }

    /* ------------------- 4. START PROGRESS THREAD ------------------- */
    copy_start_time = time(NULL);
    strncpy(current_copied, app_folder, sizeof(current_copied) - 1);
    current_copied[sizeof(current_copied) - 1] = '\0';

    progress_thread_run = 1;
    if (pthread_create(&progress_thread, NULL, progress_status_func, NULL) != 0) {
        write_log(logpath, "ERROR: pthread_create(progress) failed");
        progress_thread_run = 0;
    }

    /* ------------------- 5. COPY MAIN APP ------------------- */
    write_log(logpath, "Copying main app: %s -> %s", src_game, dst_game);
    copy_dir_recursive_tracked(src_game, dst_game);

    /* ------------------- 6. STOP PROGRESS THREAD ------------------- */
    progress_thread_run = 0;
    if (progress_thread) {
        pthread_join(progress_thread, NULL);
        progress_thread = 0;
    }

    write_log(logpath, "Main app copy complete.");
    printf_notification("Main app copy complete.");

    /* ------------------- 7. EXTRACT PPSA SHORT ID ------------------- */
    char ppsa_short[32] = {0};
    const char *dash = strchr(app_folder, '-');
    if (dash) {
        size_t n = dash - app_folder;
        if (n >= sizeof(ppsa_short)) n = sizeof(ppsa_short) - 1;
        memcpy(ppsa_short, app_folder, n);
        ppsa_short[n] = '\0';
    } else {
        strncpy(ppsa_short, app_folder, sizeof(ppsa_short) - 1);
    }

    /* ------------------- 8. COPY APPMETA (FLAT into sce_sys) ------------------- */
    char src_user_meta[512], src_sys_meta[512];
    char dst_flat[512];

    snprintf(src_user_meta, sizeof(src_user_meta), "/user/appmeta/%s", ppsa_short);
    snprintf(src_sys_meta,  sizeof(src_sys_meta),  "/system_data/priv/appmeta/%s", ppsa_short);
    snprintf(dst_flat,      sizeof(dst_flat),      "%s/sce_sys", dst_game);

    mkdirs(dst_flat);  // Ensure sce_sys exists

    if (dir_exists(src_user_meta)) {
        write_log(logpath, "Copying user appmeta (flat): %s -> %s", src_user_meta, dst_flat);
        copy_dir_recursive_tracked(src_user_meta, dst_flat);
    } else {
        write_log(logpath, "User appmeta not found: %s", src_user_meta);
    }

    if (dir_exists(src_sys_meta)) {
        write_log(logpath, "Copying system appmeta (flat): %s -> %s", src_sys_meta, dst_flat);
        copy_dir_recursive_tracked(src_sys_meta, dst_flat);
    } else {
        write_log(logpath, "System appmeta not found: %s", src_sys_meta);
    }

    /* ------------------- 9. ENSURE sce_sys SUBDIRS ------------------- */
    char trophy_dir[512], uds_dir[512];
    snprintf(trophy_dir, sizeof(trophy_dir), "%s/sce_sys/trophy2", dst_game);
    snprintf(uds_dir,    sizeof(uds_dir),    "%s/sce_sys/uds",     dst_game);
    mkdirs(trophy_dir);
    mkdirs(uds_dir);

    /* ------------------- 10. TROPHY & UDS (via npbind.dat) ------------------- */
    char npbind_src1[512], npbind_src2[512];
    snprintf(npbind_src1, sizeof(npbind_src1),
             "/system_data/priv/appmeta/%s/trophy2/npbind.dat", ppsa_short);
    snprintf(npbind_src2, sizeof(npbind_src2),
             "/system_data/priv/appmeta/%s/uds/npbind.dat", ppsa_short);

    const char *npbind_path = NULL;
    if (file_exists(npbind_src1)) npbind_path = npbind_src1;
    else if (file_exists(npbind_src2)) npbind_path = npbind_src2;

    if (npbind_path) {
        char npwr_id[32] = {0};
        if (read_npwr_id(npbind_path, npwr_id, sizeof(npwr_id)) == 0 && npwr_id[0]) {
            write_log(logpath, "NPWR ID extracted: %s", npwr_id);

            /* ---- Trophy ---- */
            char src_t[512], dst_t[512];
            snprintf(src_t, sizeof(src_t), "/user/trophy2/nobackup/conf/%s/TROPHY.UCP", npwr_id);
            snprintf(dst_t, sizeof(dst_t), "%s/sce_sys/trophy2/trophy00.ucp", dst_game);

            if (file_exists(src_t)) {
                write_log(logpath, "Copying trophy: %s -> %s", src_t, dst_t);
                copy_file_track(src_t, dst_t);
            } else {
                write_log(logpath, "Trophy file not found: %s", src_t);
            }

            /* ---- UDS ---- */
            char src_u[512], dst_u[512];
            snprintf(src_u, sizeof(src_u), "/user/np_uds/nobackup/conf/%s/uds.ucp", npwr_id);
            snprintf(dst_u, sizeof(dst_u), "%s/sce_sys/uds/uds00.ucp", dst_game);

            if (file_exists(src_u)) {
                write_log(logpath, "Copying UDS: %s -> %s", src_u, dst_u);
                copy_file_track(src_u, dst_u);
            } else {
                write_log(logpath, "UDS file not found: %s", src_u);
            }
        } else {
            write_log(logpath, "Failed to read NPWR ID from %s", npbind_path);
        }
    } else {
        write_log(logpath, "npbind.dat not found in either location.");
    }

    /* ------------------- 11. OPTIONAL DECRYPTION ------------------- */
    if (do_decrypt) {
        write_log(logpath, "Starting decryption (elf2fself=%d, backport=%d)...", do_elf2fself, do_backport);
        printf_notification("Decrypting...");
        int dec_err = decrypt_all(src_game, dst_game, do_elf2fself, do_backport);
        if (dec_err == 0) {
            write_log(logpath, "Decryption completed successfully.");
            printf_notification("Decryption complete.");
        } else {
            write_log(logpath, "Decryption failed with code %d", dec_err);
            printf_notification("Decryption failed (%d)", dec_err);
        }
    }

    /* ------------------- 12. FINALIZE ------------------- */
    write_log(logpath, "=== Dump complete (FLAT): %s ===", dst_game);
    printf_notification("Dump complete: %s", app_folder);

    return 0;
}