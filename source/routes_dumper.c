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

/* The dumper's own routes: what is on the console and the drives, starting
   titles, dumping one or a queue of them, clearing away what was cut short. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "routes.h"
#include "app_scan.h"
#include "app_launch.h"
#include "dump_job.h"
#include "dump_queue.h"
#include "dump_store.h"
#include "dump_library.h"
#include "shadowmount.h"
#include "utils.h"

static void json_job(sb_t *sb, const job_status_t *job)
{
    sb_puts(sb, "{\"state\":");
    sb_json_str(sb, job_state_name(job->state));
    sb_puts(sb, ",\"appDir\":");
    sb_json_str(sb, job->app_dir);
    sb_puts(sb, ",\"title\":");
    sb_json_str(sb, job->title);
    sb_puts(sb, ",\"titleId\":");
    sb_json_str(sb, job->title_id);
    sb_puts(sb, ",\"dest\":");
    sb_json_str(sb, job->dest);
    sb_puts(sb, ",\"stage\":");
    sb_json_str(sb, job->stage);
    sb_puts(sb, ",\"currentFile\":");
    sb_json_str(sb, job->current_file);
    sb_puts(sb, ",\"message\":");
    sb_json_str(sb, job->message);
    sb_printf(sb, ",\"totalBytes\":%llu,\"copiedBytes\":%llu,"
                  "\"started\":%lld,\"copyStarted\":%lld,\"finished\":%lld,\"now\":%lld}",
              (unsigned long long)job->total_bytes,
              (unsigned long long)job->copied_bytes,
              (long long)job->started, (long long)job->copy_started,
              (long long)job->finished, (long long)time(NULL));
}

static void json_queue(sb_t *sb, const queue_status_t *q)
{
    sb_printf(sb, "{\"replace\":%s,", q->replace_existing ? "true" : "false");
    sb_printf(sb, "\"active\":%s,\"current\":%d,\"settle\":%d,\"wait\":%d,"
                  "\"started\":%lld,\"finished\":%lld,\"target\":",
              q->active ? "true" : "false", q->current, q->settle_seconds,
              q->wait_remaining, (long long)q->started, (long long)q->finished);
    sb_json_str(sb, q->mount);
    sb_puts(sb, ",\"items\":[");

    for (int i = 0; i < q->count; i++) {
        if (i) sb_puts(sb, ",");
        sb_puts(sb, "{\"titleId\":");
        sb_json_str(sb, q->items[i].title_id);
        sb_puts(sb, ",\"title\":");
        sb_json_str(sb, q->items[i].title);
        sb_printf(sb, ",\"custom\":%s", q->items[i].custom ? "true" : "false");
        sb_printf(sb, ",\"isDisc\":%s,\"discIn\":%s", q->items[i].is_disc ? "true" : "false",
                  title_on_disc(q->items[i].title_id) ? "true" : "false");
        sb_puts(sb, ",\"state\":");
        sb_json_str(sb, queue_item_state_name(q->items[i].state));
        sb_puts(sb, ",\"message\":");
        sb_json_str(sb, q->items[i].message);
        sb_puts(sb, "}");
    }

    sb_puts(sb, "]}");
}

static void handle_status(whb_req_t *req)
{
    job_status_t job;
    job_get_status(&job);

    /* too large for a connection thread's stack next to the request buffers */
    queue_status_t *queue = malloc(sizeof(*queue));
    if (!queue) { whb_send_error(req, 500, "out of memory"); return; }
    queue_get_status(queue);

    sb_t sb;
    sb_init(&sb);

    sb_puts(&sb, "{\"job\":");
    json_job(&sb, &job);
    sb_puts(&sb, ",\"queue\":");
    json_queue(&sb, queue);
    sb_puts(&sb, ",\"log\":");
    json_log(&sb, (unsigned)whb_param_int(req, "since", 0));
    move_status_t mv;
    dumplib_move_status(&mv);
    static const char *const move_names[] = { "idle", "running", "done", "failed", "aborted" };
    sb_printf(&sb, ",\"move\":{\"state\":\"%s\",\"deleting\":%s,\"totalBytes\":%llu,\"copiedBytes\":%llu,\"folder\":",
              move_names[mv.state], mv.deleting ? "true" : "false", (unsigned long long)mv.total_bytes, (unsigned long long)mv.copied_bytes);
    sb_json_str(&sb, mv.folder);
    sb_puts(&sb, ",\"from\":");    sb_json_str(&sb, mv.from);
    sb_puts(&sb, ",\"to\":");      sb_json_str(&sb, mv.to);
    sb_puts(&sb, ",\"message\":"); sb_json_str(&sb, mv.message);
    sb_puts(&sb, "}");

    sb_printf(&sb, ",\"busy\":%s}",
              (job_is_active() || queue->active || mv.state == MOVE_RUNNING) ? "true" : "false");

    free(queue);

    whb_send_sb(req, 200, &sb);
}

