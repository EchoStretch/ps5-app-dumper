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

/* Home-screen tile installer.
 *
 * Writes a deeplink param.json and icon0.png under /user/app/<title id> and
 * registers that folder with the app-install service. The mechanism follows
 * ps5-payload-manager's app_installer.c (GPL-3.0), itself based on ftpsrv by
 * John Toernblom; the firmware 12 handling follows ShadowMountPlus.
 *
 * Nothing here is linked. Hard-linking libSceAppInstUtil and the HTTP/SSL
 * stack it pulls in ended the complete dumper on hardware even when the
 * install was never called, while a minimal payload with the same libraries
 * survived. The libraries are therefore loaded with dlopen at the moment the
 * user asks for the tile - the same rule app_launch.c follows - so dumping
 * never depends on them. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>

#include <ps5/kernel.h>

#include "app_installer.h"
#include "utils.h"

/* embedded at build time by tools/bin2c.sh (see the Makefile) */
extern const unsigned char tile_icon0_png[];
extern const size_t        tile_icon0_png_len;

#define TILE_DIR       "/user/app/" TILE_TITLE_ID
#define TILE_SCE_SYS   TILE_DIR "/sce_sys"
#define TILE_PARAM     TILE_SCE_SYS "/param.json"
#define TILE_ICON      TILE_SCE_SYS "/icon0.png"

#define SYSTEM_LIB_DIR "/system/common/lib/"

/* The app-install service does its work on the caller's stack and goes
   through the HTTP/SSL stack; a connection thread is far too small. */
#define INSTALL_STACK_SIZE (2 * 1024 * 1024)

typedef struct install_req {
    int    port;
    int    rc;
    char  *err;
    size_t err_size;
} install_req_t;

static pthread_mutex_t g_install_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_err(install_req_t *req, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void set_err(install_req_t *req, const char *fmt, ...)
{
    if (!req->err || !req->err_size) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req->err, req->err_size, fmt, ap);
    va_end(ap);
}

/* applicationCategoryType 65536 is what makes the shell treat the title as a
   deeplink shortcut rather than something with an eboot to start. The port
   is written at install time because the server may have had to move off
   its preferred one. */
static int build_param_json(int port, char *out, size_t out_size)
{
    int n = snprintf(out, out_size,
        "{\n"
        "    \"titleId\": \"" TILE_TITLE_ID "\",\n"
        "    \"applicationCategoryType\": 65536,\n"
        "    \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
        "    \"localizedParameters\": {\n"
        "        \"defaultLanguage\": \"en-US\",\n"
        "        \"en-US\": {\n"
        "            \"titleName\": \"App Dumper\"\n"
        "        }\n"
        "    }\n"
        "}\n", port);
    return (n > 0 && (size_t)n < out_size) ? n : -1;
}

