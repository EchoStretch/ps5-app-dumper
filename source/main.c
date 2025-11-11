#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#include "utils.h"

/* prototype for decrypt function in decrypt.c */
extern int decrypt_all(const char *src_game, const char *dst_game, int do_elf2fself, int do_backport);

#define VERSION "1.05"
#define SANDBOX_PATH "/mnt/sandbox/pfsmnt"
#define LOG_FILE_NAME "log.txt"

int main(void)
{
    printf_notification("Welcome to PS5 App Dumper v%s", VERSION);

    /* Wait for USB */
    while (find_usb_and_setup() == -1) {
        printf_notification("Please insert USB (exFAT) into any port...");
        sleep(7);
    }

    const char *usb_data = get_usb_homebrew_path();
    if (!usb_data || !usb_data[0]) {
        printf_notification("Failed to detect USB path.");
        return 1;
    }

    /* Read config */
    int do_decrypt     = read_decrypter_config();
    int do_elf2fself   = read_elf2fself_config();
    int do_backport    = read_backport_config();
    g_enable_logging   = read_logging_config();

    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/%s", usb_data, LOG_FILE_NAME);
    strncpy(g_log_path, logpath, sizeof(g_log_path) - 1);
    g_log_path[sizeof(g_log_path) - 1] = '\0';

    write_log(logpath, "=== PS5 App Dumper v%s started ===", VERSION);

    /* Detect running app */
    DIR *d = opendir(SANDBOX_PATH);
    if (!d)
    {
        write_log(logpath, "ERROR: Failed to open %s", SANDBOX_PATH);
        printf_notification("Failed to open %s", SANDBOX_PATH);
        return 1;
    }

    struct dirent *dp;
    char ppsa_foldername[128] = {0};

    while ((dp = readdir(d)) != NULL)
    {
        if (dp->d_type == DT_DIR && strncmp(dp->d_name, "PPSA", 4) == 0)
        {
            size_t len = strlen(dp->d_name);
            if (len > 5 && strcmp(dp->d_name + len - 5, "-app0") == 0)
            {
                strncpy(ppsa_foldername, dp->d_name, sizeof(ppsa_foldername)-1);
                break;
            }
        }
    }
    closedir(d);

    if (ppsa_foldername[0] == '\0')
    {
        write_log(logpath, "Please start the App before running the payload...");
        printf_notification("Please start the App before running the payload...");
        return 1;
    }

    write_log(logpath, "Detected App: %s", ppsa_foldername);
    printf_notification("Detected: %s", ppsa_foldername);

    /* Extract short PPSA ID (e.g., PPSA12345) */
    char ppsa_short[32] = {0};
    char *dash = strchr(ppsa_foldername, '-');
    if (dash)
    {
        size_t n = dash - ppsa_foldername;
        if (n >= sizeof(ppsa_short)) n = sizeof(ppsa_short)-1;
        memcpy(ppsa_short, ppsa_foldername, n);
        ppsa_short[n] = '\0';
    } else {
        strncpy(ppsa_short, ppsa_foldername, sizeof(ppsa_short)-1);
    }

    /* Build paths */
    char src_game[1024], dst_game[1024];
    snprintf(src_game, sizeof(src_game), "%s/%s", SANDBOX_PATH, ppsa_foldername);
    snprintf(dst_game, sizeof(dst_game), "%s/%s", usb_data, ppsa_foldername);

    /* Calculate total size */
    folder_size_current = 0;
    size_walker(src_game, &folder_size_current);

    /* Start progress thread */
    pthread_t progress_thread;
    progress_thread_run = 1;
    pthread_create(&progress_thread, NULL, progress_status_func, NULL);

    write_log(logpath, "Copying App: %s -> %s", src_game, dst_game);
    printf_notification("Copying App: %s -> %s", src_game, dst_game);

    copy_dir_recursive_tracked(src_game, dst_game);

    write_log(logpath, "Main App copy complete.");
    printf_notification("Main App copy complete.");

    /* Copy appmeta (user + system) */
    char src_meta_user[512], src_meta_sys[512], dst_meta[512];
    snprintf(src_meta_user, sizeof(src_meta_user), "/user/appmeta/%s", ppsa_short);
    snprintf(src_meta_sys,  sizeof(src_meta_sys),  "/system_data/priv/appmeta/%s", ppsa_short);
    snprintf(dst_meta,      sizeof(dst_meta),      "%s/sce_sys", dst_game);

    if (dir_exists(src_meta_user))
    {
        write_log(logpath, "Copying user appmeta...");
        copy_dir_recursive_tracked(src_meta_user, dst_meta);
    }
    if (dir_exists(src_meta_sys))
    {
        write_log(logpath, "Copying system appmeta...");
        copy_dir_recursive_tracked(src_meta_sys, dst_meta);
    }

    /* Extract NPWR ID and copy UDS / Trophy */
    char npbind_src1[1024], npbind_src2[1024];
    snprintf(npbind_src1, sizeof(npbind_src1),
             "/system_data/priv/appmeta/%s/trophy2/npbind.dat", ppsa_short);
    snprintf(npbind_src2, sizeof(npbind_src2),
             "/system_data/priv/appmeta/%s/uds/npbind.dat", ppsa_short);

    char npwr_id[32] = {0};
    const char *npbind_path = NULL;

    write_log(logpath, "Searching for npbind.dat...");
    write_log(logpath, "Checking: %s", npbind_src1);
    write_log(logpath, "Checking: %s", npbind_src2);

    if (file_exists(npbind_src1))
        npbind_path = npbind_src1;
    else if (file_exists(npbind_src2))
        npbind_path = npbind_src2;

    if (npbind_path && read_npwr_id(npbind_path, npwr_id, sizeof(npwr_id)) == 0 && npwr_id[0] != '\0')
    {
        write_log(logpath, "Extracted NPWR ID: %s", npwr_id);

        char src_uds[512], dst_uds[512];
        snprintf(src_uds, sizeof(src_uds), "/user/np_uds/nobackup/conf/%s/uds.ucp", npwr_id);
        snprintf(dst_uds, sizeof(dst_uds), "%s/sce_sys/uds/uds00.ucp", dst_game);
        mkdirs(dst_game); mkdirs(dst_meta);

        if (file_exists(src_uds))
        {
            write_log(logpath, "Copying UDS: %s -> %s", src_uds, dst_uds);
            copy_file_track(src_uds, dst_uds);
        }
        else
        {
            write_log(logpath, "UDS not found: %s", src_uds);
        }

        char src_trophy[512], dst_trophy[512];
        snprintf(src_trophy, sizeof(src_trophy), "/user/trophy2/nobackup/conf/%s/TROPHY.UCP", npwr_id);
        snprintf(dst_trophy, sizeof(dst_trophy), "%s/sce_sys/trophy2/trophy00.ucp", dst_game);

        if (file_exists(src_trophy))
        {
            write_log(logpath, "Copying TROPHY: %s -> %s", src_trophy, dst_trophy);
            copy_file_track(src_trophy, dst_trophy);
        }
        else
        {
            write_log(logpath, "TROPHY not found: %s", src_trophy);
        }
    }
    else
    {
        write_log(logpath,
                  "npbind.dat not found or NPWR ID not extracted.\nChecked:\n  %s\n  %s",
                  npbind_src1, npbind_src2);
    }

    /* Stop progress thread */
    progress_thread_run = 0;
    pthread_join(progress_thread, NULL);

    /* Decrypt + FSELF + Backport */
    if (do_decrypt) {
        write_log(logpath, "Decrypting to: %s", dst_game);
        printf_notification("Decrypting to: %s", dst_game);

        int decrypt_err = decrypt_all(src_game, dst_game, do_elf2fself, do_backport);
        if (decrypt_err == 0) {
            write_log(logpath, "Decryption Finished: %s", dst_game);
            printf_notification("Decryption Finished: %s", dst_game);
        } else {
            write_log(logpath, "Decryption Failed: %d", decrypt_err);
            printf_notification("Decryption Failed: %d", decrypt_err);
        }
    }

    write_log(logpath, "Dump Complete: %s", dst_game);
    printf_notification("Dump Complete: %s", dst_game);

    write_log(logpath, "=== PS5 App Dumper v%s finished ===", VERSION);

    return 0;
}