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

#include "dump_job.h"
#include "app_scan.h"
#include "ps4_dumper.h"
#include "ps5_dumper.h"
#include "utils.h"

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
/* serialises job_start() so two requests cannot both pass the active check */
static pthread_mutex_t g_start_mtx = PTHREAD_MUTEX_INITIALIZER;
static job_status_t    g_status;
static pthread_t       g_worker;
static int             g_worker_valid = 0;

/* Snapshot of the job request, owned by the worker thread. */
typedef struct {
    app_entry_t     app;
    char            dest[384];
    dumper_config_t cfg;
} job_request_t;

const char *job_state_name(job_state_t state)
{
    switch (state) {
        case JOB_IDLE:      return "idle";
        case JOB_PREPARING: return "preparing";
        case JOB_RUNNING:   return "running";
        case JOB_DONE:      return "done";
        case JOB_FAILED:    return "failed";
        case JOB_ABORTED:   return "aborted";
    }
    return "unknown";
}

static void set_stage(const char *stage)
{
    pthread_mutex_lock(&g_mtx);
    strncpy(g_status.stage, stage, sizeof(g_status.stage) - 1);
    g_status.stage[sizeof(g_status.stage) - 1] = '\0';
    pthread_mutex_unlock(&g_mtx);
}

static void finish(job_state_t state, const char *fmt, ...)
{
    char msg[sizeof(g_status.message)];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_mtx);
    g_status.state = state;
    g_status.finished = time(NULL);
    strncpy(g_status.message, msg, sizeof(g_status.message) - 1);
    g_status.message[sizeof(g_status.message) - 1] = '\0';
    strncpy(g_status.stage, job_state_name(state), sizeof(g_status.stage) - 1);
    g_status.stage[sizeof(g_status.stage) - 1] = '\0';
    pthread_mutex_unlock(&g_mtx);
}

static void *worker(void *arg)
{
    job_request_t *req = (job_request_t *)arg;
    int rc;

    /* The backport modules read their levels straight from config.ini, so
       the only global left to propagate is the PS4 split mode. */
    g_split_mode = req->cfg.split;
    g_enable_logging = req->cfg.enable_logging;

    snprintf(g_log_path, sizeof(g_log_path), "%s/log.txt", req->dest);

    set_stage("Measuring");
    uint64_t estimate = app_size(&req->app);

    pthread_mutex_lock(&g_mtx);
    g_status.total_bytes = estimate;
    g_status.state = JOB_RUNNING;
    pthread_mutex_unlock(&g_mtx);

    set_stage("Dumping");
    write_log(g_log_path, "Web UI: dumping %s to %s", req->app.dir, req->dest);

    if (req->app.is_ps4) {
        rc = dump_ps4_cusa_app(SANDBOX_PATH, req->app.dir, req->app.patch_dir,
                               req->dest, req->cfg.enable_decrypter,
                               req->cfg.enable_elf2fself, req->cfg.enable_backport);
    } else {
        rc = dump_ps5_ppsa_app(SANDBOX_PATH, req->app.dir, req->dest,
                               req->cfg.enable_decrypter,
                               req->cfg.enable_elf2fself, req->cfg.enable_backport);
    }

    if (abort_requested()) {
        finish(JOB_ABORTED, "Dump stopped, %s is incomplete", req->dest);
        printf_notification("Dump stopped by user");
    } else if (rc != 0) {
        finish(JOB_FAILED, "Dump failed with code %d", rc);
    } else {
        finish(JOB_DONE, "Dump complete: %s", req->dest);
    }

    free(req);
    return NULL;
}

int job_is_active(void)
{
    pthread_mutex_lock(&g_mtx);
    int active = (g_status.state == JOB_PREPARING || g_status.state == JOB_RUNNING);
    pthread_mutex_unlock(&g_mtx);
    return active;
}

int job_start(const char *app_dir, const char *mount,
              const dumper_config_t *cfg, char *err, size_t err_size)
{
    #define FAIL(msg) do { if (err && err_size) snprintf(err, err_size, "%s", msg); \
                           pthread_mutex_unlock(&g_start_mtx); return -1; } while (0)

    pthread_mutex_lock(&g_start_mtx);

    if (!app_dir || !mount || !cfg) FAIL("missing parameters");
    if (job_is_active())            FAIL("a dump is already running");
    if (target_is_known(mount) != 0) FAIL("unknown destination");

    app_entry_t app;
    if (app_find(app_dir, &app) != 0)
        FAIL("app is no longer mounted, start the game first");

    if (!dir_exists(mount)) FAIL("destination is not mounted");

    job_request_t *req = calloc(1, sizeof(*req));
    if (!req) FAIL("out of memory");

    req->app = app;
    req->cfg = *cfg;

    if (cfg->dump_subdir[0])
        snprintf(req->dest, sizeof(req->dest), "%s/%s", mount, cfg->dump_subdir);
    else
        snprintf(req->dest, sizeof(req->dest), "%s", mount);

    mkdirs(req->dest);
    if (!dir_exists(req->dest)) {
        free(req);
        FAIL("cannot create the destination folder");
    }

    /* A finished worker is joined here so its resources are released before
       the next one starts. */
    if (g_worker_valid) {
        pthread_join(g_worker, NULL);
        g_worker_valid = 0;
    }

    clear_abort();

    /* the copy routines report progress through these globals */
    folder_size_current = 0;
    total_bytes_copied  = 0;
    copy_start_time     = 0;
    current_copied[0]   = '\0';

    pthread_mutex_lock(&g_mtx);
    memset(&g_status, 0, sizeof(g_status));
    g_status.state = JOB_PREPARING;
    g_status.started = time(NULL);
    strncpy(g_status.app_dir,  app.dir,      sizeof(g_status.app_dir) - 1);
    strncpy(g_status.title,    app.title,    sizeof(g_status.title) - 1);
    strncpy(g_status.title_id, app.title_id, sizeof(g_status.title_id) - 1);
    strncpy(g_status.dest,     req->dest,    sizeof(g_status.dest) - 1);
    strncpy(g_status.stage,    "Preparing",  sizeof(g_status.stage) - 1);
    pthread_mutex_unlock(&g_mtx);

    if (pthread_create(&g_worker, NULL, worker, req) != 0) {
        free(req);
        finish(JOB_FAILED, "could not start the worker thread");
        FAIL("could not start the worker thread");
    }

    g_worker_valid = 1;
    pthread_mutex_unlock(&g_start_mtx);
    return 0;

    #undef FAIL
}

void job_abort(void)
{
    if (!job_is_active()) return;

    request_abort();
    set_stage("Stopping");
    write_log(g_log_path, "Web UI: stop requested");
}

void job_get_status(job_status_t *out)
{
    if (!out) return;

    pthread_mutex_lock(&g_mtx);
    *out = g_status;
    pthread_mutex_unlock(&g_mtx);

    if (out->state == JOB_RUNNING || out->state == JOB_PREPARING) {
        /* live figures kept by the copy routines */
        if (folder_size_current > 0)
            out->total_bytes = (uint64_t)folder_size_current;
        out->copied_bytes = (uint64_t)total_bytes_copied;

        strncpy(out->current_file, current_copied, sizeof(out->current_file) - 1);
        out->current_file[sizeof(out->current_file) - 1] = '\0';
    }
}
