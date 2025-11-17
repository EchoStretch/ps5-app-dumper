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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "pfs.h"
#include "utils.h"

#define BUFFER_SIZE   0x100000   /* 1 MB */

/* ----------------------------------------------------------------- */
/*  Helper: safe string concatenation                                */
/* ----------------------------------------------------------------- */
static char *strcat_alloc(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    char *out = malloc(len_a + len_b + 2);
    if (!out) return NULL;
    memcpy(out, a, len_a);
    if (len_a && len_b) out[len_a] = '/';
    memcpy(out + len_a + (len_a && len_b ? 1 : 0), b, len_b);
    out[len_a + (len_a && len_b ? 1 : 0) + len_b] = '\0';
    return out;
}

/* ----------------------------------------------------------------- */
/*  Copy chunk with progress                                         */
/* ----------------------------------------------------------------- */
static int copy_chunk(int pfs_fd, uint64_t src_off, const char *dst_path,
                      uint64_t size, pfs_progress_cb progress)
{
    int out_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (out_fd < 0) return -1;

    char *buf = malloc(BUFFER_SIZE);
    if (!buf) { close(out_fd); return -1; }

    uint64_t copied = 0;
    while (size) {
        size_t chunk = (size > BUFFER_SIZE) ? BUFFER_SIZE : (size_t)size;
        if (lseek(pfs_fd, src_off + copied, SEEK_SET) < 0) break;
        if (read(pfs_fd, buf, chunk) != (ssize_t)chunk) break;
        if (write(out_fd, buf, chunk) != (ssize_t)chunk) break;

        copied += chunk;
        size   -= chunk;

        // === PROGRESS UPDATE ===
        total_bytes_copied += chunk;
        if (progress) {
            strncpy(current_copied, dst_path, sizeof(current_copied) - 1);
            current_copied[sizeof(current_copied) - 1] = '\0';
            progress(total_bytes_copied, folder_size_current, current_copied);
        }
    }

    free(buf);
    close(out_fd);
    return (size == 0) ? 0 : -1;
}

/* ----------------------------------------------------------------- */
/*  Recursive parser with progress                                   */
/* ----------------------------------------------------------------- */
static void parse_dir(int pfs_fd, const struct pfs_header_t *hdr,
                      const struct di_d32 *inodes,
                      uint32_t ino, int level,
                      const char *parent_path,
                      int dry_run,
                      pfs_progress_cb progress,
                      uint64_t *total_size)
{
    const struct di_d32 *node = &inodes[ino];

    for (uint32_t b = 0; b < node->blocks; ++b) {
        uint32_t db  = node->db[0] + b;
        uint64_t pos = (uint64_t)hdr->blocksz * db;
        uint64_t end = pos + node->size;

        while (pos < end) {
            struct dirent_t ent;
            if (lseek(pfs_fd, pos, SEEK_SET) < 0 ||
                read(pfs_fd, &ent, sizeof(ent)) != sizeof(ent))
                return;

            if (ent.type == 0) return;

            char *name = malloc(ent.namelen + 1);
            if (!name) return;
            name[ent.namelen] = '\0';
            if (level > 0) {
                if (lseek(pfs_fd, pos + sizeof(ent), SEEK_SET) < 0 ||
                    read(pfs_fd, name, ent.namelen) != ent.namelen) {
                    free(name);
                    return;
                }
            } else {
                name[0] = '\0';
            }

            char *full_path = strcat_alloc(parent_path, name);
            free(name);
            if (!full_path) return;

            if (ent.type == 2 && level > 0) {
                if (dry_run) {
                    *total_size += inodes[ent.ino].size;
                } else {
                    uint64_t off = (uint64_t)hdr->blocksz * inodes[ent.ino].db[0];
                    copy_chunk(pfs_fd, off, full_path, inodes[ent.ino].size, progress);
                }
            } else if (ent.type == 3) {
                if (!dry_run) mkdir(full_path, 0777);
                parse_dir(pfs_fd, hdr, inodes, ent.ino, level + 1,
                          full_path, dry_run, progress, total_size);
            }

            free(full_path);
            pos += ent.entsize;
        }
    }
}

/* ----------------------------------------------------------------- */
/*  Public entry point – with progress                               */
/* ----------------------------------------------------------------- */
int unpfs(const char *pfs_path, const char *out_dir, pfs_progress_cb progress)
{
    if (!pfs_path || !out_dir) return -1;

    int pfs_fd = open(pfs_path, O_RDONLY, 0);
    if (pfs_fd < 0) return -1;

    struct pfs_header_t *hdr = malloc(sizeof(*hdr));
    if (!hdr) { close(pfs_fd); return -1; }

    if (lseek(pfs_fd, 0, SEEK_SET) < 0 ||
        read(pfs_fd, hdr, sizeof(*hdr)) != sizeof(*hdr)) {
        free(hdr); close(pfs_fd); return -1;
    }

    size_t inode_count = (size_t)hdr->ndinode;
    struct di_d32 *inodes = calloc(inode_count, sizeof(struct di_d32));
    if (!inodes) { free(hdr); close(pfs_fd); return -1; }

    uint32_t ix = 0;
    for (uint32_t i = 0; i < hdr->ndinodeblock; ++i) {
        size_t per_block = hdr->blocksz / sizeof(struct di_d32);
        for (uint32_t j = 0; j < per_block && ix < inode_count; ++j) {
            uint64_t off = (uint64_t)hdr->blocksz * (i + 1) +
                           sizeof(struct di_d32) * j;
            if (lseek(pfs_fd, off, SEEK_SET) < 0 ||
                read(pfs_fd, &inodes[ix], sizeof(struct di_d32)) != sizeof(struct di_d32))
                goto cleanup;
            ++ix;
        }
    }

    /* === DRY RUN: calculate total size === */
    uint64_t total_size = 0;
    parse_dir(pfs_fd, hdr, inodes, (uint32_t)hdr->superroot_ino,
              0, out_dir, 1, NULL, &total_size);

    /* === SET GLOBAL PROGRESS STATE === */
    folder_size_current = total_size;
    total_bytes_copied = 0;
    copy_start_time = time(NULL);
    current_copied[0] = '\0';

    /* === CREATE ROOT DIR === */
    mkdir(out_dir, 0777);

    /* === REAL EXTRACTION WITH PROGRESS === */
    parse_dir(pfs_fd, hdr, inodes, (uint32_t)hdr->superroot_ino,
              0, out_dir, 0, progress, NULL);

cleanup:
    free(inodes);
    free(hdr);
    close(pfs_fd);
    return 0;
}