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
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "app_launch.h"
#include "app_scan.h"
#include "dump_job.h"
#include "routes.h"
#include "ps4_dumper.h"
#include "ps5_dumper.h"
#include "version.h"
#include "utils.h"

#define VERSION DUMPER_VERSION

/* ------------------------------------------------------------------ */
/*  Headless mode - dump the running title and exit                    */
/* ------------------------------------------------------------------ */

static int run_headless(const dumper_config_t *cfg)
{
    /* looks again where our files live each time: that is what finds the drive */
    for (;;) {
        whb_config_init();
        if (storage_first_usb() != -1) break;
        printf_notification("Please insert USB (exFAT) into any port...");
        sleep(7);
    }

    const char *usb = get_usb_homebrew_path();
    if (!usb || !usb[0]) return 1;

    /* the dump still goes to <drive>/homebrew; its log joins the others */
    log_use_general();
    const char *logpath = g_log_path;

    write_log(logpath, "=== PS5 App Dumper v%s ===", VERSION);

    app_entry_t apps[APP_SCAN_MAX];
    int count = app_scan(apps, APP_SCAN_MAX);

    if (count <= 0) {
        write_log(logpath, "Please start the App before running the payload...");
        printf_notification("Please start the App before running the payload...");
        return 1;
    }

    /* headless mode has no way to ask, so it takes the first mount */
    const app_entry_t *app = &apps[0];

    write_log(logpath, "Detected App: %s", app->dir);
    printf_notification("Detected: %s", app->title[0] ? app->title : app->dir);

    if (app->is_ps4) {
        dump_ps4_cusa_app(SANDBOX_PATH, app->dir, app->patch_dir, usb,
                          cfg->enable_decrypter, cfg->enable_elf2fself,
                          cfg->enable_backport);
    } else {
        dump_ps5_ppsa_app(SANDBOX_PATH, app->dir, usb,
                          cfg->enable_decrypter, cfg->enable_elf2fself,
                          cfg->enable_backport);
    }

    write_log(logpath, "=== PS5 App Dumper v%s finished ===", VERSION);
    printf_notification("Dump Complete!");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  auto_start with the web UI up                                      */
/* ------------------------------------------------------------------ */

/* How long the web server gets to come up before the dump is started. */
#define AUTO_DUMP_DELAY 2

/* Dumps the running title the way the web UI's button would. The job runs
   inside the web UI's machinery on purpose: the dump shows up in the
   browser, can be stopped there, and marks this instance as busy so that
   sending the payload again does not end it. Above all the switch that
   caused it stays within reach - with a blind dump it could only be turned
   off by editing config.ini on the drive. */
static void *auto_dump_thread(void *arg)
{
    dumper_config_t *cfg = (dumper_config_t *)arg;

    sleep(AUTO_DUMP_DELAY);

    app_entry_t *apps = calloc(APP_SCAN_MAX, sizeof(*apps));
    target_entry_t *targets = calloc(TARGET_SCAN_MAX, sizeof(*targets));
    const char *problem = NULL;
    char err[160] = {0};

    if (!apps || !targets) {
        problem = "out of memory";
    } else if (app_scan(apps, APP_SCAN_MAX) <= 0) {
        problem = "no game is running";
    } else {
        /* the writable drive with the most room, as the web UI picks it;
           the console's own storage only when there is no drive at all */
        int count = target_scan(targets, TARGET_SCAN_MAX), best = -1;
        for (int i = 0; i < count; i++) {
            if (!targets[i].writable) continue;
            if (best < 0 || (targets[best].internal && !targets[i].internal) ||
                (targets[best].internal == targets[i].internal &&
                 targets[i].free_bytes > targets[best].free_bytes))
                best = i;
        }

        if (best < 0)
            problem = "no drive to dump to";
        /* never overwrites: nobody is there to be asked */
        else if (job_start(apps[0].dir, targets[best].mount, cfg, 0, err, sizeof(err)) != 0)
            problem = err[0] ? err : "the dump could not be started";
        else
            printf_notification("Auto dump: %s\nWatch or stop it in the web UI",
                                apps[0].title[0] ? apps[0].title : apps[0].dir);
    }

    if (problem) {
        write_log(g_log_path, "Auto dump skipped: %s", problem);
        printf_notification("Auto dump skipped: %s\nUse the web UI instead", problem);
    }

    free(apps);
    free(targets);
    free(cfg);
    return NULL;
}

static void schedule_auto_dump(const dumper_config_t *cfg)
{
    dumper_config_t *copy = malloc(sizeof(*copy));
    if (!copy) return;
    *copy = *cfg;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&tid, &attr, auto_dump_thread, copy) != 0) free(copy);
    pthread_attr_destroy(&attr);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    dumper_config_init();
    if (whb_start(dumper_app()) != 0) return 0;

    dumper_config_t cfg;
    cfg_snapshot(&cfg);

    /* headless is what enable_webui = 0 asks for - auto_start alone is not */
    if (!cfg.enable_webui)
        return run_headless(&cfg);

    if (cfg.auto_start)
        schedule_auto_dump(&cfg);

    routes_dumper_init();

    if (whb_serve(0) != 0) {
        printf_notification("Web UI failed to start, dumping directly instead");
        return run_headless(&cfg);
    }

    return 0;
}
