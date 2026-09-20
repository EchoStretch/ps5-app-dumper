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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>

#include "dump_queue.h"
#include "dump_job.h"
#include "dump_store.h"
#include "app_scan.h"
#include "app_launch.h"
#include "utils.h"

/* How long a title may take to show up under pfsmnt after it was started. */
#ifndef MOUNT_TIMEOUT      /* the host harness shortens this */
#define MOUNT_TIMEOUT    120
#endif
/* The console answers "still open" for a moment after a title went away. */
#define LAUNCH_ATTEMPTS  3
#define LAUNCH_RETRY_GAP 5
/* A freshly inserted disc is visible before the console is done with it. */
#define DISC_SETTLE      8
/* How often the console is reminded that the queue wants a disc. */
#define DISC_REMINDER    120

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
/* serialises queue_start() so two requests cannot both pass the active check */
static pthread_mutex_t g_start_mtx = PTHREAD_MUTEX_INITIALIZER;
static queue_status_t  g_queue = { .current = -1 };
static dumper_config_t g_queue_cfg[QUEUE_MAX];   /* one per title */
static volatile int    g_stop = 0;
static volatile int    g_skip = 0;
static pthread_t       g_worker;
static int             g_worker_valid = 0;

const char *queue_item_state_name(queue_item_state_t state)
{
    switch (state) {
        case QITEM_PENDING:   return "pending";
        case QITEM_WAITING_DISC: return "waiting_disc";
        case QITEM_LAUNCHING: return "launching";
        case QITEM_SETTLING:  return "settling";
        case QITEM_DUMPING:   return "dumping";
        case QITEM_DONE:      return "done";
        case QITEM_FAILED:    return "failed";
        case QITEM_SKIPPED:   return "skipped";
    }
    return "unknown";
}

static void set_item(int index, queue_item_state_t state, const char *fmt, ...)
{
    char msg[sizeof(g_queue.items[0].message)] = {0};

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
    }

    pthread_mutex_lock(&g_mtx);
    g_queue.items[index].state = state;
    memcpy(g_queue.items[index].message, msg, sizeof(msg));
    g_queue.wait_remaining = 0;
    pthread_mutex_unlock(&g_mtx);
}

static void set_wait(int seconds)
{
    pthread_mutex_lock(&g_mtx);
    g_queue.wait_remaining = seconds;
    pthread_mutex_unlock(&g_mtx);
}

/* ------------------------------------------------------------------ */
/*  Waiting                                                            */
/* ------------------------------------------------------------------ */

static int title_mounted(void *arg)
{
    app_entry_t app;
    return app_find((const char *)arg, &app) == 0;
}

/* The package mount, or the certainty that there will be none. A title that
   runs from a folder shows its sandbox at once; it has to stay that way for a
   while before it is believed, in case a package mount is merely late. */
typedef struct { const char *dir, *title_id; int folder_seen; } mount_wait_t;

static int mounted_or_never(void *arg)
{
    mount_wait_t *mw = arg;
    if (title_mounted((void *)mw->dir)) return 1;

    mw->folder_seen = title_runs_from_folder(mw->title_id) ? mw->folder_seen + 1 : 0;
    return mw->folder_seen >= 10;
}

#define WAIT_STOPPED (-1)
#define WAIT_SKIPPED (-2)

/* Waits up to seconds for cond to hold; with no cond it simply waits the
   time out. Returns 1 when cond held, 0 when the time ran out, WAIT_STOPPED
   when the queue was stopped and WAIT_SKIPPED when the user passed over
   this title. */
static int wait_until(int seconds, int (*cond)(void *), void *arg)
{
    for (int left = seconds; left > 0; left--) {
        if (g_stop) return WAIT_STOPPED;
        if (g_skip) return WAIT_SKIPPED;
        if (cond && cond(arg)) return 1;

        set_wait(left);
        sleep(1);
    }

    set_wait(0);
    if (g_stop) return WAIT_STOPPED;
    if (g_skip) return WAIT_SKIPPED;
    return (cond && cond(arg)) ? 1 : 0;
}

static int disc_inserted(void *arg)
{
    return title_on_disc((const char *)arg);
}

/* Holds until the title's disc is in the drive. Swapping a disc needs a
   person, who may be away for hours, so this has no time limit - only a
   stop or a skip ends it early. Returns 0 once the disc is there, or the
   WAIT_* code. */
