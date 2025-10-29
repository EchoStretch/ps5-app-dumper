/* main.c - PS5 App Dumper */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "utils.h"

#define VERSION "1.0"
#define SANDBOX_PATH "/mnt/sandbox/pfsmnt"
#define USB_ROOT "/mnt/usb0"
#define USB_DATA "/mnt/usb0/data"
#define LOG_FILE_NAME "log.txt"

size_t folder_size_current = 0;
size_t total_bytes_copied = 0;
char current_copied[256];
int progress_thread_run = 1;
time_t copy_start_time = 0; // Start timer when first byte copied

/* read NPWR id from npbind.dat */
int read_npwr_id(const char *npbind_path, char *npwr_out, size_t out_size)
{
    if (!npbind_path || !npwr_out || out_size == 0) return -1;

    FILE *fp = fopen(npbind_path, "rb");
    if (!fp) return -1;

    unsigned char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    if (n < 9) return -1;

    for (size_t i = 0; i <= n - 9; i++)
    {
        if (buf[i] == 'N' && buf[i+1] == 'P' && buf[i+2] == 'W' && buf[i+3] == 'R')
        {
            size_t j;
            for (j = 0; j < out_size - 1 && i + j < n; j++)
            {
                if (isprint(buf[i+j]))
                    npwr_out[j] = buf[i+j];
                else
                    break;
            }
            npwr_out[j] = '\0';
            return 0;
        }
    }

    return -1;
}

/* chunked file copy to update total_bytes_copied smoothly */
int copy_file_chunked(const char *src, const char *dst)
{
    FILE *fs = fopen(src, "rb");
    if (!fs) return -1;

    FILE *fd = fopen(dst, "wb");
    if (!fd) { fclose(fs); return -1; }

    char buf[1024*1024]; // 1 MB chunks
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fs)) > 0)
    {
        if (fwrite(buf, 1, n, fd) != n) { fclose(fs); fclose(fd); return -1; }
        total_bytes_copied += n;
    }

    fclose(fs);
    fclose(fd);
    return 0;
}

/* copy single file and update totals */
int copy_file_track(const char *src, const char *dst)
{
    if (!src || !dst) return -1;

    strncpy(current_copied, src, sizeof(current_copied)-1);
    current_copied[sizeof(current_copied)-1] = '\0';

    if (copy_start_time == 0) copy_start_time = time(NULL); // start timer at first byte

    return copy_file_chunked(src, dst);
}

/* recursively copy directory with progress tracking */
void copy_dir_recursive_tracked(const char *src, const char *dst)
{
    DIR *d = opendir(src);
    if (!d) return;

    mkdirs(dst);

    struct dirent *dp;
    struct stat st;
    char src_path[1024], dst_path[1024];

    while ((dp = readdir(d)) != NULL)
    {
        if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) continue;

        snprintf(src_path, sizeof(src_path), "%s/%s", src, dp->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, dp->d_name);

        if (stat(src_path, &st) == 0)
        {
            if (S_ISDIR(st.st_mode))
                copy_dir_recursive_tracked(src_path, dst_path);
            else if (S_ISREG(st.st_mode))
                copy_file_track(src_path, dst_path);
        }
    }
    closedir(d);
}

/* calculate folder size recursively */
static void size_walker(const char *path, size_t *acc)
{
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *dp;
    struct stat st;
    char sub[1024];

    while ((dp = readdir(d)) != NULL)
    {
        if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, dp->d_name);
        if (stat(sub, &st) == 0)
        {
            if (S_ISDIR(st.st_mode)) size_walker(sub, acc);
            else if (S_ISREG(st.st_mode)) *acc += (size_t)st.st_size;
        }
    }
    closedir(d);
}

