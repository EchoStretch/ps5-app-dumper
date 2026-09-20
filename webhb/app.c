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

/* Who the core is working for. */

#include <stdio.h>

#include "webhb.h"

/* stands in until whb_app_set() was called, so that nothing the core prints
   or builds a path from is ever a NULL */
static const whb_app_t g_nobody = {
    .name = "Web homebrew", .short_name = "Homebrew", .version = "0",
    .process_name = "webhb.elf", .data_dirname = "webhb", .elf_basename = "webhb",
    .default_port = 8081,   /* 8080 usually belongs to websrv */
};

static const whb_app_t *g_app = &g_nobody;
static char             g_elf_name[96] = "webhb_v0.elf";

void whb_app_set(const whb_app_t *app)
{
    if (!app) return;
    g_app = app;
    snprintf(g_elf_name, sizeof(g_elf_name), "%s_v%s.elf", app->elf_basename, app->version);
}

const whb_app_t *whb_app(void)
{
    return g_app;
}

int whb_busy(void)
{
    return g_app->busy ? g_app->busy() : 0;
}

const char *whb_elf_name(void)
{
    return g_elf_name;
}
