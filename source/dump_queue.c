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
#include "app_scan.h"
#include "app_launch.h"
#include "utils.h"

/* How long a title may take to show up under pfsmnt after it was started,
   and to leave it again after it was closed. */
#ifndef MOUNT_TIMEOUT      /* the host harness shortens these */
#define MOUNT_TIMEOUT    120
#endif
#ifndef UNMOUNT_TIMEOUT
#define UNMOUNT_TIMEOUT  60
#endif
/* The console answers "still open" for a moment after a title went away. */
#define LAUNCH_ATTEMPTS  3
#define LAUNCH_RETRY_GAP 5

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
/* serialises queue_start() so two requests cannot both pass the active check */
static pthread_mutex_t g_start_mtx = PTHREAD_MUTEX_INITIALIZER;
static queue_status_t  g_queue = { .current = -1 };
static dumper_config_t g_queue_cfg;
static volatile int    g_stop = 0;
static pthread_t       g_worker;
static int             g_worker_valid = 0;

const char *queue_item_state_name(queue_item_state_t state)
{
    switch (state) {
        case QITEM_PENDING:   return "pending";
        case QITEM_CLOSING:   return "closing";
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

static int nothing_mounted(void *arg)
{
    (void)arg;

    app_entry_t *apps = calloc(APP_SCAN_MAX, sizeof(*apps));
    if (!apps) return 1;

    int count = app_scan(apps, APP_SCAN_MAX);
    free(apps);
    return count == 0;
}

/* A title can hold the console without ever mounting - one that hangs on
   its way up, say - so the mounts alone do not tell whether the way is free. */
static int console_idle(void *arg)
{
    return nothing_mounted(arg) && app_running_id() <= 0;
}

/* Waits up to seconds for cond to hold; with no cond it simply waits the
   time out. Returns 1 when cond held, 0 when the time ran out and -1 when
   the queue was stopped. */
static int wait_until(int seconds, int (*cond)(void *), void *arg)
{
    for (int left = seconds; left > 0; left--) {
        if (g_stop) return -1;
        if (cond && cond(arg)) return 1;

        set_wait(left);
        sleep(1);
    }

    set_wait(0);
    if (g_stop) return -1;
    return (cond && cond(arg)) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  One title                                                          */
/* ------------------------------------------------------------------ */

/* Brings the title up. Returns 0 once it is mounted and had its time to
   load, 1 when it failed (recorded on the item) and -1 when stopped. */
static int bring_up(int index, const char *title_id, const char *dir, int settle)
{
    char err[192] = {0};

    /* A PS5 runs one game at a time, and the one in the way is usually the
       title this queue dumped a moment ago. */
    if (!console_idle(NULL)) {
        set_item(index, QITEM_CLOSING, "closing the running game");

        if (app_close_running(err, sizeof(err)) != 0) {
            set_item(index, QITEM_FAILED, "%s", err);
            return 1;
        }

        int rc = wait_until(UNMOUNT_TIMEOUT, console_idle, NULL);
        if (rc < 0) return -1;
        if (rc == 0)
            write_log(g_log_path, "Queue: the running game is still up, trying anyway");
    }

    set_item(index, QITEM_LAUNCHING, "starting the game");

    for (int attempt = 1; ; attempt++) {
        if (app_launch_title(title_id, 1, err, sizeof(err)) == 0) break;

        write_log(g_log_path, "Queue: launch of %s failed (attempt %d): %s",
                  title_id, attempt, err);

        if (attempt >= LAUNCH_ATTEMPTS) {
            set_item(index, QITEM_FAILED, "%s", err);
            return 1;
        }
        if (wait_until(LAUNCH_RETRY_GAP, NULL, NULL) < 0) return -1;
    }

    int rc = wait_until(MOUNT_TIMEOUT, title_mounted, (void *)dir);
    if (rc < 0) return -1;
    if (rc == 0) {
        set_item(index, QITEM_FAILED,
                 "the game did not come up within %d seconds", MOUNT_TIMEOUT);
        return 1;
    }

    set_item(index, QITEM_SETTLING, "waiting for the game to finish loading");
    if (wait_until(settle, NULL, NULL) < 0) return -1;

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

    /* a title that is already up is dumped as it is */
    if (!title_mounted(dir)) {
        int rc = bring_up(index, title_id, dir, snap.settle_seconds);
        if (rc != 0) return rc < 0 ? -1 : 0;
    }

    if (g_stop) return -1;

    if (job_start(dir, snap.mount, &g_queue_cfg, err, sizeof(err)) != 0) {
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

        if (g_stop || process_item(i) < 0) stopped = 1;
    }

    int done = 0;

    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_queue.count; i++) {
        queue_item_t *item = &g_queue.items[i];

        if (item->state == QITEM_DONE) { done++; continue; }
        if (item->state == QITEM_FAILED) continue;

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

int queue_start(const char *const *title_ids, int count, const char *mount,
                int settle_seconds, const dumper_config_t *cfg,
                char *err, size_t err_size)
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
    }

    if (settle_seconds < QUEUE_SETTLE_MIN) settle_seconds = QUEUE_SETTLE_MIN;
    if (settle_seconds > QUEUE_SETTLE_MAX) settle_seconds = QUEUE_SETTLE_MAX;

    next.active = 1;
    next.count = count;
    next.current = -1;
    next.settle_seconds = settle_seconds;
    next.started = time(NULL);
    strncpy(next.mount, mount, sizeof(next.mount) - 1);

    /* A finished worker is joined here so its resources are released before
       the next one starts. */
    if (g_worker_valid) {
        pthread_join(g_worker, NULL);
        g_worker_valid = 0;
    }

    g_stop = 0;
    g_queue_cfg = *cfg;

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
    write_log(g_log_path, "Queue: started with %d titles, %d s to load each",
              count, settle_seconds);

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
