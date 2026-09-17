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
#include <sys/aio.h>
#include <errno.h>

#include "utils.h"

size_t folder_size_current = 0;
size_t total_bytes_copied = 0;
char current_copied[256] = {0};
int progress_thread_run = 1;
time_t copy_start_time = 0;
pthread_t progress_thread = 0;

static char g_usb_homebrew[128] = {0};

/* ------------------------------------------------------------------ */
/*  In-memory log ring                                                 */
/* ------------------------------------------------------------------ */

static char     g_log_ring[LOG_RING_CAPACITY][LOG_LINE_MAX];
static unsigned g_log_ring_seq = 0;   /* sequence number of the newest line */
static pthread_mutex_t g_log_ring_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  Cooperative abort                                                  */
/* ------------------------------------------------------------------ */

static volatile int g_abort_requested = 0;

int g_enable_logging = 1;
char g_log_path[512] = {0};
int g_split_mode = 3;  // default: split both

void log_ring_push(const char *line)
{
    if (!line || !*line) return;

    pthread_mutex_lock(&g_log_ring_mtx);
    g_log_ring_seq++;
    char *slot = g_log_ring[g_log_ring_seq % LOG_RING_CAPACITY];
    strncpy(slot, line, LOG_LINE_MAX - 1);
    slot[LOG_LINE_MAX - 1] = '\0';

    /* newlines would break the one-line-per-entry contract of the UI */
    for (char *p = slot; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';

    pthread_mutex_unlock(&g_log_ring_mtx);
}

unsigned log_ring_seq(void)
{
    pthread_mutex_lock(&g_log_ring_mtx);
    unsigned seq = g_log_ring_seq;
    pthread_mutex_unlock(&g_log_ring_mtx);
    return seq;
}

void log_ring_walk(unsigned since, log_line_cb cb, void *ctx)
{
    if (!cb) return;

    pthread_mutex_lock(&g_log_ring_mtx);
    unsigned newest = g_log_ring_seq;
    unsigned oldest = (newest > LOG_RING_CAPACITY) ? newest - LOG_RING_CAPACITY + 1 : 1;
    if (since + 1 > oldest) oldest = since + 1;

    for (unsigned seq = oldest; seq <= newest; seq++) {
        char copy[LOG_LINE_MAX];
        strncpy(copy, g_log_ring[seq % LOG_RING_CAPACITY], sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        pthread_mutex_unlock(&g_log_ring_mtx);
        cb(ctx, seq, copy);
        pthread_mutex_lock(&g_log_ring_mtx);
    }
    pthread_mutex_unlock(&g_log_ring_mtx);
}

void request_abort(void) { g_abort_requested = 1; }
void clear_abort(void)   { g_abort_requested = 0; }
int  abort_requested(void) { return g_abort_requested; }

int find_usb_and_setup(void) {
    const char *possible_mounts[] = {
        "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
        "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7"
    };
    const int num_mounts = sizeof(possible_mounts) / sizeof(possible_mounts[0]);

    for (int i = 0; i < num_mounts; ++i) {
        const char *root = possible_mounts[i];
        char homebrew[128], testfile[256], config[256];

        snprintf(homebrew, sizeof(homebrew), "%s/homebrew", root);
        snprintf(testfile, sizeof(testfile), "%s/.probe_usb", homebrew);
        snprintf(config,   sizeof(config),   "%s/config.ini", homebrew);

        g_enable_logging = read_logging_config();

        if (!dir_exists(root)) continue;

        mkdirs(homebrew);

        int fd = open(testfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd != -1) {
            if (write(fd, "PROBE", 5) == 5) {
                close(fd);
                unlink(testfile);

                strncpy(g_usb_homebrew, homebrew, sizeof(g_usb_homebrew) - 1);
                g_usb_homebrew[sizeof(g_usb_homebrew) - 1] = '\0';

                if (!file_exists(config)) {
                    dumper_config_t defaults;
                    config_defaults(&defaults);
                    config_save(&defaults);
                }

                snprintf(g_log_path, sizeof(g_log_path), "%s/log.txt", homebrew);

                if (g_enable_logging && g_log_path[0]) {
                    write_log(g_log_path,
                              "USB detected (writable) at %s – %s",
                              root, detect_fs_type(root));
                }

                return i;
            }
            close(fd);
            unlink(testfile);
        }

        if (file_exists(config)) {
            printf_notification("USB (read-only fallback): %s", root);
            strncpy(g_usb_homebrew, homebrew, sizeof(g_usb_homebrew) - 1);
            g_usb_homebrew[sizeof(g_usb_homebrew) - 1] = '\0';
            snprintf(g_log_path, sizeof(g_log_path), "%s/log.txt", homebrew);

            if (g_enable_logging && g_log_path[0]) {
                write_log(g_log_path,
                          "USB detected (read-only) at %s – %s",
                          root, detect_fs_type(root));
            }
            return i;
        }
    }

    return -1;
}

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

int read_backport_config(void)
{
    if (g_usb_homebrew[0] == '\0') return 1; 

    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.ini", g_usb_homebrew);

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
    if (g_usb_homebrew[0] == '\0') return 1;

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/config.ini", g_usb_homebrew);

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
    if (g_usb_homebrew[0] == '\0') return 3;

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/config.ini", g_usb_homebrew);

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

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

void config_defaults(dumper_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enable_decrypter   = 1;
    cfg->enable_backport    = 0;
    cfg->ps4_backport_level = 4;
    cfg->ps5_backport_level = 1;
    cfg->enable_elf2fself   = 0;
    cfg->enable_logging     = 1;
    cfg->split              = 3;
    cfg->enable_webui       = 1;
    cfg->web_port           = 8081;   /* 8080 usually belongs to websrv */
    cfg->auto_start         = 0;
    strncpy(cfg->dump_subdir, "homebrew", sizeof(cfg->dump_subdir) - 1);
}

int config_path(char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;
    if (g_usb_homebrew[0] == '\0') { out[0] = '\0'; return -1; }
    snprintf(out, out_size, "%s/config.ini", g_usb_homebrew);
    return 0;
}

/* Splits "  key = value  ; comment" into key/value, both trimmed.
   Returns 0 when the line carries a setting. */
static int config_split_line(char *line, char **key, char **value)
{
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ';' || *p == '#' || *p == '\n' || *p == '\r' || *p == '\0') return -1;

    char *eq = strchr(p, '=');
    if (!eq) return -1;
    *eq = '\0';

    char *k_end = eq - 1;
    while (k_end >= p && (*k_end == ' ' || *k_end == '\t')) *k_end-- = '\0';

    char *v = eq + 1;
    while (*v == ' ' || *v == '\t') v++;

    char *v_end = v + strlen(v) - 1;
    while (v_end >= v && (*v_end == '\n' || *v_end == '\r' ||
                          *v_end == ' '  || *v_end == '\t')) *v_end-- = '\0';

    /* strip a trailing inline comment */
    char *c = strpbrk(v, ";#");
    if (c) {
        *c = '\0';
        char *e = c - 1;
        while (e >= v && (*e == ' ' || *e == '\t')) *e-- = '\0';
    }

    *key = p;
    *value = v;
    return 0;
}

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void config_load(dumper_config_t *cfg)
{
    if (!cfg) return;
    config_defaults(cfg);

    char path[512];
    if (config_path(path, sizeof(path)) != 0) return;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256], *key, *val;
    while (fgets(line, sizeof(line), f)) {
        if (config_split_line(line, &key, &val) != 0) continue;

        if      (!strcmp(key, "enable_decrypter"))   cfg->enable_decrypter   = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "enable_backport"))    cfg->enable_backport    = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "ps4_backport_level")) cfg->ps4_backport_level = clamp_int(atoi(val), 1, 6);
        else if (!strcmp(key, "ps5_backport_level")) cfg->ps5_backport_level = clamp_int(atoi(val), 1, 10);
        else if (!strcmp(key, "enable_elf2fself"))   cfg->enable_elf2fself   = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "enable_logging"))     cfg->enable_logging     = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "split"))              cfg->split              = clamp_int(atoi(val), 0, 3);
        else if (!strcmp(key, "enable_webui"))       cfg->enable_webui       = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "web_port"))           cfg->web_port           = clamp_int(atoi(val), 1024, 65535);
        else if (!strcmp(key, "auto_start"))         cfg->auto_start         = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "dump_subdir")) {
            strncpy(cfg->dump_subdir, val, sizeof(cfg->dump_subdir) - 1);
            cfg->dump_subdir[sizeof(cfg->dump_subdir) - 1] = '\0';
        }
    }
    fclose(f);
}

