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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "webhb.h"

/* http_server_run() walks this many ports up from the configured one. */
#define PORT_SPAN      10
/* How long an instance gets to leave after it agreed to. */
#define QUIT_GRACE_MS  4000

void instance_claim_name(void)
{
    /* Sent over elfldr the process is called after the loader's generic
       "payload.elf". Best-effort: harmless if the syscall fails. */
    syscall(SYS_thr_set_name, -1, whb_app()->process_name);
}

/* ------------------------------------------------------------------ */
/*  Talking to a copy's web UI                                         */
/* ------------------------------------------------------------------ */

/* Sends one request to 127.0.0.1:port and reads the answer. Returns the
   number of bytes read, -1 when nobody answered. */
static int local_request(int port, const char *request, char *out, size_t out_size)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int got = -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        send(fd, request, strlen(request), 0) == (ssize_t)strlen(request)) {
        size_t len = 0;
        ssize_t n;
        while (len + 1 < out_size && (n = recv(fd, out + len, out_size - len - 1, 0)) > 0)
            len += (size_t)n;
        out[len] = '\0';
        got = (int)len;
    }

    close(fd);
    return got;
}

/* Copies built before the core's routes moved below WHB_API answer the old
   paths only; they are asked second. */
static const char *const g_api_roots[] = { WHB_API, "/api" };
#define API_ROOTS ((int)(sizeof(g_api_roots) / sizeof(g_api_roots[0])))

static int ask(int port, const char *method, const char *root, const char *what, char *out, size_t out_size)
{
    char request[160];
    snprintf(request, sizeof(request), "%s %s%s HTTP/1.0\r\nContent-Length: 0\r\n\r\n", method, root, what);
    return local_request(port, request, out, out_size);
}

/* 1 busy, 0 idle, -1 when the port does not belong to a copy of this
   payload. Web homebrews sit on neighbouring ports and answer the same
   routes, so a copy is told by the file name its "self" route gives - it
   starts with the app's elf_basename - and never by the shape of an answer.
   *root gets the paths that copy understands. */
static int instance_state(int port, const char **root)
{
    /* the status answer carries the log, which can be long */
    char *buf = malloc(32768);
    if (!buf) return -1;

    char mark[96];
    snprintf(mark, sizeof(mark), "\"file\":\"%s_v", whb_app()->elf_basename);

    int state = -1;
    for (int i = 0; i < API_ROOTS && state < 0; i++) {
        int n = ask(port, "GET", g_api_roots[i], "/self", buf, 32768);
        if (n <= 0) break;                       /* nobody there at all */
        if (!strstr(buf, mark)) continue;

        n = ask(port, "GET", g_api_roots[i], "/status", buf, 32768);
        if (n > 0 && strstr(buf, "\"busy\":")) {
            state = strstr(buf, "\"busy\":true") ? 1 : 0;
            if (root) *root = g_api_roots[i];
        }
    }

    free(buf);
    return state;
}

/* ------------------------------------------------------------------ */
/*  Finding copies among the processes                                 */
/* ------------------------------------------------------------------ */

/* Ends every other process that carries this payload's name. Returns how
   many there were. */
static int kill_namesakes(void)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t size = 0;

    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0) return 0;

    size += size / 8;   /* processes come and go between the two calls */
    char *buf = malloc(size);
    if (!buf) return 0;

    int found = 0;
    pid_t self = getpid();

    if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
        for (char *p = buf; p + sizeof(int) <= buf + size; ) {
            struct kinfo_proc *ki = (struct kinfo_proc *)p;
            if (ki->ki_structsize <= 0) break;
            p += ki->ki_structsize;

            if (ki->ki_pid == self) continue;
            if (strncmp(ki->ki_comm, whb_app()->process_name, sizeof(ki->ki_comm)) != 0) continue;

            write_log(g_log_path, "Ending the previous instance, pid %d", (int)ki->ki_pid);
            if (kill(ki->ki_pid, SIGKILL) == 0) found++;
        }
    }

    free(buf);
    return found;
}

/* ------------------------------------------------------------------ */

int instance_take_over(int web_port, int *busy_port)
{
    int asked = 0;

    /* The web UI is asked first: it knows whether a dump is running, and a
       copy built before the process had a name can only be found this way. */
    for (int port = web_port; port < web_port + PORT_SPAN; port++) {
        const char *root = WHB_API;
        int state = instance_state(port, &root);
        if (state < 0) continue;

        if (state == 1) {
            if (busy_port) *busy_port = port;
            return -1;
        }

        char reply[512];
        ask(port, "POST", root, "/quit", reply, sizeof(reply));
        write_log(g_log_path, "Asked the instance on port %d to quit", port);
        asked++;
    }

    /* give them a moment to close their sockets */
    for (int waited = 0; asked && waited < QUIT_GRACE_MS; waited += 250) {
        int still = 0;
        for (int port = web_port; port < web_port + PORT_SPAN; port++)
            if (instance_state(port, NULL) >= 0) still++;
        if (!still) break;
        usleep(250000);
    }

    /* whoever is left did not listen, or has no web UI (headless mode) */
    if (kill_namesakes() > 0) usleep(500000);

    return 0;
}
