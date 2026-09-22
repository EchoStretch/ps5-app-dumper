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

/* The dumper, as the web-homebrew core gets to know it. */

#include "routes.h"
#include "web_assets.h"
#include "app_launch.h"
#include "dump_job.h"
#include "dump_queue.h"
#include "dump_library.h"
#include "version.h"

/* embedded at build time (webhb/webhb.mk) */
extern const unsigned char app_icon_png[];
extern const size_t        app_icon_png_len;

int dumper_busy(void)
{
    return job_is_active() || queue_is_active() || dumplib_move_active() || dumplib_fself_active();
}

const whb_app_t *dumper_app(void)
{
    static whb_app_t app = {
        .name          = "PS5 App Dumper",
        .short_name    = "App Dumper",
        .version       = DUMPER_VERSION,
        .process_name  = "ps5-app-dumper.elf",
        .data_dirname  = "ps5-app-dumper",
        .log_basename  = "dumper",
        .elf_basename  = "ps5-app-dumper",
        .tile_title_id = "APDU00001",
        .default_port  = 8081,   /* 8080 usually belongs to websrv */
        .busy          = dumper_busy,
        .busy_with     = "a dump",
        /* before anything talks to the system services */
        .on_start      = app_launch_init,
    };

    /* the generated lengths are variables, not constants */
    app.page     = web_index_html;
    app.page_len = web_index_html_len;
    app.icon_png = app_icon_png;
    app.icon_len = app_icon_png_len;
    return &app;
}