static int wait_for_disc(int index, const char *title_id, const char *title)
{
    if (disc_inserted((void *)title_id)) return 0;

    set_item(index, QITEM_WAITING_DISC, "insert the disc for this game");
    write_log(g_log_path, "Queue: waiting for the disc of %s", title_id);

    for (unsigned waited = 0; ; waited++) {
        if (g_stop) return WAIT_STOPPED;
        if (g_skip) return WAIT_SKIPPED;
        if (disc_inserted((void *)title_id)) break;

        if (waited % DISC_REMINDER == 0)
            printf_notification_quiet("Dump queue: insert the disc for\n%s", title);
        sleep(1);
    }

    write_log(g_log_path, "Queue: disc of %s found", title_id);
    set_item(index, QITEM_WAITING_DISC, "disc found, giving the console a moment");

    int rc = wait_until(DISC_SETTLE, NULL, NULL);
    return rc < 0 ? rc : 0;
}

/* ------------------------------------------------------------------ */
/*  One title                                                          */
/* ------------------------------------------------------------------ */

/* Brings the title up. Returns 0 once it is mounted and had its time to
   load, 1 when it failed (recorded on the item), or the WAIT_* code. */
static int bring_up(int index, const char *title_id, const char *dir, int settle)
{
    char err[192] = {0};

    set_item(index, QITEM_LAUNCHING, "starting the game");

    /* A PS5 runs one game at a time, and the one in the way is usually the
       title this queue dumped a moment ago. The launch closes it on its way,
       the same as a forced start from the web UI - that goes by whatever the
       console reports as running, so a title that hung before it ever
       mounted is cleared away too. */
    for (int attempt = 1; ; attempt++) {
        if (app_launch_title(title_id, 1, err, sizeof(err)) == 0) break;

        write_log(g_log_path, "Queue: launch of %s failed (attempt %d): %s",
                  title_id, attempt, err);

        if (attempt >= LAUNCH_ATTEMPTS) {
            set_item(index, QITEM_FAILED, "%s", err);
            return 1;
        }
        int gap = wait_until(LAUNCH_RETRY_GAP, NULL, NULL);
        if (gap < 0) return gap;
    }

    mount_wait_t mw = { dir, title_id, 0 };
    int rc = wait_until(MOUNT_TIMEOUT, mounted_or_never, &mw);
    if (rc < 0) return rc;
    if (rc == 1 && !title_mounted((void *)dir)) {
        set_item(index, QITEM_FAILED, "runs from a folder instead of its package - a dump is mounted "
                                      "over it (ShadowMount?), so there is nothing to dump");
        write_log(g_log_path, "Queue: %s runs without a package mount - skipped", title_id);
        return 1;
    }
    if (rc == 0) {
        set_item(index, QITEM_FAILED,
                 "the game did not come up within %d seconds", MOUNT_TIMEOUT);
        return 1;
    }

    set_item(index, QITEM_SETTLING, "waiting for the game to finish loading");
    rc = wait_until(settle, NULL, NULL);
    if (rc < 0) return rc;

    /* it may have crashed or been closed by hand while we waited */
    if (!title_mounted((void *)dir)) {
        set_item(index, QITEM_FAILED, "the game closed again before the dump started");
        return 1;
    }

    return 0;
}

