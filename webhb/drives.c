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

/* The places a web homebrew can put files: the drives that are plugged in,
   and the console's own storage. Console business - the host harness brings
   its own. */

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/mount.h>

#include "webhb.h"

/* Mount points that may hold a dump, probed in this order. */
static const char *g_candidate_mounts[] = {
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
    "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
    "/mnt/ext0", "/mnt/ext1",
    NULL
};

int target_scan(target_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;

    int count = 0;

    for (int i = 0; g_candidate_mounts[i] && count < max; i++) {
        const char *mount = g_candidate_mounts[i];
        if (!dir_exists(mount)) continue;

        target_entry_t *t = &out[count];
        memset(t, 0, sizeof(*t));
        strncpy(t->mount, mount, sizeof(t->mount) - 1);

        struct statfs sf;
        if (statfs(mount, &sf) == 0) {
            /* Unused mount points exist as empty directories on the root
               file system, so they answer statfs with the root's own type
               and a couple of megabytes - not somewhere a dump can go. */
            if (strcmp(sf.f_fstypename, "exfatfs") != 0 &&
                strcmp(sf.f_fstypename, "msdosfs") != 0 &&
                strcmp(sf.f_fstypename, "ntfs")    != 0 &&
                strcmp(sf.f_fstypename, "ufs")     != 0 &&
                strcmp(sf.f_fstypename, "fusefs")  != 0)
                continue;

            uint64_t total = (uint64_t)sf.f_blocks * sf.f_bsize;
            if (total < 64ull * 1024 * 1024) continue;

            strncpy(t->fs, sf.f_fstypename, sizeof(t->fs) - 1);
            t->total_bytes = total;
            t->free_bytes  = (uint64_t)sf.f_bavail * sf.f_bsize;
            t->writable    = (sf.f_flags & MNT_RDONLY) ? 0 : 1;
        } else {
            /* statfs is unavailable for this mount - assume it is usable
               and let the dump report the real error. */
            t->writable = 1;
        }

        count++;
    }

    /* The console itself, last: never the obvious choice while a drive is
       there, but a place to dump to when none is. */
    if (count < max) {
        const char *root = storage_internal_root();
        mkdirs(root);

        struct statfs sf;
        if (dir_exists(root) && statfs(root, &sf) == 0 && !(sf.f_flags & MNT_RDONLY)) {
            target_entry_t *t = &out[count++];
            memset(t, 0, sizeof(*t));
            strncpy(t->mount, root, sizeof(t->mount) - 1);
            strncpy(t->fs, sf.f_fstypename, sizeof(t->fs) - 1);
            t->writable    = 1;
            t->internal    = 1;
            t->total_bytes = (uint64_t)sf.f_blocks * sf.f_bsize;
            t->free_bytes  = (uint64_t)sf.f_bavail * sf.f_bsize;
        }
    }

    return count;
}

int target_is_known(const char *mount)
{
    if (!mount || !mount[0]) return -1;

    /* checked against the scan so a placeholder mount cannot be selected */
    target_entry_t list[TARGET_SCAN_MAX];
    int count = target_scan(list, TARGET_SCAN_MAX);

    for (int i = 0; i < count; i++)
        if (strcmp(mount, list[i].mount) == 0) return 0;

    return -1;
}