static void handle_devices(whb_req_t *req)
{
    /* Pick up a drive that was plugged in or pulled after the payload
       started. Only a change of place may touch the settings - rereading them
       on every poll would throw away what the user just changed. */
    if (!dumper_busy() && storage_refresh()) whb_config_storage_changed();

    app_entry_t *apps = calloc(APP_SCAN_MAX, sizeof(*apps));
    target_entry_t *targets = calloc(TARGET_SCAN_MAX, sizeof(*targets));
    if (!apps || !targets) {
        free(apps); free(targets);
        whb_send_error(req, 500, "out of memory");
        return;
    }

    int app_count = app_scan(apps, APP_SCAN_MAX);
    int target_count = target_scan(targets, TARGET_SCAN_MAX);

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"apps\":[");

    for (int i = 0; i < app_count; i++) {
        if (i) sb_puts(&sb, ",");
        sb_puts(&sb, "{\"dir\":");
        sb_json_str(&sb, apps[i].dir);
        sb_puts(&sb, ",\"titleId\":");
        sb_json_str(&sb, apps[i].title_id);
        sb_puts(&sb, ",\"title\":");
        sb_json_str(&sb, apps[i].title[0] ? apps[i].title : apps[i].title_id);
        sb_puts(&sb, ",\"version\":");
        sb_json_str(&sb, apps[i].version);
        sb_puts(&sb, ",\"patchDir\":");
        sb_json_str(&sb, apps[i].patch_dir);
        playgo_status_t pg;
        if (title_playgo_status(apps[i].title_id, &pg) != 0) memset(&pg, 0, sizeof(pg));
        sb_printf(&sb, ",\"isPs4\":%s,\"hasIcon\":%s,\"media\":\"%s\",\"discIn\":%s,\"installed\":%d,\"installing\":%s,\"chunks\":[%d,%d,%d]}",
                  apps[i].is_ps4 ? "true" : "false",
                  apps[i].has_icon ? "true" : "false",
                  apps[i].is_disc ? "disc" : "pkg",
                  apps[i].on_disc ? "true" : "false",
                  title_installed_percent(apps[i].title_id),
                  title_install_pending(apps[i].title_id) ? "true" : "false", pg.chunks, pg.wanted, pg.here);
    }

    sb_puts(&sb, "],\"targets\":[");

    for (int i = 0; i < target_count; i++) {
        if (i) sb_puts(&sb, ",");
        sb_puts(&sb, "{\"mount\":");
        sb_json_str(&sb, targets[i].mount);
        sb_puts(&sb, ",\"fs\":");
        sb_json_str(&sb, targets[i].fs);
        sb_printf(&sb, ",\"writable\":%s,\"internal\":%s,\"totalBytes\":%llu,\"freeBytes\":%llu}",
                  targets[i].writable ? "true" : "false", targets[i].internal ? "true" : "false",
                  (unsigned long long)targets[i].total_bytes,
                  (unsigned long long)targets[i].free_bytes);
    }

    /* dumps that were cut short, per drive, so they can be cleared away */
    dumper_config_t cfg_now;
    cfg_snapshot(&cfg_now);

    sb_puts(&sb, "],\"incomplete\":[");
    dump_entry_t *cut = calloc(DUMP_LIST_MAX, sizeof(*cut));
    int cut_total = 0;
    for (int i = 0; cut && i < target_count; i++) {
        char dest[384];
        job_dest_path(targets[i].mount, &cfg_now, dest, sizeof(dest));

        int n = dump_list_incomplete(dest, cut, DUMP_LIST_MAX);
        for (int k = 0; k < n; k++) {
            /* the dump in progress is unfinished too, but not abandoned */
            job_status_t running;
            job_get_status(&running);
            if (job_is_active() && strcmp(running.title_id, cut[k].title_id) == 0) continue;

            if (cut_total++) sb_puts(&sb, ",");
            sb_puts(&sb, "{\"mount\":");
            sb_json_str(&sb, targets[i].mount);
            sb_puts(&sb, ",\"dest\":");
            sb_json_str(&sb, dest);
            sb_puts(&sb, ",\"folder\":");
            sb_json_str(&sb, cut[k].folder);
            sb_puts(&sb, ",\"titleId\":");
            sb_json_str(&sb, cut[k].title_id);
            sb_puts(&sb, ",\"title\":");
            sb_json_str(&sb, cut[k].title[0] ? cut[k].title : cut[k].title_id);
            sb_puts(&sb, ",\"state\":");
            sb_json_str(&sb, cut[k].state);
            sb_puts(&sb, "}");
        }
    }
    free(cut);

    sb_puts(&sb, "],\"config\":");
    whb_config_json(&sb);

    sb_printf(&sb, ",\"configOnConsole\":%s", storage_is_internal() ? "true" : "false");

    /* the page is waiting for a title it started: tell it when that title
       is up without a package mount, which no amount of waiting changes */
    const char *waiting = whb_param(req, "waiting", NULL);
    if (waiting)
        sb_printf(&sb, ",\"waitingRunsFromFolder\":%s",
                  title_runs_from_folder(waiting) ? "true" : "false");
    sb_puts(&sb, ",\"configPath\":");
    const char *hb = get_app_data_path();
    if (hb && hb[0]) {
        char path[256];
        snprintf(path, sizeof(path), "%s/config.ini", hb);
        sb_json_str(&sb, path);
    } else {
        sb_puts(&sb, "null");
    }

    sb_puts(&sb, "}");

    free(apps);
    free(targets);
    whb_send_sb(req, 200, &sb);
}

