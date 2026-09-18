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

#ifndef APP_LAUNCH_H
#define APP_LAUNCH_H

#include <stddef.h>

/* Claims the authority the launch services expect, before anything opens a
   connection to them. Call once, as early in main() as possible: raising it
   later does not help, the services remember the caller they first saw. */
void app_launch_init(void);

/* 1 when this console exposes the services needed to start a title. The
   symbols are resolved on first use, so the payload still runs without
   them - it just cannot launch anything. */
int app_launch_available(void);

/* Same answer, but never loads anything: reports what is already known.
   Used by the polling endpoints, which must not touch system services. */
int app_launch_probably_available(void);

/* Application id of the title currently in the foreground, or <= 0 when the
   console sits on the dashboard. */
int app_running_id(void);

/* Starts a title so its files appear under pfsmnt.

   A PS5 runs one game at a time, so a title already in the foreground has to
   be closed first - that only happens when close_running is set, since it
   ends the player's session. Returns 0 on success, -1 otherwise with the
   reason written to err. */
int app_launch_title(const char *title_id, int close_running,
                     char *err, size_t err_size);

#endif /* APP_LAUNCH_H */
