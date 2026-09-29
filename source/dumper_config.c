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

/* The dumper's settings, as the core's config store gets to know them. Job,
   queue and the dumpers keep working on a dumper_config_t: a copy of the
   store's values, taken when they ask for one. */

#include <stdio.h>

#include "routes.h"
#include "dump_queue.h"

/* a folder of their own, next to our settings - "homebrew" itself is shared
   by many tools */
static char g_default_subdir[WHB_CFG_STR_MAX];

/* The order is the one the settings have always had in the page's requests. */
static const whb_cfg_key_t g_keys[] = {
    { .ini_name = "enable_decrypter", .web_name = "enableDecrypter", .type = WHB_CFG_BOOL, .def = 1,
      .comment = "; === Decrypt App ===\n"
                 "; enable_decrypter = 1  -> decrypt ELF files (default)\n"
                 "; enable_decrypter = 0  -> disable decryption\n" },
    { .ini_name = "enable_backport", .web_name = "enableBackport", .type = WHB_CFG_BOOL, .def = 0,
      .comment = "; === Backport Options PS4/PS5 ===\n"
                 "; enable_backport = 1 -> enable SDK patching\n"
                 "; enable_backport = 0 -> disable SDK patching (default)\n"
                 "; ps4_backport_level = 1-6 -> predefined SDK pair (default: 4 (PS4 9.00) )\n"
                 "; ps5_backport_level = 1-10 -> predefined SDK pair (default: 1 (PS5 1.00) )\n"
                 "; >>>> BACKPORTING IS FOR ADVANCED USERS MAY NOT WORK <<<<\n" },
    { .ini_name = "ps4_backport_level", .web_name = "ps4BackportLevel", .type = WHB_CFG_INT,
      .lo = 1, .hi = 6, .def = 4 },
    { .ini_name = "ps5_backport_level", .web_name = "ps5BackportLevel", .type = WHB_CFG_INT,
      .lo = 1, .hi = 10, .def = 1 },
    { .ini_name = "enable_elf2fself", .web_name = "enableElf2fself", .type = WHB_CFG_BOOL, .def = 0,
      .comment = "; === FSELF Files ===\n"
                 "; enable_elf2fself = 1 -> enable fself ELF files\n"
                 "; enable_elf2fself = 0 -> disable fself (default)\n" },
    WHB_CFG_STD_LOGGING,
    { .ini_name = "split", .web_name = "split", .type = WHB_CFG_INT, .lo = 0, .hi = 3, .def = 3,
      .comment = "; === PS4 Split Mode ===\n"
                 "; 0 = no split (CUSAxxxxx/)\n"
                 "; 1 = app only (CUSAxxxxx-app/)\n"
                 "; 2 = patch only (CUSAxxxxx-patch/)\n"
                 "; 3 = both split (CUSAxxxxx-app/ + CUSAxxxxx-patch/)\n" },
    { .ini_name = "enable_webui", .web_name = "enableWebui", .type = WHB_CFG_BOOL, .def = 1,
      .comment = "; === Web UI ===\n"
                 "; enable_webui = 1 -> serve the web interface (default)\n"
                 "; enable_webui = 0 -> headless, dump the running app right away\n" },
    WHB_CFG_STD_WEB_PORT,
    WHB_CFG_STD_TILE,
    { .ini_name = "auto_start", .web_name = "autoStart", .type = WHB_CFG_BOOL, .def = 0,
      .comment = "; auto_start = 1 -> dump the running game right at launch; the web UI comes\n"
                 ";                   up as well, to watch it, stop it or switch this off\n" },
    { .ini_name = "dump_dlc", .web_name = "dumpDlc", .type = WHB_CFG_BOOL, .def = 1,
      .comment = "; === Additional content ===\n"
                 "; dump_dlc = 1 -> the DLC mounted with a title are dumped along with it, each as a\n"
                 ";                 folder of its own next to the game's (default)\n" },
    { .ini_name = "queue_delay", .web_name = "queueDelay", .type = WHB_CFG_INT,
      .lo = QUEUE_SETTLE_MIN, .hi = QUEUE_SETTLE_MAX, .def = 30,
      .comment = "; === Dump Queue ===\n"
                 "; queue_delay -> seconds a queued title gets to load before it is dumped (5-600)\n" },
    WHB_CFG_STD_ACCESS,
    { .ini_name = "dump_subdir_console", .web_name = "dumpSubdirConsole", .type = WHB_CFG_STRING,
      .flags = WHB_CFG_SUBDIR, .def_str = g_default_subdir, .if_empty = "homebrew",
      .comment = "; === Destination ===\n"
                 "; dump_subdir         -> folder below a drive's mount point that receives the dump\n"
                 "; dump_subdir_console -> the same below /data, when dumping to the console itself\n" },
    { .ini_name = "dump_subdir", .web_name = "dumpSubdir", .type = WHB_CFG_STRING,
      .flags = WHB_CFG_SUBDIR, .def_str = g_default_subdir, .if_empty = "homebrew" },
};

void cfg_snapshot(dumper_config_t *out)
{
    whb_config_lock();
    out->enable_decrypter   = whb_config_int("enable_decrypter", 1);
    out->enable_backport    = whb_config_int("enable_backport", 0);
    out->ps4_backport_level = whb_config_int("ps4_backport_level", 4);
    out->ps5_backport_level = whb_config_int("ps5_backport_level", 1);
    out->enable_elf2fself   = whb_config_int("enable_elf2fself", 0);
    out->enable_logging     = whb_config_int("enable_logging", 1);
    out->split              = whb_config_int("split", 3);
    out->enable_webui       = whb_config_int("enable_webui", 1);
    out->web_port           = whb_config_int("web_port", whb_app()->default_port);
    out->auto_start         = whb_config_int("auto_start", 0);
    out->queue_delay        = whb_config_int("queue_delay", 30);
    out->dump_dlc           = whb_config_int("dump_dlc", 1);
    whb_config_str("dump_subdir", out->dump_subdir, sizeof(out->dump_subdir));
    whb_config_str("dump_subdir_console", out->dump_subdir_console, sizeof(out->dump_subdir_console));
    whb_config_unlock();
}

static void settings_changed(void)
{
    g_split_mode = whb_config_int("split", 3);
}

void dumper_config_init(void)
{
    snprintf(g_default_subdir, sizeof(g_default_subdir), "homebrew/%s/dumps", dumper_app()->data_dirname);

    whb_config_register(g_keys, (int)(sizeof(g_keys) / sizeof(g_keys[0])));
    whb_config_on_change(settings_changed);
}