static void handle_dump(whb_req_t *req)
{
    const char *app = whb_param(req, "app", NULL);
    const char *target = whb_param(req, "target", NULL);

    if (queue_is_active())   { whb_send_error(req, 409, "a queue is running"); return; }
    if (!app || !*app)       { whb_send_error(req, 400, "no app selected"); return; }
    if (!target || !*target) { whb_send_error(req, 400, "no destination selected"); return; }

    dumper_config_t cfg;
    cfg_snapshot(&cfg);

    char err[160] = {0};
    int rc = job_start(app, target, &cfg, whb_param_int(req, "overwrite", 0), err, sizeof(err));

    if (rc == JOB_ERR_EXISTS) {
        /* the page asks, and comes back with overwrite=1 */
        sb_t sb;
        sb_init(&sb);
        sb_puts(&sb, "{\"error\":");
        sb_json_str(&sb, err);
        sb_puts(&sb, ",\"exists\":true}");
        whb_send_sb(req, 409, &sb);
        return;
    }
    if (rc != 0) {
        whb_send_error(req, 409, err[0] ? err : "could not start the dump");
        return;
    }

    sb_t sb;
    sb_init(&sb);
    job_status_t job;
    job_get_status(&job);
    sb_puts(&sb, "{\"started\":true,\"job\":");
    json_job(&sb, &job);
    sb_puts(&sb, "}");
    whb_send_sb(req, 200, &sb);
}

/* Which of these titles are on the drive already - so the page can say so
   before a queue is started, and the choice between skipping and replacing
   them is not made blind. "titles" as for the queue; the general split mode
   decides the folder names, as it does for a title without own settings. */
