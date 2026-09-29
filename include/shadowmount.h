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

#ifndef SHADOWMOUNT_H
#define SHADOWMOUNT_H

/* ShadowMount links every dump it finds to the installed title of the same
   id, and does so again after each scan. A dump that was moved out of its
   reach stays free; one that is still within it is linked anew - unless
   ShadowMount is not running. Stopping it is the user's call, never ours. */

/* The pid of a running ShadowMount, 0 when there is none. */
int shadowmount_pid(void);

/* Ends it. Returns 0 when it is gone afterwards, -1 when it would not go. */
int shadowmount_stop(void);

#endif /* SHADOWMOUNT_H */
