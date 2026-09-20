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

#ifndef ROUTES_H
#define ROUTES_H

#include "webhb.h"
#include "utils.h"

/* What the web-homebrew core is told about this payload. */
const whb_app_t *dumper_app(void);

/* 1 while a dump, a queue or the move of a dump is running. */
int dumper_busy(void);

/* Registers the dumper's routes. Call before whb_serve(). */
void routes_dumper_init(void);

/* Registers the dumper's settings with the core's config store. Call before
   whb_start(). */
void dumper_config_init(void);

/* The settings live in the core's store; everyone here gets a copy. */
void cfg_snapshot(dumper_config_t *out);

#endif /* ROUTES_H */