static void handle_dump_presence(whb_req_t *req)
{
    const char *mount = whb_param(req, "target", NULL);
    if (!mount || target_is_known(mount) != 0) { whb_send_error(req, 400, "unknown drive"); return; }

    dumper_config_t cfg;
    cfg_snapshot(&cfg);

    char dest[384];
    job_dest_path(mount, &cfg, dest, sizeof(dest));

    char list[WHB_PARAM_MAX];
    snprintf(list, sizeof(list), "%s", whb_param(req, "titles", ""));

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"dest\":");
    sb_json_str(&sb, dest);
    sb_puts(&sb, ",\"titles\":{");

    int n = 0;
    for (char *tok = strtok(list, ","); tok && n < QUEUE_MAX; tok = strtok(NULL, ",")) {
        if (strlen(tok) != 9) continue;

        dump_presence_t there = dump_presence(dest, tok, strncmp(tok, "CUSA", 4) == 0, cfg.split);
        if (n++) sb_puts(&sb, ",");
        sb_json_str(&sb, tok);
        sb_printf(&sb, ":\"%s\"", there == DUMP_PRESENT ? "present"
                                 : there == DUMP_INCOMPLETE ? "incomplete" : "absent");
    }

    sb_puts(&sb, "}}");
    whb_send_sb(req, 200, &sb);
}

/* Clears away a dump that was cut short. dump_remove_incomplete() only ever
   deletes a folder whose info file says it is an unfinished dump of ours. */
static void handle_dump_delete(whb_req_t *req)
{
    const char *mount  = whb_param(req, "mount", NULL);
    const char *folder = whb_param(req, "folder", NULL);

    if (!mount || !folder || target_is_known(mount) != 0) { whb_send_error(req, 400, "unknown drive"); return; }
    if (dumper_busy())                                     { whb_send_error(req, 409, "a dump is running"); return; }

    dumper_config_t cfg;
    cfg_snapshot(&cfg);

    char dest[384];
    job_dest_path(mount, &cfg, dest, sizeof(dest));

    if (dump_remove_incomplete(dest, folder) != 0) {
        whb_send_error(req, 409, "not an unfinished dump of this tool - nothing was deleted");
        return;
    }

    write_log(g_log_path, "Web UI: removed the unfinished dump %s/%s", dest, folder);
    whb_send_json(req, 200, "{\"deleted\":true}");
}

static void handle_abort(whb_req_t *req)
{
    /* stopping the dump of a queued title stops the queue with it */
    if (queue_is_active()) {
        queue_stop();
        whb_send_json(req, 200, "{\"stopping\":true}");
        return;
    }

    if (!job_is_active()) {
        whb_send_error(req, 409, "no dump is running");
        return;
    }

    job_abort();
    whb_send_json(req, 200, "{\"stopping\":true}");
}

/* Applies a title's own settings, e.g. "d1f0b1p4q1s3", to cfg: decrypt,
   fself, backport, ps4 level, ps5 level, split. Letters that are missing
   keep the general setting; anything else makes the string invalid. */
static int apply_item_settings(dumper_config_t *cfg, const char *spec)
{
    for (const char *p = spec; *p; ) {
        char key = *p++;
        if (*p < '0' || *p > '9') return -1;

        int val = 0;
        while (*p >= '0' && *p <= '9') val = val * 10 + (*p++ - '0');

        switch (key) {
            case 'd': cfg->enable_decrypter   = val ? 1 : 0;     break;
            case 'f': cfg->enable_elf2fself   = val ? 1 : 0;     break;
            case 'b': cfg->enable_backport    = val ? 1 : 0;     break;
            case 'p': cfg->ps4_backport_level = clamp(val, 1, 6);  break;
            case 'q': cfg->ps5_backport_level = clamp(val, 1, 10); break;
            case 's': cfg->split              = clamp(val, 0, 3);  break;
            default:  return -1;
        }
    }
    return 0;
}

/* "PPSA01234,CUSA05678" -> the titles to dump, in that order. "discs" names
   those among them the user marked as disc games; when the page sends it,
   even empty, it overrules what the scanner believes. */
