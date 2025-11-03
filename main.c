#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#include "utils.h"

/* prototype for decrypt function in decrypt.c */
int decrypt_all(const char *src_game, const char *dst_game);

#define VERSION "1.02"
#define SANDBOX_PATH "/mnt/sandbox/pfsmnt"
#define USB_ROOT     "/mnt/usb0"
#define USB_DATA     "/mnt/usb0/homebrew"
#define LOG_FILE_NAME "log.txt"

int main(void)
{
    printf_notification("Welcome to PS5 App Dumper v%s", VERSION);

    if (!dir_exists(USB_ROOT))
    {
        printf_notification("No USB plugged in! Aborting.");
        return 1;
    }

    mkdirs(USB_DATA);
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/%s", USB_DATA, LOG_FILE_NAME);
#if ENABLE_LOGGING
    write_log(logpath, "=== PS5 App Dumper v%s started ===", VERSION);
#endif

    /* locate PPSA-app0 folder */
    DIR *d = opendir(SANDBOX_PATH);
    if (!d)
    {
#if ENABLE_LOGGING
        write_log(logpath, "ERROR: Failed to open %s", SANDBOX_PATH);
#endif
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
        printf_notification("No PPSA-app0 folder found!");
#if ENABLE_LOGGING
        write_log(logpath, "No PPSA-app0 folder found.");
#endif
        return 1;
    }

#if ENABLE_LOGGING
    write_log(logpath, "Detected game: %s", ppsa_foldername);
#endif
    printf_notification("Detected: %s", ppsa_foldername);

    /* extract PPSA short name */
    char ppsa_short[32] = {0};
    char *dash = strchr(ppsa_foldername, '-');
    if (dash)
    {
        size_t n = dash - ppsa_foldername;
        if (n >= sizeof(ppsa_short)) n = sizeof(ppsa_short)-1;
        memcpy(ppsa_short, ppsa_foldername, n);
        ppsa_short[n] = '\0';
    }
    else
        strncpy(ppsa_short, ppsa_foldername, sizeof(ppsa_short)-1);

    char src_game[1024], dst_game[1024];
    snprintf(src_game, sizeof(src_game), "%s/%s", SANDBOX_PATH, ppsa_foldername);
    snprintf(dst_game, sizeof(dst_game), "%s/%s", USB_DATA, ppsa_foldername);

    folder_size_current = 0;
    size_walker(src_game, &folder_size_current);

    pthread_t progress_thread;
    progress_thread_run = 1;
    pthread_create(&progress_thread, NULL, progress_status_func, NULL);

#if ENABLE_LOGGING
    write_log(logpath, "Copying game: %s -> %s", src_game, dst_game);
#endif
    copy_dir_recursive_tracked(src_game, dst_game);
#if ENABLE_LOGGING
    write_log(logpath, "Main game copy complete.");
#endif

    /* copy user & system appmeta */
    char src_meta_user[512], src_meta_sys[512], dst_meta[512];
    snprintf(src_meta_user, sizeof(src_meta_user), "/user/appmeta/%s", ppsa_short);
    snprintf(src_meta_sys,  sizeof(src_meta_sys),  "/system_data/priv/appmeta/%s", ppsa_short);
    snprintf(dst_meta,      sizeof(dst_meta),      "%s/sce_sys", dst_game);

    if (dir_exists(src_meta_user))
    {
#if ENABLE_LOGGING
        write_log(logpath, "Copying user appmeta...");
#endif
        copy_dir_recursive_tracked(src_meta_user, dst_meta);
    }
    if (dir_exists(src_meta_sys))
    {
#if ENABLE_LOGGING
        write_log(logpath, "Copying system appmeta...");
#endif
        copy_dir_recursive_tracked(src_meta_sys, dst_meta);
    }

    /* NPWR handling */
    char npbind_src1[1024], npbind_src2[1024];
    snprintf(npbind_src1, sizeof(npbind_src1),
             "/system_data/priv/appmeta/%s/trophy2/npbind.dat", ppsa_short);
    snprintf(npbind_src2, sizeof(npbind_src2),
             "/system_data/priv/appmeta/%s/uds/npbind.dat", ppsa_short);

    char npwr_id[32] = {0};
    const char *npbind_path = NULL;

#if ENABLE_LOGGING
    write_log(logpath, "Searching for npbind.dat...");
    write_log(logpath, "Checking: %s", npbind_src1);
    write_log(logpath, "Checking: %s", npbind_src2);
#endif

    if (file_exists(npbind_src1))
        npbind_path = npbind_src1;
    else if (file_exists(npbind_src2))
        npbind_path = npbind_src2;

    if (npbind_path && read_npwr_id(npbind_path, npwr_id, sizeof(npwr_id)) == 0 && npwr_id[0] != '\0')
    {
#if ENABLE_LOGGING
        write_log(logpath, "Extracted NPWR ID: %s", npwr_id);
#endif

        /* UDS */
        char src_uds[512], dst_uds[512];
        snprintf(src_uds, sizeof(src_uds), "/user/np_uds/nobackup/conf/%s/uds.ucp", npwr_id);
        snprintf(dst_uds, sizeof(dst_uds), "%s/sce_sys/uds/uds00.ucp", dst_game);
        mkdirs(dst_game); mkdirs(dst_meta);

        if (file_exists(src_uds))
        {
#if ENABLE_LOGGING
            write_log(logpath, "Copying UDS: %s -> %s", src_uds, dst_uds);
#endif
            copy_file_track(src_uds, dst_uds);
        }
        else
        {
#if ENABLE_LOGGING
            write_log(logpath, "UDS not found: %s", src_uds);
#endif
        }

        /* TROPHY */
        char src_trophy[512], dst_trophy[512];
        snprintf(src_trophy, sizeof(src_trophy), "/user/trophy2/nobackup/conf/%s/TROPHY.UCP", npwr_id);
        snprintf(dst_trophy, sizeof(dst_trophy), "%s/sce_sys/trophy2/trophy00.ucp", dst_game);

        if (file_exists(src_trophy))
        {
#if ENABLE_LOGGING
            write_log(logpath, "Copying TROPHY: %s -> %s", src_trophy, dst_trophy);
#endif
            copy_file_track(src_trophy, dst_trophy);
        }
        else
        {
#if ENABLE_LOGGING
            write_log(logpath, "TROPHY not found: %s", src_trophy);
#endif
        }
    }
    else
    {
#if ENABLE_LOGGING
        write_log(logpath,
                  "npbind.dat not found or NPWR ID not extracted.\nChecked:\n  %s\n  %s",
                  npbind_src1, npbind_src2);
#endif
    }

    /* stop progress thread */
    progress_thread_run = 0;
    pthread_join(progress_thread, NULL);

#if ENABLE_LOGGING
    write_log(logpath, "Dump finished: %s", dst_game);
#endif

    printf_notification("Dump complete: %s", dst_game);
    printf("Dump complete. Log: %s\n", logpath);
	
    printf_notification("Starting Decrypting: %s", src_game);
#if ENABLE_LOGGING
    write_log(logpath, "Starting Decrypting: %s", src_game);
#endif
    int decrypt_err = decrypt_all(src_game, dst_game);
    printf_notification("Decryption Finished Successfully: %s", dst_game);
#if ENABLE_LOGGING
    write_log(logpath, "Decryption Finished Successfully: %s", dst_game);
    write_log(logpath, "=== PS5 App Dumper v%s finished ===", VERSION);
#endif

    return 0;

}
