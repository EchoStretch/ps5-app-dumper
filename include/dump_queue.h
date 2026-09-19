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

#ifndef DUMP_QUEUE_H
#define DUMP_QUEUE_H

#include <stddef.h>
#include <time.h>

#include "utils.h"

#define QUEUE_MAX        16
#define QUEUE_SETTLE_MIN 5
#define QUEUE_SETTLE_MAX 600

typedef enum {
    QITEM_PENDING = 0,
    QITEM_CLOSING,        /* ending the title that is in the way       */
    QITEM_LAUNCHING,      /* started, waiting for its mount to appear  */
    QITEM_SETTLING,       /* mounted, giving the game time to load     */
    QITEM_DUMPING,
    QITEM_DONE,
    QITEM_FAILED,
    QITEM_SKIPPED         /* never reached because the queue stopped   */
} queue_item_state_t;

typedef struct {
    char               title_id[16];
    char               title[128];
    queue_item_state_t state;
    char               message[192];
} queue_item_t;

typedef struct {
    int          active;
    int          count;
    int          current;         /* index being worked on, -1 when none   */
    int          settle_seconds;
    int          wait_remaining;  /* seconds left in the current wait      */
    char         mount[64];
    time_t       started;
    time_t       finished;
    queue_item_t items[QUEUE_MAX];
} queue_status_t;

/* Dumps the given titles one after the other: each one is started, given
   settle_seconds to finish loading, dumped to mount, and makes room for the
   next. A title that fails is recorded and the queue moves on. Returns 0 on
   success, -1 with the reason in err otherwise. */
int  queue_start(const char *const *title_ids, int count, const char *mount,
                 int settle_seconds, const dumper_config_t *cfg,
                 char *err, size_t err_size);

/* Stops after aborting whatever the queue is doing right now. */
void queue_stop(void);

/* Forgets the result of a finished queue. Returns -1 while one is running. */
int  queue_clear(void);

/* Snapshot of the queue, safe to call from any thread. */
void queue_get_status(queue_status_t *out);

int  queue_is_active(void);

const char *queue_item_state_name(queue_item_state_t state);

#endif /* DUMP_QUEUE_H */