static void handle_queue_start(whb_req_t *req)
{
    const char *target = whb_param(req, "target", NULL);
    if (!target || !*target) { whb_send_error(req, 400, "no destination selected"); return; }

    char list[WHB_PARAM_MAX];
    snprintf(list, sizeof(list), "%s", whb_param(req, "titles", ""));

    const char *ids[QUEUE_MAX + 1];
    int count = 0;

    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        if (count > QUEUE_MAX) break;   /* one over, so queue_start reports it */
        ids[count++] = tok;
    }

    const char *discs = whb_param(req, "discs", NULL);
    int is_disc[QUEUE_MAX + 1] = {0};
    for (int i = 0; discs && i < count; i++)
        is_disc[i] = (strstr(discs, ids[i]) != NULL);

    dumper_config_t cfg;
    cfg_snapshot(&cfg);

    /* "o_<title id>" carries the settings of a title that has its own */
    dumper_config_t *own = calloc(QUEUE_MAX + 1, sizeof(*own));
    if (!own) { whb_send_error(req, 500, "out of memory"); return; }

    const dumper_config_t *item_cfg[QUEUE_MAX + 1] = {0};
    for (int i = 0; i < count && i <= QUEUE_MAX; i++) {
        char key[32];
        snprintf(key, sizeof(key), "o_%s", ids[i]);

        const char *spec = whb_param(req, key, NULL);
        if (!spec || !*spec) continue;

        own[i] = cfg;
        if (apply_item_settings(&own[i], spec) != 0) {
            free(own);
            whb_send_error(req, 400, "invalid settings for a queued title");
            return;
        }
        item_cfg[i] = &own[i];
    }

    char err[160] = {0};
    int rc = queue_start(ids, discs ? is_disc : NULL, item_cfg, count, target,
                         cfg.queue_delay, whb_param_int(req, "replace", 0),
                         &cfg, err, sizeof(err));
    free(own);

    if (rc != 0) {
        whb_send_error(req, 409, err[0] ? err : "could not start the queue");
        return;
    }

    whb_send_json(req, 200, "{\"started\":true}");
}

static void handle_queue_skip(whb_req_t *req)
{
    if (queue_skip() != 0) {
        whb_send_error(req, 409, "the queue is not working on a title right now");
        return;
    }

    whb_send_json(req, 200, "{\"skipping\":true}");
}

static void handle_queue_clear(whb_req_t *req)
{
    if (queue_clear() != 0) {
        whb_send_error(req, 409, "the queue is still running");
        return;
    }

    whb_send_json(req, 200, "{\"cleared\":true}");
}

static void handle_icon(whb_req_t *req)
{
    const char *dir = whb_param(req, "app", NULL);
    if (!dir || !*dir) { whb_send_error(req, 400, "no app given"); return; }

    app_entry_t app;
    char path[512];

    if (app_find(dir, &app) != 0 || app_icon_path(&app, path, sizeof(path)) != 0) {
        whb_send_error(req, 404, "no icon");
        return;
    }

    whb_send_file(req, path, "image/png");
}

static void handle_size(whb_req_t *req)
{
    const char *dir = whb_param(req, "app", NULL);
    if (!dir || !*dir) { whb_send_error(req, 400, "no app given"); return; }

    app_entry_t app;
    if (app_find(dir, &app) != 0) { whb_send_error(req, 404, "app is not mounted"); return; }

    char json[128];
    snprintf(json, sizeof(json), "{\"bytes\":%llu}", (unsigned long long)app_size(&app));
    whb_send_json(req, 200, json);
}

