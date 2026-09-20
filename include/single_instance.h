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

#ifndef SINGLE_INSTANCE_H
#define SINGLE_INSTANCE_H

/* The name this payload runs under. Tools that list payloads, pldmgr among
   them, only count processes whose name ends in ".elf"; the kernel keeps 19
   characters of it. */
#define PAYLOAD_PROCESS_NAME "ps5-app-dumper.elf"

/* Names the process, so it can be found - by others and by the next copy of
   this payload. */
void instance_claim_name(void);

/* Makes this the only running copy: an idle older instance is asked to
   quit, and ended by force if it does not. web_port is where the search
   for its web UI starts.

   A copy that is in the middle of a dump is never touched. Returns 0 when
   the way is free, and -1 with the port of the busy instance in busy_port
   when this copy should step aside instead. */
int instance_take_over(int web_port, int *busy_port);

#endif /* SINGLE_INSTANCE_H */
