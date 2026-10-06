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
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>

#include "utils.h"
#include "webhb.h"

size_t folder_size_current = 0;
size_t total_bytes_copied = 0;
char current_copied[256] = {0};
int progress_thread_run = 1;
time_t copy_start_time = 0;
pthread_t progress_thread = 0;


/* ------------------------------------------------------------------ */
/*  Cooperative abort                                                  */
/* ------------------------------------------------------------------ */

static volatile int g_abort_requested = 0;

int g_split_mode = 3;  // default: split both
char g_folder_suffix[48] = "";
int g_ps4_backport_level = 0;
int g_ps5_backport_level = 0;

void request_abort(void) { g_abort_requested = 1; }
void clear_abort(void)   { g_abort_requested = 0; }
int  abort_requested(void) { return g_abort_requested; }

const char* detect_fs_type(const char *mountpoint) {
    char cmd[256], line[256];
    snprintf(cmd, sizeof(cmd), "mount | grep \"%s \"", mountpoint);
    FILE *fp = popen(cmd, "r");
    if (!fp) return "unknown";

    if (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "exfat")) { pclose(fp); return "exFAT"; }
        if (strstr(line, "vfat"))  { pclose(fp); return "FAT32"; }
        if (strstr(line, "ntfs"))  { pclose(fp); return "NTFS"; }
    }
    pclose(fp);
    return "unknown";
}

void debug_list_usbs(void) {
    FILE *fp = popen("mount | grep usb", "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        printf_notification("Mounted USB: %s", line);
    }
    pclose(fp);
}

int read_decrypter_config(void) {
    if (get_app_data_path()[0] == '\0') return 1;

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/config.ini", get_app_data_path());

    FILE *f = fopen(config_path, "r");
    if (!f) return 1;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "enable_decrypter", 9) == 0) {
            p += 9;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            if (*p == '0') { fclose(f); return 0; }
            if (*p == '1') { fclose(f); return 1; }
        }
    }
    fclose(f);
    return 1;
}

int read_logging_config(void) {
    if (get_app_data_path()[0] == '\0') return 1;

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/config.ini", get_app_data_path());

    FILE *f = fopen(config_path, "r");
    if (!f) return 1;

    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "enable_logging", 14) == 0) {
            p += 14;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            if (*p == '0') { g_enable_logging = 0; found = 1; }
            else if (*p == '1') { g_enable_logging = 1; found = 1; }
        }
    }
    fclose(f);
    return found ? g_enable_logging : 1;
}

int read_backport_config(void)
{
    if (get_app_data_path()[0] == '\0') return 1; 

    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.ini", get_app_data_path());

    FILE *f = fopen(cfg_path, "r");
    if (!f) return 1;

    char line[256];
    int  value = 1;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "enable_backport", 15) == 0) {
            p += 15;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            if (*p == '0') value = 0;
            else if (*p == '1') value = 1;
        }
    }
    fclose(f);
    return value;
}

int read_elf2fself_config(void)
{
    if (get_app_data_path()[0] == '\0') return 1;

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/config.ini", get_app_data_path());

    FILE *f = fopen(config_path, "r");
    if (!f) return 1;

    char line[128];
    int value = 1;  // default: enabled
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "enable_elf2fself", 16) == 0) {
            p += 16;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            if (*p == '0') value = 0;
            else if (*p == '1') value = 1;
        }
    }
    fclose(f);
    return value;  // 1 = enable, 0 = disable
}

int read_split_config(void)
{
    if (get_app_data_path()[0] == '\0') return 3;

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/config.ini", get_app_data_path());

    FILE *f = fopen(config_path, "r");
    if (!f) return 3;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "split", 5) == 0) {
            p += 5;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            int val = atoi(p);
            if (val >= 0 && val <= 3) {
                fclose(f);
                return val;
            }
        }
    }
    fclose(f);
    return 3;
}

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

#define BUF_SIZE 0x800000

/* read() and write() may move fewer bytes than asked for; only an error or
   the source ending before st_size fails the copy */
static int fs_ncopy(int fd_in, int fd_out, size_t size)
{
    size_t cap = size < BUF_SIZE ? size : BUF_SIZE;
    char *buf = malloc(cap ? cap : 1);
    if (!buf) return -1;

    size_t copied = 0;
    int ret = -1;

    while (copied < size) {
        if (abort_requested()) { errno = ECANCELED; goto out; }

        size_t want = size - copied;
        if (want > cap) want = cap;

        ssize_t n = read(fd_in, buf, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            goto out;
        }
        if (n == 0) { errno = EIO; goto out; }

        for (ssize_t off = 0; off < n; ) {
            ssize_t w = write(fd_out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                goto out;
            }
            if (w == 0) { errno = EIO; goto out; }
            off += w;
        }

        copied += (size_t)n;
        total_bytes_copied += (size_t)n;
    }
    ret = 0;
out:
    free(buf);
    return ret;
}

