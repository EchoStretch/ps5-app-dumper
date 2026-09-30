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

#ifndef DUMP_QUEUE_H
#define DUMP_QUEUE_H

#include <stddef.h>
#include <time.h>

#include "utils.h"

#define QUEUE_MAX        64
#define QUEUE_SETTLE_MIN 5
#define QUEUE_SETTLE_MAX 600

typedef enum {
    QITEM_PENDING = 0,
    QITEM_WAITING_DISC,   /* a disc game whose disc is not in the drive */
    QITEM_LAUNCHING,      /* started, waiting for its mount to appear  */
    QITEM_SETTLING,       /* mounted, giving the game time to load     */
    QITEM_DUMPING,
    QITEM_DONE,
    QITEM_FAILED,
    QITEM_SKIPPED         /* passed over, by the user or by a stop     */
} queue_item_state_t;

typedef struct {
    char               title_id[16];
    char               title[128];
    int                is_disc;      /* wait for its disc before starting */
    int                custom;       /* dumped with settings of its own   */
    queue_item_state_t state;
    char               message[192];
} queue_item_t;

typedef struct {
    int          active;
    int          count;
    int          current;         /* index being worked on, -1 when none   */
    int          settle_seconds;
    int          replace_existing; /* dump again what is on the drive already */
    int          wait_remaining;  /* seconds left in the current wait      */
    char         mount[64];
    time_t       started;
    time_t       finished;
    queue_item_t items[QUEUE_MAX];
} queue_status_t;

/* Dumps the given titles one after the other: each one is started, given
   settle_seconds to finish loading, dumped to mount, and makes room for the
   next. A title that fails is recorded and the queue moves on.

   is_disc marks, per title, a disc game: the queue then holds until that
   disc is in the drive, however long the swap takes. NULL lets the scanner
   decide.

   item_cfg gives, per title, the settings to dump it with; a NULL entry -
   or a NULL array - falls back to cfg.

   A title that is on the drive already, finished, is passed over - unless
   replace_existing is set, in which case the old dump is deleted and the
   title dumped anew. The choice is made once, up front, because a queue
   runs with nobody there to ask. Returns 0 on success, -1 with the reason
   in err otherwise. */
int  queue_start(const char *const *title_ids, const int *is_disc,
                 const dumper_config_t *const *item_cfg, int count,
                 const char *mount, int settle_seconds, int replace_existing,
                 const dumper_config_t *cfg, char *err, size_t err_size);

/* Passes over the title the queue is working on and moves to the next: a
   wait - for a disc, a launch, the load time - simply ends, a dump in
   progress is stopped at the next file boundary and left incomplete.
   Returns -1 when the queue is not at a title. */
int  queue_skip(void);

/* Stops after aborting whatever the queue is doing right now. */
void queue_stop(void);

/* Forgets the result of a finished queue. Returns -1 while one is running. */
int  queue_clear(void);

/* Snapshot of the queue, safe to call from any thread. */
void queue_get_status(queue_status_t *out);

int  queue_is_active(void);

const char *queue_item_state_name(queue_item_state_t state);

#endif /* DUMP_QUEUE_H */
