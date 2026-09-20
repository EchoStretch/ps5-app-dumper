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

#ifndef APP_INSTALLER_H
#define APP_INSTALLER_H

#include <stddef.h>

/* Title id the home-screen shortcut is registered under. */
#define TILE_TITLE_ID "APDU00001"

/* Installs or refreshes a home-screen tile that opens the web UI on the
   given port in the console browser. The tile is a browser deeplink: it
   does not start this payload.

   Only ever called because the user asked for it. The system libraries it
   needs are loaded at that moment and never linked, so a console that
   takes the install badly cannot keep the dumper from starting.

   Returns 0 when the tile is installed and current. Otherwise returns -1
   and puts a reason a user can act on into err. */
int tile_install(int port, char *err, size_t err_size);

/* 1 when a tile pointing at this port is already in place. Touches the
   file system only, so it is safe to call from a polled endpoint. */
int tile_is_current(int port);

#endif /* APP_INSTALLER_H */