/* Returns -1 when the queue was stopped, 0 otherwise. */
static int process_item(int index)
{
    queue_status_t snap;
    queue_get_status(&snap);

    const char *title_id = snap.items[index].title_id;
    char dir[32], err[192] = {0};
    snprintf(dir, sizeof(dir), "%s-app0", title_id);

    write_log(g_log_path, "Queue: %d/%d %s", index + 1, snap.count, title_id);

    /* A title that is on the drive already is passed over: a queue runs
       unattended, and starting a game only to find that out would cost
       minutes. A dump that was cut short does not count - job_start()
       clears it away and does it again. */
    char dest[384];
    job_dest_path(snap.mount, &g_queue_cfg[index], dest, sizeof(dest));

    if (!snap.replace_existing &&
        dump_presence(dest, title_id, strncmp(title_id, "CUSA", 4) == 0,
                      g_queue_cfg[index].split) == DUMP_PRESENT) {
        write_log(g_log_path, "Queue: %s is already dumped in %s, skipped", title_id, dest);
        set_item(index, QITEM_SKIPPED, "already dumped in %s - skipped", dest);
        return 0;
    }

    /* a title that is already up is dumped as it is */
    if (!title_mounted(dir)) {
        int rc = 0;

        if (snap.items[index].is_disc)
            rc = wait_for_disc(index, title_id, snap.items[index].title);
        if (rc == 0)
            rc = bring_up(index, title_id, dir, snap.settle_seconds);

        if (rc == WAIT_SKIPPED) {
            write_log(g_log_path, "Queue: %s skipped by user", title_id);
            set_item(index, QITEM_SKIPPED, "skipped by user");
            return 0;
        }
        if (rc != 0) return rc < 0 ? -1 : 0;
    }

    if (g_stop) return -1;

    int started = job_start(dir, snap.mount, &g_queue_cfg[index], snap.replace_existing,
                            err, sizeof(err));
    if (started == JOB_ERR_EXISTS) {
        set_item(index, QITEM_SKIPPED, "%s - skipped", err);
        return 0;
    }
    if (started != 0) {
        set_item(index, QITEM_FAILED, "%s", err);
        return 0;
    }

    set_item(index, QITEM_DUMPING, NULL);

    /* queue_stop() aborts the job itself, so this only has to wait - unless
       the stop slipped in just before the job existed */
    if (g_stop) job_abort();
    while (job_is_active()) usleep(500000);

    job_status_t job;
    job_get_status(&job);

    if (job.state == JOB_DONE) {
        set_item(index, QITEM_DONE, "%s", job.message);
        return 0;
    }

    /* Stopped on its own, by a skip: this dump is given up, the queue is
       not. A stop of the whole queue also sets g_stop and wins. */
    if (job.state == JOB_ABORTED && g_skip && !g_stop) {
        write_log(g_log_path, "Queue: dump of %s stopped by user, moving on", title_id);
        set_item(index, QITEM_SKIPPED, "dump stopped by user - the files of %s in %s are incomplete",
                 title_id, job.dest);
        return 0;
    }

    set_item(index, QITEM_FAILED, "%s", job.message);
    return job.state == JOB_ABORTED ? -1 : 0;
}

static void *worker(void *arg)
{
    (void)arg;

    queue_status_t snap;
    queue_get_status(&snap);

    int stopped = 0;

    for (int i = 0; i < snap.count && !stopped; i++) {
        pthread_mutex_lock(&g_mtx);
        g_queue.current = i;
        pthread_mutex_unlock(&g_mtx);
        g_skip = 0;   /* a skip is meant for one title only */

        if (g_stop || process_item(i) < 0) stopped = 1;
    }

    int done = 0;

    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_queue.count; i++) {
        queue_item_t *item = &g_queue.items[i];

        if (item->state == QITEM_DONE) { done++; continue; }
        if (item->state == QITEM_FAILED || item->state == QITEM_SKIPPED) continue;

        /* cut short by the stop, either mid-way or before its turn */
        int reached = (item->state != QITEM_PENDING);
        item->state = reached ? QITEM_FAILED : QITEM_SKIPPED;
        snprintf(item->message, sizeof(item->message), "%s",
                 reached ? "stopped by user" : "queue stopped before its turn");
    }
    g_queue.active = 0;
    g_queue.current = -1;
    g_queue.wait_remaining = 0;
    g_queue.finished = time(NULL);
    int count = g_queue.count;
    pthread_mutex_unlock(&g_mtx);

    write_log(g_log_path, "Queue: %s, %d of %d dumped",
              stopped ? "stopped" : "finished", done, count);
    printf_notification("Dump queue %s: %d of %d dumped",
                        stopped ? "stopped" : "finished", done, count);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public interface                                                   */
/* ------------------------------------------------------------------ */

int queue_is_active(void)
{
    pthread_mutex_lock(&g_mtx);
    int active = g_queue.active;
    pthread_mutex_unlock(&g_mtx);
    return active;
}