int fs_copy_file(const char *src, const char *dst)
{
    struct stat st;
    int src_fd = -1, dst_fd = -1;
    int ret = -1;
    int err = 0;

    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;

    src_fd = open(src, O_RDONLY);
    if (src_fd < 0) return -1;

    dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode | 0600);
    if (dst_fd < 0) { err = errno; goto cleanup; }

    /* update UI string */
    strncpy(current_copied, src, sizeof(current_copied)-1);
    current_copied[sizeof(current_copied)-1] = '\0';

    ret = fs_ncopy(src_fd, dst_fd, (size_t)st.st_size);
    if (ret) err = errno;

    /* a USB drive can report a failed write-back only here */
    if (close(dst_fd) != 0 && ret == 0) { ret = -1; err = errno; }
    dst_fd = -1;
    if (ret) unlink(dst);

cleanup:
    if (dst_fd >= 0) close(dst_fd);
    close(src_fd);
    if (ret) errno = err;
    return ret;
}

int copy_file_track(const char *src, const char *dst)
{
    if (copy_start_time == 0) copy_start_time = time(NULL);
    return fs_copy_file(src, dst);
}

int copy_dir_recursive_tracked(const char *src, const char *dst)
{
    DIR *d = opendir(src);
    if (!d) {
        write_log(g_log_path, "ERROR: cannot open %s: %s", src, strerror(errno));
        return 1;
    }

    mkdirs(dst);

    struct dirent *dp;
    struct stat st;
    char src_path[1024], dst_path[1024];
    int failed = 0;

    while ((dp = readdir(d)) != NULL)
    {
        if (abort_requested()) break;
        if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) continue;

        snprintf(src_path, sizeof(src_path), "%s/%s", src, dp->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, dp->d_name);

        if (stat(src_path, &st) != 0) {
            write_log(g_log_path, "ERROR: cannot stat %s: %s", src_path, strerror(errno));
            failed++;
        } else if (S_ISDIR(st.st_mode)) {
            failed += copy_dir_recursive_tracked(src_path, dst_path);
        } else if (S_ISREG(st.st_mode) && copy_file_track(src_path, dst_path) != 0) {
            if (abort_requested()) break;
            write_log(g_log_path, "ERROR: copy failed %s: %s", src_path, strerror(errno));
            failed++;
        }
    }
    closedir(d);
    return failed;
}

void size_walker(const char *path, size_t *acc)
{
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *dp;
    struct stat st;
    char sub[1024];

    while ((dp = readdir(d)) != NULL)
    {
        if (abort_requested()) break;
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

void *progress_status_func(void *arg)
{
    while (progress_thread_run)
    {
        sleep(7);
        if (folder_size_current == 0 || copy_start_time == 0) continue;

        size_t remaining_bytes = folder_size_current > total_bytes_copied ? folder_size_current - total_bytes_copied : 0;

        double elapsed_sec = difftime(time(NULL), copy_start_time);
        double avg_speed_mb_s = elapsed_sec > 0 ? (double)total_bytes_copied / (1024.0*1024.0) / elapsed_sec : 0;

        double est_sec = avg_speed_mb_s > 0 ? (double)remaining_bytes / (1024.0*1024.0) / avg_speed_mb_s : 0;

        int pct = folder_size_current ? (int)((total_bytes_copied * 100) / folder_size_current) : 0;
        if (pct > 100) pct = 100;

        double copied_gb = (double)total_bytes_copied / (1024.0*1024.0*1024.0);
        double total_gb   = (double)folder_size_current / (1024.0*1024.0*1024.0);

        int est_h = (int)(est_sec / 3600);
        int est_m = (int)((est_sec - est_h*3600)/60);
        int est_s = (int)(est_sec - est_h*3600 - est_m*60);

        /* The toast goes to the screen only: the log line below says the
           same, and the web console would otherwise show every tick twice. */
        printf_notification_quiet(
            "Copying: %s\nProgress: %d%%\n%.2fGB of %.2fGB\nAverage speed: %.2f MB/s\nETA: %02d:%02d:%02d",
            current_copied, pct, copied_gb, total_gb, avg_speed_mb_s, est_h, est_m, est_s
        );

        /* write_log feeds the web console even with file logging off */
        write_log(g_log_path,
                  "Progress: %d%% Copied: %.2f/%.2f GB Remaining: %.2f GB "
                  "Average speed: %.2f MB/s ETA: %02d:%02d:%02d",
                  pct, copied_gb, total_gb, (double)remaining_bytes/(1024.0*1024.0*1024.0),
                  avg_speed_mb_s, est_h, est_m, est_s);
    }
    return NULL;
}