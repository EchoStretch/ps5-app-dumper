#include "utils.h"
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

size_t folder_size_current = 0;
size_t total_bytes_copied = 0;
char current_copied[256] = {0};
int progress_thread_run = 1;
time_t copy_start_time = 0;

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

#if ENABLE_LOGGING
int write_log(const char *log_file_path, const char *fmt, ...)
{
    FILE *f = fopen(log_file_path, "a");
    if (!f) return -1;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fprintf(f, "\n");
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return 0;
}
#else
static inline int write_log(const char *log_file_path, const char *fmt, ...) { return 0; }
#endif

void printf_notification(const char *fmt, ...)
{
    SceNotificationRequest noti;
    memset(&noti, 0, sizeof(noti));

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(noti.message, sizeof(noti.message), fmt, ap);
    va_end(ap);

    noti.type = 0;
    noti.use_icon_image_uri = 1;
    noti.target_id = -1;
    strncpy(noti.uri, "cxml://psnotification/tex_icon_system", sizeof(noti.uri)-1);

    sceKernelSendNotificationRequest(0, &noti, sizeof(noti), 0);
    printf("%s\n", noti.message);
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

typedef struct async_result {
    int64_t ret;
    uint32_t state;
} async_result_t;

typedef struct async_request {
    off_t   off;
    size_t  len;
    void   *buf;
    async_result_t *res;
    int     fd;
} async_request_t;

static int fs_nread(int fd, void *buf, size_t n)
{
    ssize_t r = read(fd, buf, n);
    if (r < 0) return -1;
    if ((size_t)r != n) { errno = EIO; return -1; }
    return 0;
}

static int fs_nwrite(int fd, const void *buf, size_t n)
{
    ssize_t r = write(fd, buf, n);
    if (r < 0) return -1;
    if ((size_t)r != n) { errno = EIO; return -1; }
    return 0;
}

static int fs_ncopy(int fd_in, int fd_out, size_t size)
{
    char buf[0x4000];
    size_t copied = 0;

    while (copied < size) {
        size_t n = size - copied;
        if (n > sizeof(buf)) n = sizeof(buf);

        if (fs_nread(fd_in, buf, n)) return -1;
        if (fs_nwrite(fd_out, buf, n)) return -1;

        total_bytes_copied += n;
        copied += n;
    }
    return 0;
}

static int fs_ncopy_chunk(int fd_in, int fd_out, size_t n, off_t off)
{
    void *buf;
    async_request_t req = {0};
    async_result_t  res = {0};
    int id, state;

    buf = mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd_in, off);
    if (buf == MAP_FAILED) return -1;

    req.fd  = fd_out;
    req.off = off;
    req.buf = buf;
    req.len = n;
    req.res = &res;

    if (sceKernelAioSubmitWriteCommands(&req, 1, 1, &id)) {
        munmap(buf, n);
        return -1;
    }

    if (sceKernelAioWaitRequest(id, &state, NULL)) {
        sceKernelAioDeleteRequest(id, NULL);
        munmap(buf, n);
        return -1;
    }

    sceKernelAioDeleteRequest(id, NULL);
    munmap(buf, n);
    return 0;
}

static int fs_ncopy_large(int fd_in, int fd_out, size_t size)
{
    size_t copied = 0;

    while (copied < size) {
        size_t n = size - copied;
        if (n > BUF_SIZE) n = BUF_SIZE;

        if (fs_ncopy_chunk(fd_in, fd_out, n, copied)) return -1;

        total_bytes_copied += n;

        copied += n;
    }
    return 0;
}

int fs_copy_file(const char *src, const char *dst)
{
    struct stat st;
    int src_fd = -1, dst_fd = -1;
    int ret = -1;

    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode))
        goto cleanup;

    src_fd = open(src, O_RDONLY);
    if (src_fd < 0) goto cleanup;

    dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode | 0600);
    if (dst_fd < 0) goto cleanup;

    /* update UI string */
    strncpy(current_copied, src, sizeof(current_copied)-1);
    current_copied[sizeof(current_copied)-1] = '\0';

    if (st.st_size < 0x100000) {
        ret = fs_ncopy(src_fd, dst_fd, st.st_size);
    } else {
        ret = fs_ncopy_large(src_fd, dst_fd, st.st_size);
    }

cleanup:
    if (dst_fd >= 0) close(dst_fd);
    if (src_fd >= 0) close(src_fd);
    return ret;
}

int copy_file_track(const char *src, const char *dst)
{
    if (copy_start_time == 0) copy_start_time = time(NULL);
    return fs_copy_file(src, dst);
}

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

void size_walker(const char *path, size_t *acc)
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

void *progress_status_func(void *arg)
{
    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/%s", "/mnt/usb0/data", "log.txt");

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
        double total_gb   = (double)folder_size_current / (1024.0*1024.0*1024.0);

        int est_h = (int)(est_sec / 3600);
        int est_m = (int)((est_sec - est_h*3600)/60);
        int est_s = (int)(est_sec - est_h*3600 - est_m*60);

        printf_notification(
            "Copying: %s\nProgress: %d%%\n%.2fGB of %.2fGB\nAverage speed: %.2f MB/s\nETA: %02d:%02d:%02d",
            current_copied, pct, copied_gb, total_gb, avg_speed_mb_s, est_h, est_m, est_s
        );

#if ENABLE_LOGGING
        write_log(logpath,
                  "Progress: %d%% Copied: %.2f/%.2f GB Remaining: %.2f GB "
                  "Average speed: %.2f MB/s ETA: %02d:%02d:%02d",
                  pct, copied_gb, total_gb, (double)remaining_bytes/(1024.0*1024.0*1024.0),
                  avg_speed_mb_s, est_h, est_m, est_s);
#endif
    }
    return NULL;
}