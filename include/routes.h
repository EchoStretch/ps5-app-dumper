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

#ifndef ROUTES_H
#define ROUTES_H

#include "webhb.h"
#include "utils.h"

/* What the web-homebrew core is told about this payload. */
const whb_app_t *dumper_app(void);

/* 1 while a dump or a queue is running. */
int dumper_busy(void);

/* Registers the dumper's routes. Call both, and whb_routes_init(), before
   http_server_run(). */
void routes_settings_init(void);
void routes_dumper_init(void);

/* The settings live with the routes that serve and store them; everyone
   else gets a copy. */
void cfg_snapshot(dumper_config_t *out);
void json_config(sb_t *sb, const dumper_config_t *cfg);

/* A drive turned up after the payload started: settings changed in the
   meantime are written to it, otherwise its own are taken over. */
void cfg_drive_appeared(void);

#endif /* ROUTES_H */
