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
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h>

#include <ps5/kernel.h>

#include "app_launch.h"
#include "utils.h"

/* Launch approach and this struct layout follow the ps5-payload-dev
   homebrew launchers (studied from soniciso/elf-arsenal, GPLv3+); the SDK
   ships no header for these system-service entry points. */
typedef struct app_launch_ctx {
    uint32_t structsize;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} app_launch_ctx_t;

/* Resolved at runtime rather than linked: a payload that hard-links these
   libraries refuses to start when they are missing from the host process,
   which would take dumping down with it. */
static int (*p_user_initialize)(void *unused);
static int (*p_user_get_foreground)(uint32_t *user_id);
static int (*p_get_running_bigapp)(void);
static int (*p_kill_app)(int app_id, int how, int reason, int core_dump);
static int (*p_launch_app)(const char *title_id, char **argv, app_launch_ctx_t *ctx);
/* The low level entry point the shell itself uses. sceSystemServiceLaunchApp
   answers 0x80940005 for sideloaded titles, this one does not. */
static int (*p_lnc_initialize)(void);
static int (*p_lnc_launch_app)(const char *title_id, char **argv, app_launch_ctx_t *ctx);

/* The authority the launch services expect from their caller. */
#define SHELLCORE_AUTHID 0x4801000000000013ULL

static pthread_once_t g_resolve_once = PTHREAD_ONCE_INIT;
static int            g_available = 0;
static int            g_resolved_once_done = 0;

static void resolve_symbols(void)
{
    void *system_service = dlopen("libSceSystemService.sprx", RTLD_LAZY);
    void *user_service   = dlopen("libSceUserService.sprx", RTLD_LAZY);

    if (system_service) {
        p_get_running_bigapp = dlsym(system_service, "sceSystemServiceGetAppIdOfRunningBigApp");
        p_kill_app           = dlsym(system_service, "sceSystemServiceKillApp");
        p_launch_app         = dlsym(system_service, "sceSystemServiceLaunchApp");
        p_lnc_initialize     = dlsym(system_service, "sceLncUtilInitialize");
        p_lnc_launch_app     = dlsym(system_service, "sceLncUtilLaunchApp");
    }

    if (user_service) {
        p_user_initialize     = dlsym(user_service, "sceUserServiceInitialize");
        p_user_get_foreground = dlsym(user_service, "sceUserServiceGetForegroundUser");
    }

    /* Without this the user service hands out no foreground user, the launch
       goes out with no player context, and the console rejects it. */
    if (p_user_initialize) {
        int rc = p_user_initialize(NULL);
        if (rc && rc != 0x80960003 /* already initialised */)
            write_log(g_log_path, "Web UI: sceUserServiceInitialize = 0x%x", rc);
    }

    /* The foreground user is optional; without the launcher there is nothing
       to offer. */
    g_available = ((p_lnc_launch_app || p_launch_app) && p_get_running_bigapp) ? 1 : 0;

    g_resolved_once_done = 1;

    if (!g_available)
        write_log(g_log_path,
                  "Web UI: starting titles is unavailable (system service not reachable)");
}

void app_launch_init(void)
{
    /* Only the credential change happens this early. It is a plain kernel
       write that cannot block, and it has to be in place before anything
       talks to the system services.

       Loading those libraries is deliberately NOT done here: dlopen and
       sceUserServiceInitialize reach out to system services that may be
       busy or absent while a game is running, and a payload that stalls
       here never reaches its web server. They are resolved on first use
       instead - by then the server is up and the console is idle enough
       for the user to be asking for a launch. */
    if (kernel_set_ucred_authid(-1, SHELLCORE_AUTHID) != 0)
        write_log(g_log_path, "Web UI: could not claim launch authority");
}

int app_launch_available(void)
{
    pthread_once(&g_resolve_once, resolve_symbols);
    return g_available;
}

int app_launch_probably_available(void)
{
    /* Optimistic until proven otherwise: the listing is polled every few
       seconds and must not pull in the system service libraries. Whether a
       launch truly works is settled when one is requested. */
    return g_resolved_once_done ? g_available : 1;
}