static void handle_library(whb_req_t *req)
{
    /* A console with a full library needs ~25 KB here, which is more than a
       connection thread's stack can take. */
    library_entry_t *lib = calloc(LIBRARY_SCAN_MAX, sizeof(*lib));
    if (!lib) { whb_send_error(req, 500, "out of memory"); return; }

    int count = library_scan(lib, LIBRARY_SCAN_MAX);

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"titles\":[");

    for (int i = 0; i < count; i++) {
        if (i) sb_puts(&sb, ",");
        sb_puts(&sb, "{\"titleId\":");
        sb_json_str(&sb, lib[i].title_id);
        sb_puts(&sb, ",\"title\":");
        sb_json_str(&sb, lib[i].title[0] ? lib[i].title : lib[i].title_id);
        sb_puts(&sb, ",\"version\":");
        sb_json_str(&sb, lib[i].version);
        sb_puts(&sb, ",\"source\":");
        sb_json_str(&sb, lib[i].source);
        sb_printf(&sb, ",\"isPs4\":%s,\"hasIcon\":%s,\"hasPic\":%s,\"isRunning\":%s,"
                       "\"media\":\"%s\",\"discIn\":%s",
                  lib[i].is_ps4 ? "true" : "false",
                  lib[i].has_icon ? "true" : "false",
                  lib[i].has_pic ? "true" : "false",
                  lib[i].is_running ? "true" : "false",
                  lib[i].is_disc ? "disc" : "pkg",
                  lib[i].on_disc ? "true" : "false");
        sb_printf(&sb, ",\"installed\":%d,\"installing\":%s,\"chunks\":[%d,%d,%d]", lib[i].installed_pct, lib[i].install_pending ? "true" : "false",
                  lib[i].playgo.chunks, lib[i].playgo.wanted, lib[i].playgo.here);
        sb_puts(&sb, ",\"mountedFrom\":");
        sb_json_str(&sb, lib[i].mounted_from);
        sb_puts(&sb, "}");
    }

    /* Whether a title is up is already known from the pfsmnt scan, so the
       polling listing never asks the system service - that call reaches
       into SceLncUtil and is not worth doing every few seconds while a
       game is running. */
    int running = 0;
    for (int i = 0; i < count; i++)
        if (lib[i].is_running) running = 1;

    sb_printf(&sb, "],\"aTitleIsRunning\":%s,\"canLaunch\":%s}",
              running ? "true" : "false",
              app_launch_probably_available() ? "true" : "false");

    free(lib);
    whb_send_sb(req, 200, &sb);
}

/* ------------------------------------------------------------------ */
/*  The dumps that exist                                               */
/* ------------------------------------------------------------------ */

static void handle_dumps(whb_req_t *req)
{
    /* what is on the console is only listed for those who are in */
    int with_internal = whb_peer_is_local(req) || whb_access_token_ok(whb_param(req, "token", NULL));

    dumplib_entry_t *list = calloc(DUMPLIB_MAX, sizeof(*list));
    if (!list) { whb_send_error(req, 500, "out of memory"); return; }
    int count = dumplib_scan(list, DUMPLIB_MAX, with_internal);

    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"consoleListed\":%s,\"shadowMountRunning\":%s,\"dumps\":[",
              with_internal ? "true" : "false", shadowmount_pid() ? "true" : "false");
    for (int i = 0; i < count; i++) {
        if (i) sb_puts(&sb, ",");
        sb_puts(&sb, "{\"mount\":");   sb_json_str(&sb, list[i].mount);
        sb_puts(&sb, ",\"dir\":");     sb_json_str(&sb, list[i].dir);
        sb_puts(&sb, ",\"folder\":");  sb_json_str(&sb, list[i].folder);
        sb_puts(&sb, ",\"titleId\":"); sb_json_str(&sb, list[i].title_id);
        sb_puts(&sb, ",\"title\":");   sb_json_str(&sb, list[i].title[0] ? list[i].title : list[i].title_id);
        sb_puts(&sb, ",\"state\":");   sb_json_str(&sb, list[i].state);
        sb_printf(&sb, ",\"internal\":%s,\"inUse\":%s,\"hasIcon\":%s,\"bytes\":%llu,\"fself\":%d}",
                  list[i].internal ? "true" : "false", list[i].in_use ? "true" : "false",
                  list[i].has_icon ? "true" : "false",
                  (unsigned long long)list[i].bytes, list[i].fself);
    }
    sb_puts(&sb, "]}");
    free(list);
    whb_send_sb(req, 200, &sb);
}

/* FSELF after the fact, for a dump made without it. */
static void handle_dumps_fself(whb_req_t *req)
{
    int converted = 0, skipped = 0;
    char err[160];
    if (dumplib_fself(whb_param(req, "mount", NULL), whb_param(req, "dir", ""), whb_param(req, "folder", NULL),
                      &converted, &skipped, err, sizeof(err)) != 0) {
        whb_send_error(req, 400, err);
        return;
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"converted\":%d,\"skipped\":%d}", converted, skipped);
    whb_send_json(req, 200, json);
}

static void handle_dumps_unfself(whb_req_t *req)
{
    int restored = 0, skipped = 0;
    char err[160];
    if (dumplib_unfself(whb_param(req, "mount", NULL), whb_param(req, "dir", ""), whb_param(req, "folder", NULL),
                        &restored, &skipped, err, sizeof(err)) != 0) {
        whb_send_error(req, 400, err);
        return;
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"restored\":%d,\"skipped\":%d}", restored, skipped);
    whb_send_json(req, 200, json);
}

