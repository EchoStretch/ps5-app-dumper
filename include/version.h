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

#ifndef VERSION_H
#define VERSION_H

#define DUMPER_VERSION "1.12"

/* The name a copy of this payload is stored under. Payload managers show no
   version of their own for an uploaded file - pldmgr reads it out of the
   file name ("_v1.12"), so the name has to carry it. */
#define DUMPER_ELF_NAME "ps5-app-dumper_v" DUMPER_VERSION ".elf"

#endif /* VERSION_H */
