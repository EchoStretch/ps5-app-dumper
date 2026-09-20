/* Copyright (C) 2026 slopmaster33

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

/* Klog Viewer: the console's kernel log in a browser. The second web homebrew
   on webhb/, and the proof that the core carries more than the dumper: all
   that is written here is where the lines come from, one setting and one
   route. Server, page kit, settings, access code, single instance, a place
   in Payload Manager and surviving rest mode come with the core.

   /dev/klog hands every line to one reader only: while another klog server
   runs, the two share the lines between them. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include "webhb.h"

#define KLOGVIEW_VERSION "0.1"

/* embedded at build time (webhb/webhb.mk) */
extern const unsigned char web_index_html[];
extern const size_t        web_index_html_len;
extern const unsigned char app_icon_png[];
extern const size_t        app_icon_png_len;

/* ------------------------------------------------------------------ */
/*  Settings                                                           */
/* ------------------------------------------------------------------ */

static const whb_cfg_key_t g_keys[] = {
    { .ini_name = "save_to_file", .web_name = "saveToFile", .type = WHB_CFG_BOOL, .def = 0,
      .comment = "; === Kernel log ===\n"
                 "; save_to_file = 1 -> also write every line to logs/klog.txt next to this file\n" },
    WHB_CFG_STD_WEB_PORT,
    WHB_CFG_STD_LOGGING,
    WHB_CFG_STD_ACCESS,
};

/* ------------------------------------------------------------------ */
/*  Where the lines come from                                          */
/* ------------------------------------------------------------------ */

static FILE *g_file = NULL;          /* logs/klog.txt while save_to_file is on */
static pthread_mutex_t g_file_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Opens or closes the file as the setting says. Called by the core whenever
   the settings may have changed. */
static void settings_changed(void)
{
    int want = whb_config_int("save_to_file", 0);

    pthread_mutex_lock(&g_file_mtx);
    if (want && !g_file && get_app_data_path()[0]) {
        char path[256];
        snprintf(path, sizeof(path), "%s/logs/klog.txt", get_app_data_path());
        g_file = fopen(path, "a");
    } else if (!want && g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    pthread_mutex_unlock(&g_file_mtx);
}

static void take_line(const char *line)
{
    if (!line[0]) return;
    log_ring_push(line);              /* the page's live console reads the ring */

    pthread_mutex_lock(&g_file_mtx);
    if (g_file) fprintf(g_file, "%s\n", line);
    pthread_mutex_unlock(&g_file_mtx);
}

static void flush_file(void)
{
    pthread_mutex_lock(&g_file_mtx);
    if (g_file) fflush(g_file);
    pthread_mutex_unlock(&g_file_mtx);
}

#ifdef WHB_HOST
/* no kernel log on this machine: a few lines that look like one */
static void *reader_thread(void *arg)
{
    (void)arg;
    static const char *const fake[] = {
        "<118>[SceShellCore] appId=0x60000012 state=foreground",
        "[klog] vm_fault: pid 87 (SceShellUI) page not present - recovered",
        "<118>[SceLncService] launch request titleId=PPSA01234",
        "[usb] ugen0.3: <SanDisk Ultra> at usbus0",
        "WARNING: pfs mount took 412 ms",
        "<118>[SceNetCtl] link state changed: up",
        "# A user thread receives a fatal signal - error: SIGSEGV (example)",
    };
    for (unsigned n = 0; ; n++) {
        char line[LOG_LINE_MAX];
        snprintf(line, sizeof(line), "%s  #%u", fake[n % (sizeof(fake) / sizeof(fake[0]))], n);
        take_line(line);
        flush_file();
        usleep(700000);
    }
    return NULL;
}
#else
static void *reader_thread(void *arg)
{
    (void)arg;
    int fd = open("/dev/klog", O_RDONLY);
    if (fd < 0) {
        write_log(g_log_path, "Klog: /dev/klog cannot be opened - nothing to show");
        return NULL;
    }

    char buf[4096], line[LOG_LINE_MAX];
    size_t len = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            /* a line longer than the ring's is cut into pieces, not lost */
            if (c == '\n' || len == sizeof(line) - 1) {
                line[len] = '\0';
                take_line(line);
                len = 0;
                if (c == '\n') continue;
            }
            line[len++] = c;
        }
        flush_file();
    }

    write_log(g_log_path, "Klog: reading /dev/klog ended");
    close(fd);
    return NULL;
}
#endif

/* ------------------------------------------------------------------ */
/*  The one route of its own                                           */
/* ------------------------------------------------------------------ */

/* Puts a line of one's own between the kernel's - "pressed the button now" -
   so that what follows can be found again. */
static void handle_mark(whb_req_t *req)
{
    char stamp[16], line[LOG_LINE_MAX];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime(&now));
    snprintf(line, sizeof(line), "======== %s  %s ========", stamp, whb_param(req, "text", "mark"));
    take_line(line);
    flush_file();
    whb_send_json(req, 200, "{\"marked\":true}");
}

/* ------------------------------------------------------------------ */

static const whb_app_t *klogview_app(void)
{
    static whb_app_t app = {
        .name          = "Klog Viewer",
        .short_name    = "Klog Viewer",
        .version       = KLOGVIEW_VERSION,
        .process_name  = "klogview.elf",
        .data_dirname  = "klogview",
        .elf_basename  = "klogview",
        .tile_title_id = "KLOG00001",
        .default_port  = 8181,     /* out of the way of the dumper's 8081-8090 */
    };

    /* the generated lengths are variables, not constants */
    app.page     = web_index_html;
    app.page_len = web_index_html_len;
    app.icon_png = app_icon_png;
    app.icon_len = app_icon_png_len;
    return &app;
}

int main(void)
{
    whb_config_register(g_keys, (int)(sizeof(g_keys) / sizeof(g_keys[0])));
    whb_config_on_change(settings_changed);

    if (whb_start(klogview_app()) != 0) return 0;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, reader_thread, NULL);
    pthread_attr_destroy(&attr);

    whb_route("POST", "/api/klog/mark", handle_mark);

    int port = 0;
#ifdef WHB_HOST
    if (getenv("WHB_PORT")) port = atoi(getenv("WHB_PORT"));
#endif
    return whb_serve(port) == 0 ? 0 : 1;
}
