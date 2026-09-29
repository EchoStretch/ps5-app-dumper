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

/* Finding and ending ShadowMount. See shadowmount.h. */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include "shadowmount.h"
#include "utils.h"

/* "shadowmountplus.elf" on the console this was written against; the plain
   ShadowMount goes by a name that starts the same way */
#define SHADOWMOUNT_NAME "shadowmount"

int shadowmount_pid(void)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t size = 0;

    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) return 0;

    size += size / 8;   /* processes come and go between the two calls */
    char *buf = malloc(size);
    if (!buf) return 0;

    int pid = 0;
    if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
        for (char *p = buf; p + sizeof(int) <= buf + size; ) {
            struct kinfo_proc *ki = (struct kinfo_proc *)p;
            if (ki->ki_structsize <= 0) break;
            p += ki->ki_structsize;

            if (strncasecmp(ki->ki_comm, SHADOWMOUNT_NAME, strlen(SHADOWMOUNT_NAME)) == 0) {
                pid = (int)ki->ki_pid;
                break;
            }
        }
    }

    free(buf);
    return pid;
}

int shadowmount_stop(void)
{
    /* there may be more than one; each gets a moment to go */
    for (int attempt = 0; attempt < 4; attempt++) {
        int pid = shadowmount_pid();
        if (!pid) return 0;

        write_log(g_log_path, "Ending ShadowMount, pid %d - asked for in the web UI", pid);
        kill(pid, attempt < 2 ? SIGTERM : SIGKILL);
        usleep(600000);
    }
    return shadowmount_pid() ? -1 : 0;
}
