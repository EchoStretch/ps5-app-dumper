#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Check if directory exists */
int dir_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/* Check if file exists */
int file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/* Create directories recursively */
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

/* Write formatted log to file */
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

/* Return file size in bytes */
size_t size_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return (size_t)st.st_size;
    return 0;
}

/* Copy a single file */
int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;

    char dst_dir[512];
    strncpy(dst_dir, dst, sizeof(dst_dir)-1);
    dst_dir[sizeof(dst_dir)-1] = '\0';
    char *slash = strrchr(dst_dir, '/');
    if (slash) { *slash = '\0'; mkdirs(dst_dir); }

    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }

    fflush(out); fsync(fileno(out));
    fclose(in); fclose(out);
    return 0;
}

/* Recursive directory copy */
void copy_dir_recursive(const char *src, const char *dst)
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
                copy_dir_recursive(src_path, dst_path);
            else if (S_ISREG(st.st_mode))
                copy_file(src_path, dst_path);
        }
    }
    closedir(d);
}

/* Minimal notification */
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

/* Return USB path */
char *getusbpath(void)
{
    const char *p = "/mnt/usb0/data";
    if (!dir_exists(p)) mkdirs(p);

    char *ret = malloc(strlen(p)+1);
    if (!ret) return NULL;
    strcpy(ret, p);
    return ret;
}
