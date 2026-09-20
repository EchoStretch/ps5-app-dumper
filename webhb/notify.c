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

/* Toasts on the TV. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

#include "webhb.h"

static void send_notification(const char *fmt, va_list ap, int to_console)
{
    SceNotificationRequest noti;
    memset(&noti, 0, sizeof(noti));

    vsnprintf(noti.message, sizeof(noti.message), fmt, ap);

    noti.type = 0;
    noti.use_icon_image_uri = 1;
    noti.target_id = -1;
    strncpy(noti.uri, "cxml://psnotification/tex_icon_system", sizeof(noti.uri)-1);

    sceKernelSendNotificationRequest(0, &noti, sizeof(noti), 0);
    printf("%s\n", noti.message);
    if (to_console) log_ring_push(noti.message);
}

void printf_notification(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    send_notification(fmt, ap, 1);
    va_end(ap);
}

void printf_notification_quiet(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    send_notification(fmt, ap, 0);
    va_end(ap);
}