/* progress thread */
void *progress_status_func(void *arg)
{
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/%s", USB_DATA, LOG_FILE_NAME);

    while (progress_thread_run)
    {
        sleep(1);
        if (folder_size_current == 0 || copy_start_time == 0) continue;

        size_t remaining_bytes = folder_size_current > total_bytes_copied ? folder_size_current - total_bytes_copied : 0;

        double elapsed_sec = difftime(time(NULL), copy_start_time);
        double avg_speed_mb_s = elapsed_sec > 0 ? (double)total_bytes_copied / (1024.0*1024.0) / elapsed_sec : 0;

        double est_sec = avg_speed_mb_s > 0 ? (double)remaining_bytes / (1024.0*1024.0) / avg_speed_mb_s : 0;

        int pct = folder_size_current ? (int)((total_bytes_copied * 100) / folder_size_current) : 0;
        if (pct > 100) pct = 100;

        double copied_gb = (double)total_bytes_copied / (1024.0*1024.0*1024.0);
        double total_gb = (double)folder_size_current / (1024.0*1024.0*1024.0);

        int est_h = (int)(est_sec / 3600);
        int est_m = (int)((est_sec - est_h*3600)/60);
        int est_s = (int)(est_sec - est_h*3600 - est_m*60);

        printf_notification(
            "Copying: %s\nProgress: %d%%\n%.2fGB of %.2fGB\nAverage speed: %.2f MB/s\nETA: %02d:%02d:%02d",
            current_copied, pct, copied_gb, total_gb, avg_speed_mb_s, est_h, est_m, est_s
        );

        write_log(logpath,
                  "Progress: %d%% Copied: %.2f/%.2f GB Remaining: %.2f GB "
                  "Average speed: %.2f MB/s ETA: %02d:%02d:%02d",
                  pct, copied_gb, total_gb, (double)remaining_bytes/(1024.0*1024.0*1024.0),
                  avg_speed_mb_s, est_h, est_m, est_s);
    }
    return NULL;
}

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
    write_log(logpath, "=== PS5 App Dumper v%s started ===", VERSION);

    /* locate PPSA-app0 folder */
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
        printf_notification("No PPSA-app0 folder found!");
        write_log(logpath, "No PPSA-app0 folder found.");
        return 1;
    }

    write_log(logpath, "Detected game: %s", ppsa_foldername);
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

    write_log(logpath, "Copying game: %s -> %s", src_game, dst_game);
    copy_dir_recursive_tracked(src_game, dst_game);
    write_log(logpath, "Main game copy complete.");

    /* copy user & system appmeta */
    char src_meta_user[512], src_meta_sys[512], dst_meta[512];
    snprintf(src_meta_user, sizeof(src_meta_user), "/user/appmeta/%s", ppsa_short);
    snprintf(src_meta_sys, sizeof(src_meta_sys), "/system_data/priv/appmeta/%s", ppsa_short);
    snprintf(dst_meta, sizeof(dst_meta), "%s/sce_sys", dst_game);

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

    /* NPWR handling: both UDS and TROPHY under /user */
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

        /* Copy UDS */
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
            write_log(logpath, "UDS not found: %s", src_uds);

        /* Copy TROPHY */
        char src_trophy[512], dst_trophy[512];
        snprintf(src_trophy, sizeof(src_trophy), "/user/trophy2/nobackup/conf/%s/TROPHY.UCP", npwr_id);
        snprintf(dst_trophy, sizeof(dst_trophy), "%s/sce_sys/trophy2/trophy00.ucp", dst_game);

        if (file_exists(src_trophy))
        {
            write_log(logpath, "Copying TROPHY: %s -> %s", src_trophy, dst_trophy);
            copy_file_track(src_trophy, dst_trophy);
        }
        else
            write_log(logpath, "TROPHY not found: %s", src_trophy);
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

    write_log(logpath, "Dump finished: %s", dst_game);
    write_log(logpath, "=== PS5 App Dumper v%s finished ===", VERSION);
    printf_notification("Dump complete: %s", dst_game);
    printf("Dump complete. Log: %s\n", logpath);

    return 0;
}
