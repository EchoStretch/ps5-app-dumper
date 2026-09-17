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
#include <unistd.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include "app_launch.h"
#include "app_scan.h"
#include "http_server.h"
#include "ps4_dumper.h"
#include "ps5_dumper.h"
#include "utils.h"

#define VERSION "1.11"

/* ------------------------------------------------------------------ */
/*  Headless mode - dump the running title and exit                    */
/* ------------------------------------------------------------------ */

static int run_headless(const dumper_config_t *cfg)
{
    while (find_usb_and_setup() == -1) {
        printf_notification("Please insert USB (exFAT) into any port...");
        sleep(7);
    }

    const char *usb = get_usb_homebrew_path();
    if (!usb || !usb[0]) return 1;

    char logpath[512];
    snprintf(logpath, sizeof(logpath), "%s/log.txt", usb);
    strncpy(g_log_path, logpath, sizeof(g_log_path) - 1);

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

/* Names the process the payload was loaded into. A payload lives only as
   long as its host, so when the web UI vanishes the moment a game starts,
   this line says which process took it down. */
static void log_host_process(void)
{
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };

    if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && kp.ki_comm[0])
        write_log(g_log_path, "Running as pid %d inside \"%s\"", (int)getpid(), kp.ki_comm);
    else
        write_log(g_log_path, "Running as pid %d (host unknown)", (int)getpid());
}

int main(void)
{
    /* before anything talks to the system services */
    app_launch_init();

    printf_notification("PS5 App Dumper v%s", VERSION);
    log_host_process();

    dumper_config_t cfg;

    /* One pass over the mount points so config.ini can be read; the web UI
       rescans on its own once a drive shows up later. */
    find_usb_and_setup();
    config_load(&cfg);

    g_enable_logging = cfg.enable_logging;
    g_split_mode = cfg.split;

    if (!cfg.enable_webui || cfg.auto_start)
        return run_headless(&cfg);

    if (http_server_run(cfg.web_port) != 0) {
        printf_notification("Web UI failed to start, dumping directly instead");
        return run_headless(&cfg);
    }

    return 0;
}
