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

/* The log: a ring in memory that feeds the live console, and the files on
   whatever holds our data folder. */

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

int  g_enable_logging = 1;
char g_log_path[512] = {0};

/* ------------------------------------------------------------------ */
/*  In-memory log ring                                                 */
/* ------------------------------------------------------------------ */

static char     g_log_ring[LOG_RING_CAPACITY][LOG_LINE_MAX];
static unsigned g_log_ring_seq = 0;   /* sequence number of the newest line */
static pthread_mutex_t g_log_ring_mtx = PTHREAD_MUTEX_INITIALIZER;

void log_ring_push(const char *line)
{
    if (!line || !*line) return;

    pthread_mutex_lock(&g_log_ring_mtx);
    g_log_ring_seq++;
    char *slot = g_log_ring[g_log_ring_seq % LOG_RING_CAPACITY];
    strncpy(slot, line, LOG_LINE_MAX - 1);
    slot[LOG_LINE_MAX - 1] = '\0';

    /* newlines would break the one-line-per-entry contract of the UI */
    for (char *p = slot; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';

    pthread_mutex_unlock(&g_log_ring_mtx);
}

unsigned log_ring_seq(void)
{
    pthread_mutex_lock(&g_log_ring_mtx);
    unsigned seq = g_log_ring_seq;
    pthread_mutex_unlock(&g_log_ring_mtx);
    return seq;
}

void log_ring_walk(unsigned since, log_line_cb cb, void *ctx)
{
    if (!cb) return;

    pthread_mutex_lock(&g_log_ring_mtx);
    unsigned newest = g_log_ring_seq;
    unsigned oldest = (newest > LOG_RING_CAPACITY) ? newest - LOG_RING_CAPACITY + 1 : 1;
    if (since + 1 > oldest) oldest = since + 1;

    for (unsigned seq = oldest; seq <= newest; seq++) {
        char copy[LOG_LINE_MAX];
        strncpy(copy, g_log_ring[seq % LOG_RING_CAPACITY], sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        pthread_mutex_unlock(&g_log_ring_mtx);
        cb(ctx, seq, copy);
        pthread_mutex_lock(&g_log_ring_mtx);
    }
    pthread_mutex_unlock(&g_log_ring_mtx);
}

/* The log of everything that is not one particular dump: start-up, the
   queue's moves, the web UI. A dump switches to a file of its own and comes
   back here when it is over. */
void log_use_general(void)
{
    if (get_app_data_path()[0]) snprintf(g_log_path, sizeof(g_log_path), "%s/logs/dumper.log", get_app_data_path());
}

void log_use_dump(const char *title_id)
{
    if (!get_app_data_path()[0] || !title_id) return;

    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", localtime(&now));
    snprintf(g_log_path, sizeof(g_log_path), "%s/logs/%s_%s.log", get_app_data_path(), stamp, title_id);
}

int write_log(const char *log_file_path, const char *fmt, ...)
{
    char msg[LOG_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* the ring backs the live console of the web UI and stays alive even
       when file logging is turned off */
    log_ring_push(msg);

    if (!g_enable_logging || !log_file_path || !log_file_path[0]) return 0;

    FILE *f = fopen(log_file_path, "a");
    if (!f) return -1;

    char timestamp[64];
    time_t t = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&t));

    fprintf(f, "[%s] %s\n", timestamp, msg);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return 0;
}