static void handle_dump_icon(whb_req_t *req)
{
    const char *mount = whb_param(req, "mount", "");

    /* what is on the console is only shown to those who are in */
    if (!strcmp(mount, storage_internal_root()) && !whb_peer_is_local(req) &&
        !whb_access_token_ok(whb_param(req, "token", NULL))) {
        whb_send_error(req, 401, "enter the code shown on the TV first");
        return;
    }

    char path[512];
    if (dumplib_icon_path(mount, whb_param(req, "dir", ""), whb_param(req, "folder", ""), path, sizeof(path)) != 0) {
        whb_send_error(req, 404, "no icon");
        return;
    }
    whb_send_file(req, path, "image/png");
}

/* Takes the link off a title that is redirected to this dump, without moving
   the dump: the game then starts from its installed package again. */
static void handle_dump_unlink(whb_req_t *req)
{
    const char *mount  = whb_param(req, "mount", NULL);
    const char *dir    = whb_param(req, "dir", "");
    const char *folder = whb_param(req, "folder", NULL);

    if (!mount || !folder || target_is_known(mount) != 0) { whb_send_error(req, 400, "unknown drive"); return; }
    if (!dump_folder_name_ok(folder) || !fs_path_is_safe(dir)) { whb_send_error(req, 400, "not a dump folder"); return; }

    char id[16], path[384];
    snprintf(id, sizeof(id), "%.9s", folder);
    snprintf(path, sizeof(path), "%s%s%s/%s", mount, dir[0] ? "/" : "", dir, folder);

    if (title_runs_from_folder(id)) {
        whb_send_error(req, 409, "the game is running from this folder right now - close it first");
        return;
    }
    if (!title_drop_mount_link(id, path)) {
        whb_send_error(req, 409, "no installed game is linked to this dump");
        return;
    }

    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"unlinked\":true,\"shadowMountRunning\":%s}", shadowmount_pid() ? "true" : "false");
    whb_send_sb(req, 200, &sb);
}

/* Only ever because the user said so, with the consequences in front of them. */
static void handle_shadowmount_stop(whb_req_t *req)
{
    if (!shadowmount_pid()) { whb_send_json(req, 200, "{\"stopped\":true,\"wasRunning\":false}"); return; }
    if (shadowmount_stop() != 0) { whb_send_error(req, 500, "ShadowMount would not stop"); return; }
    whb_send_json(req, 200, "{\"stopped\":true,\"wasRunning\":true}");
}

static void handle_dump_move(whb_req_t *req)
{
    char err[256] = {0};
    if (dumplib_move(whb_param(req, "mount", NULL), whb_param(req, "dir", ""), whb_param(req, "folder", NULL),
                     whb_param(req, "toMount", NULL), whb_param(req, "toDir", ""), err, sizeof(err)) != 0) {
        whb_send_error(req, 409, err[0] ? err : "could not move the dump");
        return;
    }

    move_status_t mv;
    dumplib_move_status(&mv);
    whb_send_json(req, 200, mv.state == MOVE_RUNNING ? "{\"started\":true,\"copying\":true}"
                                                : "{\"started\":true,\"copying\":false}");
}

static void handle_dump_remove(whb_req_t *req)
{
    char err[256] = {0};
    if (dumplib_delete(whb_param(req, "mount", NULL), whb_param(req, "dir", ""), whb_param(req, "folder", NULL),
                       whb_param(req, "confirm", NULL), err, sizeof(err)) != 0) {
        whb_send_error(req, 409, err[0] ? err : "could not delete the dump");
        return;
    }
    whb_send_json(req, 200, "{\"started\":true}");
}

static void handle_dump_move_cancel(whb_req_t *req)
{
    dumplib_move_cancel();
    whb_send_json(req, 200, "{\"stopping\":true}");
}

static void handle_dump_move_clear(whb_req_t *req)
{
    dumplib_move_clear();
    whb_send_json(req, 200, "{\"cleared\":true}");
}