int queue_start(const char *const *title_ids, const int *is_disc,
                const dumper_config_t *const *item_cfg, int count,
                const char *mount, int settle_seconds, int replace_existing,
                const dumper_config_t *cfg, char *err, size_t err_size)
{
    #define FAIL(...) do { if (err && err_size) snprintf(err, err_size, __VA_ARGS__); \
                           pthread_mutex_unlock(&g_start_mtx); return -1; } while (0)

    pthread_mutex_lock(&g_start_mtx);

    if (!title_ids || !mount || !cfg)    FAIL("missing parameters");
    if (count <= 0)                      FAIL("the queue is empty");
    if (count > QUEUE_MAX)               FAIL("a queue holds at most %d titles", QUEUE_MAX);
    if (queue_is_active())               FAIL("a queue is already running");
    if (job_is_active())                 FAIL("a dump is already running");
    if (target_is_known(mount) != 0)     FAIL("unknown destination");
    if (strlen(mount) >= sizeof(((queue_status_t *)0)->mount))
                                         FAIL("destination path is too long");
    if (!app_launch_available())         FAIL("this console build cannot start titles");

    queue_status_t next;
    memset(&next, 0, sizeof(next));

    for (int i = 0; i < count; i++) {
        library_entry_t entry;
        if (library_find(title_ids[i], &entry) != 0)
            FAIL("%s is not installed", title_ids[i] ? title_ids[i] : "(null)");

        for (int k = 0; k < i; k++)
            if (strcmp(next.items[k].title_id, entry.title_id) == 0)
                FAIL("%s is in the queue twice", entry.title_id);

        queue_item_t *item = &next.items[i];
        strncpy(item->title_id, entry.title_id, sizeof(item->title_id) - 1);
        strncpy(item->title, entry.title[0] ? entry.title : entry.title_id,
                sizeof(item->title) - 1);

        item->custom = (item_cfg && item_cfg[i]) ? 1 : 0;
        item->is_disc = is_disc ? (is_disc[i] != 0) : entry.is_disc;
        /* what the user knows about a title is worth keeping */
        if (item->is_disc) title_remember_disc(entry.title_id);
    }

    if (settle_seconds < QUEUE_SETTLE_MIN) settle_seconds = QUEUE_SETTLE_MIN;
    if (settle_seconds > QUEUE_SETTLE_MAX) settle_seconds = QUEUE_SETTLE_MAX;

    next.active = 1;
    next.count = count;
    next.current = -1;
    next.settle_seconds = settle_seconds;
    next.replace_existing = replace_existing ? 1 : 0;
    next.started = time(NULL);
    strncpy(next.mount, mount, sizeof(next.mount) - 1);

    /* A finished worker is joined here so its resources are released before
       the next one starts. */
    if (g_worker_valid) {
        pthread_join(g_worker, NULL);
        g_worker_valid = 0;
    }

    g_stop = 0;
    g_skip = 0;
    for (int i = 0; i < count; i++)
        g_queue_cfg[i] = (item_cfg && item_cfg[i]) ? *item_cfg[i] : *cfg;

    pthread_mutex_lock(&g_mtx);
    g_queue = next;
    pthread_mutex_unlock(&g_mtx);

    if (pthread_create(&g_worker, NULL, worker, NULL) != 0) {
        pthread_mutex_lock(&g_mtx);
        memset(&g_queue, 0, sizeof(g_queue));
        g_queue.current = -1;
        pthread_mutex_unlock(&g_mtx);
        FAIL("could not start the worker thread");
    }

    g_worker_valid = 1;
    write_log(g_log_path, "Queue: started with %d titles, %d s to load each, existing dumps are %s",
              count, settle_seconds, replace_existing ? "replaced" : "skipped");

    pthread_mutex_unlock(&g_start_mtx);
    return 0;

    #undef FAIL
}

void queue_stop(void)
{
    if (!queue_is_active()) return;

    g_stop = 1;
    job_abort();
    write_log(g_log_path, "Queue: stop requested");
}

int queue_skip(void)
{
    pthread_mutex_lock(&g_mtx);
    int waiting = 0, dumping = 0;
    if (g_queue.active && g_queue.current >= 0) {
        queue_item_state_t st = g_queue.items[g_queue.current].state;
        waiting = (st == QITEM_WAITING_DISC || st == QITEM_LAUNCHING ||
                   st == QITEM_SETTLING);
        dumping = (st == QITEM_DUMPING);
    }
    pthread_mutex_unlock(&g_mtx);

    if (!waiting && !dumping) return -1;

    g_skip = 1;
    /* a dump does not look at g_skip; it is ended like any other, and the
       flag tells process_item() that the queue goes on afterwards */
    if (dumping) job_abort();
    return 0;
}

int queue_clear(void)
{
    pthread_mutex_lock(&g_mtx);
    int active = g_queue.active;
    if (!active) {
        memset(&g_queue, 0, sizeof(g_queue));
        g_queue.current = -1;
    }
    pthread_mutex_unlock(&g_mtx);
    return active ? -1 : 0;
}

void queue_get_status(queue_status_t *out)
{
    if (!out) return;

    pthread_mutex_lock(&g_mtx);
    *out = g_queue;
    pthread_mutex_unlock(&g_mtx);
}
