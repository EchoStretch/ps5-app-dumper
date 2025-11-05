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
#include <sys/aio.h>
#include <errno.h>

size_t folder_size_current = 0;
size_t total_bytes_copied = 0;
char current_copied[256] = {0};
int progress_thread_run = 1;
time_t copy_start_time = 0;

static char g_usb_homebrew[128] = {0};

int g_enable_logging = 1;
char g_log_path[512] = {0};

int find_usb_and_setup(void) {
    for (int i = 0; i < 8; i++) {
        char root[32], homebrew[128], testfile[256];
        snprintf(root, sizeof(root), "/mnt/usb%d", i);
        snprintf(homebrew, sizeof(homebrew), "%s/homebrew", root);
        snprintf(testfile, sizeof(testfile), "%s/.usb_test", homebrew);

        if (!dir_exists(root)) continue;

        int fd = open(testfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) continue;

        if (write(fd, "USB_TEST", 8) != 8) {
            close(fd);
            continue;
        }

        close(fd);
        unlink(testfile);

        mkdirs(homebrew);

        char config[256];
        snprintf(config, sizeof(config), "%s/config.ini", homebrew);
        if (!file_exists(config)) {
            FILE *f = fopen(config, "w");
            if (f) {
                fprintf(f, "; PS5 App Dumper Config\n");
                fprintf(f, "; decrypter = 1  -> decrypt after dump (default)\n");
                fprintf(f, "; decrypter = 0  -> skip decryption\n");
                fprintf(f, "; enable_logging = 1 -> write log.txt (default)\n");
                fprintf(f, "; enable_logging = 0 -> disable logging\n");
                fprintf(f, "decrypter = 1\n");
                fprintf(f, "enable_logging = 1\n");
                fclose(f);
            }
        }

        strncpy(g_usb_homebrew, homebrew, sizeof(g_usb_homebrew) - 1);
        g_usb_homebrew[sizeof(g_usb_homebrew) - 1] = '\0';

        // Set default log path
        snprintf(g_log_path, sizeof(g_log_path), "%s/log.txt", homebrew);

        return i;
    }
    return -1;
}

const char* get_usb_homebrew_path(void) {
    return g_usb_homebrew;
}

int read_decrypter_config(void) {
    if (g_usb_homebrew[0] == '\0') return 1;

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/config.ini", g_usb_homebrew);

    FILE *f = fopen(config_path, "r");
    if (!f) return 1;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "decrypter", 9) == 0) {
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
    if (g_usb_homebrew[0] == '\0') return 1;

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/config.ini", g_usb_homebrew);

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

int write_log(const char *log_file_path, const char *fmt, ...)
{
    if (!g_enable_logging || !log_file_path || !log_file_path[0]) return 0;

    FILE *f = fopen(log_file_path, "a");
    if (!f) return -1;

    char timestamp[64];
    time_t t = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&t));

    fprintf(f, "[%s] ", timestamp);

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

static int fs_ncopy_large(int src, int dst, size_t size)
{
  struct aiocb aior = {
    .aio_fildes = src,
    .aio_nbytes = BUF_SIZE,
    .aio_offset = 0
  };
  struct aiocb aiow = {
    .aio_fildes = dst,
    .aio_nbytes = BUF_SIZE,
    .aio_offset = 0
  };
  void* buf;
  ssize_t n;

  if(!(buf=malloc(BUF_SIZE))) {
    return -1;
  }

  aior.aio_buf = buf;

  while(1) {
    if(aio_read(&aior) < 0) {
      free(buf);
      return -1;
    }

    aio_suspend(&(const struct aiocb*){&aior}, 1, 0);
    if((n=aio_return(&aior)) < 0) {
      free(buf);
      return -1;
    }

    if(!n) {
      break;
    }

    aiow.aio_buf = aior.aio_buf;
    aiow.aio_nbytes = n;

    if(aio_write(&aiow) < 0) {
      free(buf);
      return -1;
    }

    aio_suspend(&(const struct aiocb*){&aiow}, 1, 0);
    if(aio_return(&aiow) < 0) {
      free(buf);
      return -1;
    }

    aior.aio_offset += n;
    aiow.aio_offset += n;
    total_bytes_copied += n;
  }

  free(buf);
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

        // Only log if enabled and path is set
        if (g_enable_logging && g_log_path[0]) {
            write_log(g_log_path,
                      "Progress: %d%% Copied: %.2f/%.2f GB Remaining: %.2f GB "
                      "Average speed: %.2f MB/s ETA: %02d:%02d:%02d",
                      pct, copied_gb, total_gb, (double)remaining_bytes/(1024.0*1024.0*1024.0),
                      avg_speed_mb_s, est_h, est_m, est_s);
        }
    }
    return NULL;
}