static int write_file(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int ok = (fwrite(data, 1, size, f) == size);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int file_equals(const char *path, const void *data, size_t size)
{
    struct stat st;
    if (stat(path, &st) != 0 || (size_t)st.st_size != size) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    const unsigned char *want = data;
    unsigned char buf[4096];
    size_t off = 0;
    int same = 1;
    while (same && off < size) {
        size_t chunk = size - off < sizeof(buf) ? size - off : sizeof(buf);
        if (fread(buf, 1, chunk, f) != chunk || memcmp(buf, want + off, chunk) != 0)
            same = 0;
        off += chunk;
    }
    fclose(f);
    return same;
}

int tile_is_current(int port)
{
    char json[512];
    int len = build_param_json(port, json, sizeof(json));
    if (len < 0) return 0;
    return file_equals(TILE_PARAM, json, (size_t)len) &&
           file_equals(TILE_ICON, tile_icon0_png, tile_icon0_png_len);
}

/* Most hosts resolve a bare library name, some only the full path. */
static void *load_system_lib(const char *name)
{
    char path[128];
    snprintf(path, sizeof(path), SYSTEM_LIB_DIR "%s", name);

    void *handle = dlopen(path, RTLD_LAZY);
    if (!handle) handle = dlopen(name, RTLD_LAZY);
    return handle;
}

static unsigned firmware_major(void)
{
    uint32_t bcd = (kernel_get_fw_version() >> 24) & 0xffu;
    return ((bcd >> 4) & 0xfu) * 10u + (bcd & 0xfu);
}

static int register_tile(install_req_t *req)
{
    /* Registering a title makes the app-install service fetch catalog
       metadata over HTTPS. That fetch faults the caller unless the HTTP, SSL
       and net-control libraries are present in the process, so bring in the
       chain ps5-payload-manager links before the installer itself. A library
       that is missing on this firmware is not fatal on its own. */
    static const char *const support_libs[] = {
        "libSceIpmi.sprx", "libSceSsl.sprx", "libSceHttp2.sprx",
        "libSceSystemService.sprx",
    };
    for (size_t i = 0; i < sizeof(support_libs) / sizeof(support_libs[0]); i++) {
        if (!load_system_lib(support_libs[i]))
            write_log(g_log_path, "Tile: %s is not available", support_libs[i]);
    }

    void *net_ctl      = load_system_lib("libSceNetCtl.sprx");
    void *user_service = load_system_lib("libSceUserService.sprx");
    void *app_inst     = load_system_lib("libSceAppInstUtil.sprx");
    if (!app_inst) {
        set_err(req, "this console does not offer the app-install service");
        return -1;
    }

    int (*user_initialize)(int *priority) =
        user_service ? dlsym(user_service, "sceUserServiceInitialize") : NULL;
    int (*net_ctl_init)(void) =
        net_ctl ? dlsym(net_ctl, "sceNetCtlInit") : NULL;

    int (*inst_initialize)(void) = dlsym(app_inst, "sceAppInstUtilInitialize");
    int (*inst_terminate)(void)  = dlsym(app_inst, "sceAppInstUtilTerminate");
    int (*inst_all)(void *)      = dlsym(app_inst, "sceAppInstUtilAppInstallAll");
    int (*inst_title_dir)(const char *, const char *, void *) = NULL;

    /* From firmware 12 on the per-title call is unusable (ShadowMountPlus
       skips it there as well); the batch call registers the folder instead. */
    if (firmware_major() < 12)
        inst_title_dir = dlsym(app_inst, "sceAppInstUtilAppInstallTitleDir");

    if (!inst_initialize || (!inst_title_dir && !inst_all)) {
        set_err(req, "the app-install service on this firmware is not usable");
        return -1;
    }

    /* The service talks to PSN in the user's and the network's context.
       Both calls answer "already initialised" when something got there
       first, which is fine. */
    int priority = 256;
    if (user_initialize) user_initialize(&priority);
    if (net_ctl_init)    net_ctl_init();

    int rc = inst_initialize();
    if (rc) {
        write_log(g_log_path, "Tile: sceAppInstUtilInitialize = 0x%08x", rc);
        set_err(req, "the app-install service refused to start (0x%08x)", (unsigned)rc);
        return -1;
    }

    const char *how = inst_title_dir ? "AppInstallTitleDir" : "AppInstallAll";
    rc = inst_title_dir ? inst_title_dir(TILE_TITLE_ID, "/user/app/", NULL)
                        : inst_all(NULL);

    if (inst_terminate) inst_terminate();

    if (rc) {
        write_log(g_log_path, "Tile: %s = 0x%08x", how, rc);
        set_err(req, "the console rejected the shortcut (0x%08x)", (unsigned)rc);
        return -1;
    }

    write_log(g_log_path, "Tile: registered through %s", how);
    return 0;
}

static void *install_thread(void *arg)
{
    install_req_t *req = arg;
    req->rc = -1;

    char json[512];
    int len = build_param_json(req->port, json, sizeof(json));
    if (len < 0) {
        set_err(req, "internal error while building param.json");
        return NULL;
    }

    if ((mkdir(TILE_DIR, 0755) && errno != EEXIST) ||
        (mkdir(TILE_SCE_SYS, 0755) && errno != EEXIST)) {
        write_log(g_log_path, "Tile: mkdir under /user/app failed (errno %d)", errno);
        set_err(req, "could not create %s", TILE_DIR);
        return NULL;
    }

    if (write_file(TILE_PARAM, json, (size_t)len) ||
        write_file(TILE_ICON, tile_icon0_png, tile_icon0_png_len)) {
        write_log(g_log_path, "Tile: could not write the tile files (errno %d)", errno);
        set_err(req, "could not write the shortcut files");
        return NULL;
    }

    req->rc = register_tile(req);
    if (req->rc == 0) {
        write_log(g_log_path, "Tile: home-screen shortcut installed (%s, port %d)",
                  TILE_TITLE_ID, req->port);
        printf_notification("App Dumper: home-screen shortcut ready");
    }
    return NULL;
}

int tile_install(int port, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';
    if (port <= 0 || port > 65535) {
        if (err && err_size) snprintf(err, err_size, "the web server has no port yet");
        return -1;
    }

    install_req_t req = { .port = port, .rc = -1, .err = err, .err_size = err_size };

    pthread_mutex_lock(&g_install_mutex);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, INSTALL_STACK_SIZE);

    pthread_t tid;
    int started = pthread_create(&tid, &attr, install_thread, &req);
    pthread_attr_destroy(&attr);

    if (started != 0)
        set_err(&req, "could not start the installer thread");
    else
        pthread_join(tid, NULL);

    pthread_mutex_unlock(&g_install_mutex);
    return req.rc;
}