int config_save(const dumper_config_t *cfg)
{
    if (!cfg) return -1;

    char path[512];
    if (config_path(path, sizeof(path)) != 0) return -1;

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f,
        "; PS5 App Dumper Config\n"
        "; Managed by the web UI - hand edits are picked up on the next start.\n"
        "\n"
        "; === Web UI ===\n"
        "; enable_webui = 1 -> serve the web interface (default)\n"
        "; enable_webui = 0 -> headless, dump the running app right away\n"
        "; auto_start   = 1 -> dump immediately even when the web UI is enabled\n"
        "; web_port          -> first TCP port tried for the web interface (default 8081)\n"
        "enable_webui = %d\n"
        "auto_start = %d\n"
        "web_port = %d\n"
        "\n"
        "; === Destination ===\n"
        "; dump_subdir -> folder below the mount point that receives the dump\n"
        "dump_subdir = %s\n"
        "\n"
        "; === Decrypt App ===\n"
        "; enable_decrypter = 1  -> decrypt ELF files (default)\n"
        "; enable_decrypter = 0  -> disable decryption\n"
        "enable_decrypter = %d\n"
        "\n"
        "; === Backport Options PS4/PS5 ===\n"
        "; enable_backport = 1 -> enable SDK patching\n"
        "; enable_backport = 0 -> disable SDK patching (default)\n"
        "; ps4_backport_level = 1-6 -> predefined SDK pair (default: 4 (PS4 9.00) )\n"
        "; ps5_backport_level = 1-10 -> predefined SDK pair (default: 1 (PS5 1.00) )\n"
        "; >>>> BACKPORTING IS FOR ADVANCED USERS MAY NOT WORK <<<<\n"
        "enable_backport = %d\n"
        "ps4_backport_level = %d\n"
        "ps5_backport_level = %d\n"
        "\n"
        "; === FSELF Files ===\n"
        "; enable_elf2fself = 1 -> enable fself ELF files\n"
        "; enable_elf2fself = 0 -> disable fself (default)\n"
        "enable_elf2fself = %d\n"
        "\n"
        "; === Logging ===\n"
        "; enable_logging = 1 -> write log.txt (default)\n"
        "; enable_logging = 0 -> disable logging\n"
        "enable_logging = %d\n"
        "\n"
        "; === PS4 Split Mode ===\n"
        "; 0 = no split (CUSAxxxxx/)\n"
        "; 1 = app only (CUSAxxxxx-app/)\n"
        "; 2 = patch only (CUSAxxxxx-patch/)\n"
        "; 3 = both split (CUSAxxxxx-app/ + CUSAxxxxx-patch/)\n"
        "split = %d\n",
        cfg->enable_webui, cfg->auto_start, cfg->web_port,
        cfg->dump_subdir[0] ? cfg->dump_subdir : "homebrew",
        cfg->enable_decrypter,
        cfg->enable_backport, cfg->ps4_backport_level, cfg->ps5_backport_level,
        cfg->enable_elf2fself,
        cfg->enable_logging,
        cfg->split);

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return 0;
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
    char msg[LOG_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* the ring backs the live console of the web UI and stays alive even
       when file logging is turned off */
    log_ring_push(msg);

    if (!g_enable_logging || !log_file_path || !log_file_path[0]) return 0;

    FILE *f = fopen(log_file_path, "a");
    if (!f) return -1;

    char timestamp[64];
    time_t t = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&t));

    fprintf(f, "[%s] %s\n", timestamp, msg);
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
    log_ring_push(noti.message);
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
        if (abort_requested()) { errno = ECANCELED; return -1; }

        size_t n = size - copied;
        if (n > sizeof(buf)) n = sizeof(buf);

        if (fs_nread(fd_in, buf, n)) return -1;
        if (fs_nwrite(fd_out, buf, n)) return -1;

        total_bytes_copied += n;
        copied += n;
    }
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
  size_t copied = 0;
  void* buf;
  ssize_t n;

  if(!(buf=malloc(BUF_SIZE))) {
    return -1;
  }

  aior.aio_buf = buf;

  while(copied < size) {
    if(abort_requested()) {
      free(buf);
      errno = ECANCELED;
      return -1;
    }

    if(copied + aior.aio_nbytes > size) {
      aior.aio_nbytes = size - copied;
    }

    if(aio_read(&aior) < 0) {
      free(buf);
      return -1;
    }

    aio_suspend(&(const struct aiocb*){&aior}, 1, 0);
    if((n=aio_return(&aior)) < 0) {
      free(buf);
      return -1;
    }

    if(n != aior.aio_nbytes) {
      free(buf);
      return -1;
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
	copied += n;
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
        if (abort_requested()) break;
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

        printf_notification(
            "Copying: %s\nProgress: %d%%\n%.2fGB of %.2fGB\nAverage speed: %.2f MB/s\nETA: %02d:%02d:%02d",
            current_copied, pct, copied_gb, total_gb, avg_speed_mb_s, est_h, est_m, est_s
        );

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