static void handle_launch(whb_req_t *req)
{
    const char *title = whb_param(req, "title", NULL);
    if (!title || !*title) { whb_send_error(req, 400, "no title given"); return; }

    if (dumper_busy()) {
        whb_send_error(req, 409, "a dump is running");
        return;
    }

    /* Said before anything is closed: starting a disc game without its disc
       would end the running game and then fail. A title that ShadowMount
       serves from a dump folder starts without the disc. */
    library_entry_t lib;
    int mounted = library_find(title, &lib) == 0 && lib.mounted_from[0];
    if (!mounted && title_is_disc_game(title) && !title_on_disc(title)) {
        whb_send_error(req, 409, "this game needs its disc - insert it and try again");
        return;
    }

    int close_running = whb_param_int(req, "force", 0);
    char err[192] = {0};

    if (app_launch_title(title, close_running, err, sizeof(err)) != 0) {
        /* the caller may retry with force=1 once the player agrees */
        int running = (strstr(err, "another game is running") != NULL);
        sb_t sb;
        sb_init(&sb);
        sb_puts(&sb, "{\"error\":");
        sb_json_str(&sb, err[0] ? err : "could not start the title");
        sb_printf(&sb, ",\"needsClose\":%s}", running ? "true" : "false");
        whb_send_sb(req, 409, &sb);
        return;
    }

    whb_send_json(req, 200, "{\"launched\":true}");
}

static void handle_library_icon(whb_req_t *req)
{
    const char *title = whb_param(req, "title", NULL);
    if (!title || !*title) { whb_send_error(req, 400, "no title given"); return; }

    /* library_icon_path only accepts a title id, so no path can be injected */
    char path[512];
    if (library_icon_path(title, path, sizeof(path)) != 0) {
        whb_send_error(req, 404, "no icon");
        return;
    }

    whb_send_file(req, path, "image/png");
}

static void handle_library_pic(whb_req_t *req)
{
    const char *title = whb_param(req, "title", NULL);
    if (!title || !*title) { whb_send_error(req, 400, "no title given"); return; }

    /* library_pic_path only accepts a title id, so no path can be injected */
    char path[512];
    if (library_pic_path(title, path, sizeof(path)) != 0) {
        whb_send_error(req, 404, "no artwork");
        return;
    }

    whb_send_file(req, path, "image/png");
}

void routes_dumper_init(void)
{
    whb_route("GET",  "/api/status",         handle_status);
    whb_route("GET",  "/api/devices",        handle_devices);
    whb_route("GET",  "/api/library",        handle_library);
    whb_route("GET",  "/api/icon",           handle_icon);
    whb_route("GET",  "/api/libicon",        handle_library_icon);
    whb_route("GET",  "/api/libpic",         handle_library_pic);
    whb_route("GET",  "/api/size",           handle_size);
    whb_route("POST", "/api/launch",         handle_launch);
    whb_route("POST", "/api/dump",           handle_dump);
    whb_route("POST", "/api/abort",          handle_abort);
    whb_route("GET",  "/api/dumps/presence", handle_dump_presence);
    whb_route("POST", "/api/dumps/delete",   handle_dump_delete);
    whb_route("POST", "/api/dumps/fself",   handle_dumps_fself);
    whb_route("POST", "/api/dumps/unfself", handle_dumps_unfself);
    whb_route("GET",  "/api/dumps",          handle_dumps);
    whb_route("GET",  "/api/dumps/icon",     handle_dump_icon);
    whb_route("POST", "/api/shadowmount/stop", handle_shadowmount_stop);
    whb_route("POST", "/api/dumps/unlink",   handle_dump_unlink);
    whb_route("POST", "/api/dumps/move",     handle_dump_move);
    whb_route("POST", "/api/dumps/remove",   handle_dump_remove);
    whb_route("POST", "/api/dumps/move/cancel", handle_dump_move_cancel);
    whb_route("POST", "/api/dumps/move/clear",  handle_dump_move_clear);
    whb_route("POST", "/api/queue/start",    handle_queue_start);
    whb_route("POST", "/api/queue/skip",     handle_queue_skip);
    whb_route("POST", "/api/queue/clear",    handle_queue_clear);
}
