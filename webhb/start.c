/* Copyright (C) 2025 EchoStretch
   Copyright (C) 2026 slopmaster33

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

/* The start of a web homebrew on the console, up to the point where the app
   decides what to do with itself. */

#include <stdio.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include "webhb.h"

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

int whb_start(const whb_app_t *app)
{
    whb_app_set(app);

    /* first of all, so that whatever happens next can be attributed */
    instance_claim_name();

    if (whb_app()->on_start) whb_app()->on_start();

    printf_notification("%s v%s", whb_app()->name, whb_app()->version);
    log_host_process();

    /* One pass over the mount points so config.ini can be read; the web UI
       rescans on its own once a drive shows up later. */
    whb_config_init();

    /* Sending the payload again replaces the copy that is running, rather
       than putting a second one next to it on the next free port - unless
       that copy is busy, which a careless resend must not destroy. */
    int busy_port = 0;
    if (instance_take_over(whb_config_int("web_port", whb_app()->default_port), &busy_port) != 0) {
        printf_notification("%s is already running and busy with %s\n"
                            "It stays as it is: port %d", whb_app()->name,
                            whb_app()->busy_with ? whb_app()->busy_with : "its work", busy_port);
        return 1;
    }
    return 0;
}
