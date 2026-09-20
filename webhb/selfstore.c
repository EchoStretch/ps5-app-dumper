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
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "webhb.h"

/* pldmgr's web server; the upload is a plain POST with the file as body,
   the same request its own page sends. */
#define PLDMGR_PORT 8084

/* Where pldmgr keeps what it stores. */
#ifndef STORE_ROOT          /* the host harness points this at a scratch folder */
#define STORE_ROOT "/data/pldmgr/"
#endif

int self_store_available(void)
{
    return self_elf_len > 0;
}

/* Only a payload manager's own storage, and only a file of our name: the
   path comes in over HTTP. */
static int path_is_ours(const char *path)
{
    if (!path || strstr(path, "..")) return 0;
    if (strncmp(path, STORE_ROOT, strlen(STORE_ROOT)) != 0 && strncmp(path, "/mnt/usb", 8) != 0) return 0;

    const char *name = strrchr(path, '/');
    return name && strcmp(name + 1, whb_elf_name()) == 0;
}

int self_store_matches(const char *path)
{
    if (!self_store_available() || !path_is_ours(path)) return -1;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    /* an ELF of ours is a megabyte; anything far off is not worth reading */
    if ((size_t)st.st_size < self_elf_len || st.st_size > 16 * 1024 * 1024) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    size_t size = (size_t)st.st_size;
    unsigned char *buf = malloc(size);
    int same = -1;

    if (buf && fread(buf, 1, size, f) == size) {
        same = 0;
        /* equal, or this build one stage later, which carries the copy inside */
        for (size_t off = 0; off + self_elf_len <= size; off++) {
            if (buf[off] == self_elf[0] && memcmp(buf + off, self_elf, self_elf_len) == 0) {
                same = 1;
                break;
            }
            if (size == self_elf_len) break;   /* same size: only offset 0 counts */
        }
    }

    free(buf);
    fclose(f);
    return same;
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int self_store_to_pldmgr(char *err, size_t err_size)
{
    #define FAIL(...) do { if (err && err_size) snprintf(err, err_size, __VA_ARGS__); \
                           if (fd >= 0) close(fd); return -1; } while (0)
    int fd = -1;

    if (!self_store_available())
        FAIL("this copy was started from a stored file - there is nothing to store");

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) FAIL("no socket (%s)", strerror(errno));

    struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PLDMGR_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        FAIL("Payload Manager is not running on this console");

    char head[320];
    int n = snprintf(head, sizeof(head),
                     "POST /manage:upload?filename=%s HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%d\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n", whb_elf_name(), PLDMGR_PORT, self_elf_len);

    if (send_all(fd, head, (size_t)n) != 0 || send_all(fd, self_elf, self_elf_len) != 0)
        FAIL("the upload to Payload Manager broke off (%s)", strerror(errno));

    char reply[512];
    size_t got = 0;
    ssize_t r;
    while (got + 1 < sizeof(reply) && (r = recv(fd, reply + got, sizeof(reply) - got - 1, 0)) > 0)
        got += (size_t)r;
    reply[got] = '\0';

    int status = 0;
    if (sscanf(reply, "HTTP/%*d.%*d %d", &status) != 1)
        FAIL("Payload Manager did not answer the upload");
    if (status < 200 || status >= 300)
        FAIL("Payload Manager refused the upload (HTTP %d)", status);

    close(fd);
    write_log(g_log_path, "Stored %s in Payload Manager (%zu bytes)", whb_elf_name(), self_elf_len);
    return 0;

    #undef FAIL
}