/* Turns what the launch services return into something a user can act on. */
static const char *launch_error_text(int rc)
{
    switch ((unsigned)rc) {
        case 0x8094000cu:
            return "the console still has this title open - close it on the console and try again";
        case 0x80940005u:
            return "the console refused permission to start this title";
        case 0x80940003u:
            return "the console does not know this title";
        case 0x80020060u:
            return "the console rejected this title - it will not start from the home screen either";
    }
    return NULL;
}

/* Guards the system call against anything that is not a title id. */
static int valid_title_id(const char *id)
{
    if (!id || strlen(id) != 9) return 0;
    if (strncmp(id, "PPSA", 4) != 0 && strncmp(id, "CUSA", 4) != 0) return 0;

    for (int i = 4; i < 9; i++)
        if (id[i] < '0' || id[i] > '9') return 0;

    return 1;
}

int app_running_id(void)
{
    if (!app_launch_available()) return 0;
    return p_get_running_bigapp();
}

int app_launch_title(const char *title_id, int close_running,
                     char *err, size_t err_size)
{
    #define FAIL(...) do { if (err && err_size) snprintf(err, err_size, __VA_ARGS__); \
                           return -1; } while (0)

    if (!app_launch_available()) FAIL("this console build cannot start titles");
    if (!valid_title_id(title_id)) FAIL("not a title id");

    int app_id = p_get_running_bigapp();
    if (app_id > 0) {
        if (!close_running)
            FAIL("another game is running");

        if (!p_kill_app) FAIL("cannot close the running game");

        write_log(g_log_path, "Web UI: closing running app %d", app_id);
        int rc = p_kill_app(app_id, -1, 0, 0);
        if (rc) FAIL("could not close the running game (0x%x)", rc);
    }

    /* Launch on behalf of the signed-in player; without a user the system
       service falls back to its own default. */
    app_launch_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    uint32_t user_id = 0xFFFFFFFFu;
    int have_ctx = 0;
    int user_rc = p_user_get_foreground ? p_user_get_foreground(&user_id) : -1;

    if (user_rc == 0 && user_id != 0xFFFFFFFFu) {
        ctx.structsize = sizeof(ctx);
        ctx.user_id = user_id;
        have_ctx = 1;
    } else {
        write_log(g_log_path, "Web UI: no foreground user (rc=0x%x, id=0x%x)",
                  user_rc, user_id);
    }

    char *argv[1] = { NULL };
    int rc = 0, lnc_rc = 0;

    /* Try the shell's own entry point first; the system service rejects
       sideloaded titles outright. */
    if (p_lnc_launch_app) {
        if (p_lnc_initialize) {
            int init_rc = p_lnc_initialize();
            if (init_rc)
                write_log(g_log_path, "Web UI: sceLncUtilInitialize = 0x%x", init_rc);
        }
        rc = lnc_rc = p_lnc_launch_app(title_id, argv, have_ctx ? &ctx : NULL);
        if (rc < 0)
            write_log(g_log_path, "Web UI: sceLncUtilLaunchApp(%s) = 0x%x", title_id, rc);
    } else {
        lnc_rc = 0;
        rc = -1;
    }

    if (rc < 0 && p_launch_app) {
        rc = p_launch_app(title_id, argv, have_ctx ? &ctx : NULL);
        if (rc < 0)
            write_log(g_log_path, "Web UI: sceSystemServiceLaunchApp(%s) = 0x%x", title_id, rc);
    }

    if (rc < 0) {
        const char *known = launch_error_text(lnc_rc ? lnc_rc : rc);
        if (known) FAIL("%s (0x%x)", known, lnc_rc ? lnc_rc : rc);

        FAIL("the console refused to start %s (lnc 0x%x, service 0x%x, user %s)",
             title_id, lnc_rc, rc, have_ctx ? "ok" : "missing");
    }

    write_log(g_log_path, "Web UI: launched %s", title_id);
    printf_notification("Starting %s...", title_id);
    return 0;

    #undef FAIL
}
