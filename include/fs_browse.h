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

#ifndef FS_BROWSE_H
#define FS_BROWSE_H

#include <stddef.h>

#define FS_BROWSE_MAX   256   /* directories returned per listing */
#define FS_NAME_MAX     256   /* one directory name               */

/* A relative path is safe to join below a mount when it has no leading
   slash, no "." or ".." segment, no backslash, and fits the config field.
   An empty string is safe and means "the mount root". Returns 1 / 0. */
int fs_path_is_safe(const char *rel);

/* A single folder name is safe when it is non-empty, holds no slash, no
   backslash, and is not "." or "..". Returns 1 / 0. */
int fs_name_is_safe(const char *name);

/* Lists the sub-directories of <mount>/<rel>, sorted, names only (no "."
   or ".."). Returns the count, or -1 when the path is unsafe or cannot be
   opened. rel may be "" for the mount root. */
int fs_list_dirs(const char *mount, const char *rel,
                 char out[][FS_NAME_MAX], int max);

/* Creates <mount>/<rel>/<name>. Both rel and name are validated. On success
   writes the new path relative to the mount into out_rel and returns 0;
   returns -1 on validation failure or if the directory cannot be created. */
int fs_make_subdir(const char *mount, const char *rel, const char *name,
                   char *out_rel, size_t out_size);

#endif /* FS_BROWSE_H */
