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

#ifndef PS4_DUMPER_H
#define PS4_DUMPER_H

int ps4_dumper_start(const char *title_id,
                      const char *app_folder,
                      const char *patch_folder,
                      const char *usb_path,
                      int do_decrypt,
                      int do_elf2fself,
                      int do_backport);

int dump_ps4_cusa_app(const char *title_id,
                      const char *app_folder,
                      const char *patch_folder,
                      const char *usb_path,
                      int do_decrypt,
                      int do_elf2fself,
                      int do_backport);

#endif