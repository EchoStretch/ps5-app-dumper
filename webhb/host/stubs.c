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

/* What the core leaves to the console, for a build on this machine ("make
   sim"): toasts go to the terminal, there is no tile, no older copy to
   replace, and the only place to put files is the pretend console storage.
   An app that needs a busier pretend console brings its own, as the dumper's
   harness does. */

#include <stdio.h>
#include <string.h>

#include "webhb.h"

const unsigned char self_elf[] = "\177ELF-this-stands-in-for-the-payload";
const size_t        self_elf_len = sizeof(self_elf) - 1;

int sceKernelSendNotificationRequest(int device, SceNotificationRequest *req, size_t size, int blocking)
{
    (void)device; (void)size; (void)blocking;
    printf("[notify] %s\n", req->message);
    return 0;
}

int tile_exists(void) { return 0; }
int tile_is_current(int port) { (void)port; return 0; }
int tile_install(int port, char *err, size_t err_size)
{
    (void)port;
    snprintf(err, err_size, "there is no home screen here");
    return -1;
}

int target_scan(target_entry_t *out, int max)
{
    if (!out || max < 1) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->mount, sizeof(out->mount), "%s", storage_internal_root());
    mkdirs(out->mount);
    strcpy(out->fs, "ufs");
    out->writable = 1;
    out->internal = 1;
    out->total_bytes = 825ull << 30;
    out->free_bytes  = 120ull << 30;
    return 1;
}

int target_is_known(const char *mount)
{
    return (mount && !strcmp(mount, storage_internal_root())) ? 0 : -1;
}

int whb_start(const whb_app_t *app)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    whb_app_set(app);
    if (whb_app()->on_start) whb_app()->on_start();
    printf_notification("%s v%s", whb_app()->name, whb_app()->version);
    whb_config_init();
    return 0;
}
