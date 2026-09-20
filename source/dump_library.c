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

/* The dumps that exist, and moving them. See dump_library.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "dump_library.h"
#include "dump_store.h"
#include "app_scan.h"
#include "routes.h"
#include "utils.h"

/* how deep below a destination's root a dump is still looked for, and how
   many folders one scan opens at most - a drive can hold a lot that is not
   ours */
#define SCAN_DEPTH     3
#define SCAN_DIRS_MAX  500

/* what a move onto the console's own storage has to leave free */
#ifndef INTERNAL_RESERVE
#define INTERNAL_RESERVE (10ull << 30)
#endif

/* ------------------------------------------------------------------ */
/*  Finding them                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    dumplib_entry_t *out;
    int max, count, dirs;
    const target_entry_t *target;
    const library_entry_t *lib;
    int lib_count;
} scan_ctx_t;

static int skip_dir(const char *name)
{
    return name[0] == '.' || name[0] == '$' ||
           !strcmp(name, "System Volume Information") || !strcmp(name, "logs") ||
           /* a console's save data export: folders named after title ids, and no dumps */
           !strcmp(name, "SAVEDATA");
}

/* A title id is not enough of a name: save data, screenshots and other tools
   file things under it too. A dump is a folder with our info file next to it,
   or with a game's insides - sce_sys, an eboot.bin. Anything else is neither
   listed nor ever moved. */
static int looks_like_dump(const char *dest, const char *folder)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s" DUMP_INFO_SUFFIX, dest, folder);
    if (file_exists(path)) return 1;

    snprintf(path, sizeof(path), "%s/%s/sce_sys", dest, folder);
    if (dir_exists(path)) return 1;

    snprintf(path, sizeof(path), "%s/%s/eboot.bin", dest, folder);
    return file_exists(path);
}

/* "bytes": 123 - the one number the library wants from an info file */
static uint64_t info_bytes(const char *dest, const char *folder)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s" DUMP_INFO_SUFFIX, dest, folder);

    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *p = strstr(buf, "\"bytes\": ");
    return p ? strtoull(p + 9, NULL, 10) : 0;
}

static void add_entry(scan_ctx_t *c, const char *rel, const char *name)
{
    if (c->count >= c->max) return;

    dumplib_entry_t *e = &c->out[c->count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->mount, sizeof(e->mount), "%s", c->target->mount);
    e->internal = c->target->internal;
    snprintf(e->dir, sizeof(e->dir), "%s", rel);
    snprintf(e->folder, sizeof(e->folder), "%s", name);
    snprintf(e->title_id, sizeof(e->title_id), "%.9s", name);

    char dest[256];
    if (rel[0]) snprintf(dest, sizeof(dest), "%s/%s", e->mount, rel);
    else        snprintf(dest, sizeof(dest), "%s", e->mount);

    dump_info_string(dest, name, "state", e->state, sizeof(e->state));
    dump_info_string(dest, name, "title", e->title, sizeof(e->title));
    e->bytes = info_bytes(dest, name);

    char full[384], icon[448];
    snprintf(full, sizeof(full), "%s/%s", dest, name);
    snprintf(icon, sizeof(icon), "%s/sce_sys/icon0.png", full);
    e->has_icon = file_exists(icon);
    for (int i = 0; i < c->lib_count; i++) {
        if (strcmp(c->lib[i].title_id, e->title_id) != 0) continue;
        if (!e->title[0]) snprintf(e->title, sizeof(e->title), "%s", c->lib[i].title);
        if (!strcmp(c->lib[i].mounted_from, full)) e->in_use = 1;
    }
}

static void walk(scan_ctx_t *c, const char *rel, int depth)
{
    if (c->dirs++ >= SCAN_DIRS_MAX || c->count >= c->max) return;

    char path[384];
    if (rel[0]) snprintf(path, sizeof(path), "%s/%s", c->target->mount, rel);
    else        snprintf(path, sizeof(path), "%s", c->target->mount);

    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && c->count < c->max) {
        if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        char child[256];
        if (rel[0]) { if (snprintf(child, sizeof(child), "%s/%s", rel, ent->d_name) >= (int)sizeof(child)) continue; }
        else        snprintf(child, sizeof(child), "%s", ent->d_name);

        if (dump_folder_name_ok(ent->d_name)) {
            /* a dump is never looked into: it is somebody's game, not a place for more */
            if (strlen(rel) < sizeof(((dumplib_entry_t *)0)->dir) && looks_like_dump(path, ent->d_name))
                add_entry(c, rel, ent->d_name);
        } else if (depth < SCAN_DEPTH && !skip_dir(ent->d_name)) {
            walk(c, child, depth + 1);
        }
    }
    closedir(d);
}

int dumplib_scan(dumplib_entry_t *out, int max, int with_internal)
{
    if (!out || max <= 0) return 0;

    target_entry_t *targets = calloc(TARGET_SCAN_MAX, sizeof(*targets));
    library_entry_t *lib = calloc(64, sizeof(*lib));
    if (!targets || !lib) { free(targets); free(lib); return 0; }

    scan_ctx_t c = { .out = out, .max = max, .lib = lib, .lib_count = library_scan(lib, 64) };
    int target_count = target_scan(targets, TARGET_SCAN_MAX);

    for (int t = 0; t < target_count; t++) {
        c.target = &targets[t];
        c.dirs = 0;

        if (!targets[t].internal) { walk(&c, "", 0); continue; }
        if (!with_internal) continue;

        /* The console's storage is not crawled from the top: homebrew, and
           wherever the settings send dumps on the console. */
        dumper_config_t cfg;
        cfg_snapshot(&cfg);
        walk(&c, "homebrew", 1);

        char first[64];
        snprintf(first, sizeof(first), "%s", cfg.dump_subdir_console);
        first[strcspn(first, "/")] = '\0';
        if (first[0] && strcmp(first, "homebrew") != 0) walk(&c, first, 1);
    }

    free(targets);
    free(lib);
    return c.count;
}

int dumplib_icon_path(const char *mount, const char *dir, const char *folder,
                      char *out, size_t out_size)
{
    if (!mount || !dir || !folder || !out || !out_size) return -1;
    if (!dump_folder_name_ok(folder) || !fs_path_is_safe(dir)) return -1;
    if (target_is_known(mount) != 0) return -1;

    char dest[256];
    snprintf(dest, sizeof(dest), "%s%s%s", mount, dir[0] ? "/" : "", dir);
    if (!looks_like_dump(dest, folder)) return -1;

    snprintf(out, out_size, "%s/%s/sce_sys/icon0.png", dest, folder);
    return file_exists(out) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Moving one                                                         */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static move_status_t   g_move;

typedef struct {
    char src[384], dst[384];            /* the dump folder, old and new    */
    char src_info[448], dst_info[448];  /* its info file, old and new      */
} move_req_t;

static void move_finish(move_state_t state, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&g_mtx);
    g_move.state = state;
    va_start(ap, fmt);
    vsnprintf(g_move.message, sizeof(g_move.message), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_mtx);
    write_log(g_log_path, "Move: %s", g_move.message);
}

/* src is <...>/<TITLEID>[-app0]: whoever was redirected there is set free */
static void drop_stale_link(const char *src)
{
    const char *name = strrchr(src, '/');
    if (!name) return;
    char id[16];
    snprintf(id, sizeof(id), "%.9s", name + 1);
    title_drop_mount_link(id, src);
}

static void *move_thread(void *arg)
{
    move_req_t *req = arg;

    copy_dir_recursive_tracked(req->src, req->dst);

    if (abort_requested()) {
        dump_remove_tree(req->dst);
        clear_abort();
        move_finish(MOVE_ABORTED, "stopped - %s stays where it was", g_move.folder);
        free(req);
        return NULL;
    }

    /* The original goes only when the copy is all there. */
    size_t copied = 0;
    size_walker(req->dst, &copied);
    if ((uint64_t)copied != g_move.total_bytes) {
        dump_remove_tree(req->dst);
        move_finish(MOVE_FAILED, "the copy is not complete (%llu of %llu MB) - %s stays where it was",
                    (unsigned long long)(copied >> 20), (unsigned long long)(g_move.total_bytes >> 20),
                    g_move.folder);
        free(req);
        return NULL;
    }

    if (file_exists(req->src_info)) fs_copy_file(req->src_info, req->dst_info);

    int left = dump_remove_tree(req->src);
    unlink(req->src_info);
    if (left == 0) drop_stale_link(req->src);

    if (left != 0)
        move_finish(MOVE_DONE, "%s is now in %s - but the original could not be removed completely",
                    g_move.folder, g_move.to);
    else
        move_finish(MOVE_DONE, "%s is now in %s", g_move.folder, g_move.to);

    free(req);
    return NULL;
}

int dumplib_move(const char *mount, const char *dir, const char *folder,
                 const char *to_mount, const char *to_dir, char *err, size_t err_size)
{
    #define FAIL(...) do { if (err && err_size) snprintf(err, err_size, __VA_ARGS__); return -1; } while (0)

    if (!mount || !dir || !folder || !to_mount || !to_dir) FAIL("missing parameters");
    if (!dump_folder_name_ok(folder))                      FAIL("not a dump folder");
    if (!fs_path_is_safe(dir) || !fs_path_is_safe(to_dir)) FAIL("invalid folder");
    if (target_is_known(mount) != 0 || target_is_known(to_mount) != 0) FAIL("unknown drive");
    if (dumper_busy())                                     FAIL("a dump or a move is running");

    move_req_t *req = calloc(1, sizeof(*req));
    if (!req) FAIL("out of memory");

    char from_dir[256], dest_dir[256];
    snprintf(from_dir, sizeof(from_dir), "%s%s%s", mount, dir[0] ? "/" : "", dir);
    snprintf(dest_dir, sizeof(dest_dir), "%s%s%s", to_mount, to_dir[0] ? "/" : "", to_dir);
    snprintf(req->src, sizeof(req->src), "%s/%s", from_dir, folder);
    snprintf(req->dst, sizeof(req->dst), "%s/%s", dest_dir, folder);
    snprintf(req->src_info, sizeof(req->src_info), "%s" DUMP_INFO_SUFFIX, req->src);
    snprintf(req->dst_info, sizeof(req->dst_info), "%s" DUMP_INFO_SUFFIX, req->dst);

    #undef FAIL
    #define FAIL(...) do { if (err && err_size) snprintf(err, err_size, __VA_ARGS__); free(req); return -1; } while (0)

    if (!dir_exists(req->src))           FAIL("the dump is not there any more");
    if (!looks_like_dump(from_dir, folder)) FAIL("that folder is not a dump - it is left alone");
    if (!strcmp(req->src, req->dst))     FAIL("it is in that folder already");
    if (dir_exists(req->dst) || file_exists(req->dst))
        FAIL("%s already holds a %s - move or remove that one first", dest_dir, folder);

    /* a title that is being played from this folder holds it open */
    char id[16];
    snprintf(id, sizeof(id), "%.9s", folder);
    library_entry_t title;
    if (library_find(id, &title) == 0 && !strcmp(title.mounted_from, req->src) && title_runs_from_folder(id))
        FAIL("%s is running from this folder right now - close the game first", title.title[0] ? title.title : id);

    mkdirs(dest_dir);
    if (!dir_exists(dest_dir)) FAIL("could not create %s", dest_dir);

    pthread_mutex_lock(&g_mtx);
    memset(&g_move, 0, sizeof(g_move));
    snprintf(g_move.folder, sizeof(g_move.folder), "%s", folder);
    snprintf(g_move.from, sizeof(g_move.from), "%s", from_dir);
    snprintf(g_move.to, sizeof(g_move.to), "%s", dest_dir);
    pthread_mutex_unlock(&g_mtx);

    /* within one drive nothing is copied */
    if (!strcmp(mount, to_mount)) {
        if (rename(req->src, req->dst) != 0) {
            int e = errno;
            FAIL("could not move it (%s)", strerror(e));
        }
        if (file_exists(req->src_info)) rename(req->src_info, req->dst_info);
        drop_stale_link(req->src);
        move_finish(MOVE_DONE, "%s is now in %s", folder, dest_dir);
        free(req);
        return 0;
    }

    size_t total = 0;
    size_walker(req->src, &total);

    struct statfs sf;
    if (statfs(dest_dir, &sf) == 0) {
        uint64_t avail = (uint64_t)sf.f_bavail * sf.f_bsize;
        uint64_t reserve = strcmp(to_mount, storage_internal_root()) == 0 ? INTERNAL_RESERVE : 0;
        if ((uint64_t)total + reserve > avail)
            FAIL("not enough space: it needs %.1f GB%s, %.1f GB are free", total / 1073741824.0,
                 reserve ? " and 10 GB have to stay free" : "", avail / 1073741824.0);
    }

    pthread_mutex_lock(&g_mtx);
    g_move.state = MOVE_RUNNING;
    g_move.total_bytes = (uint64_t)total;
    snprintf(g_move.message, sizeof(g_move.message), "copying");
    pthread_mutex_unlock(&g_mtx);

    clear_abort();
    total_bytes_copied = 0;
    write_log(g_log_path, "Move: copying %s to %s (%llu MB)", req->src, dest_dir,
              (unsigned long long)(total >> 20));

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    int started = pthread_create(&tid, &attr, move_thread, req);
    pthread_attr_destroy(&attr);

    if (started != 0) {
        pthread_mutex_lock(&g_mtx);
        g_move.state = MOVE_FAILED;
        pthread_mutex_unlock(&g_mtx);
        FAIL("could not start the move");
    }
    return 0;

    #undef FAIL
}

/* ------------------------------------------------------------------ */
/*  Deleting one                                                       */
/* ------------------------------------------------------------------ */

static void *delete_thread(void *arg)
{
    move_req_t *req = arg;

    int left = dump_remove_tree(req->src);
    if (left == 0) {
        unlink(req->src_info);
        drop_stale_link(req->src);
        move_finish(MOVE_DONE, "%s was deleted", g_move.folder);
    } else {
        move_finish(MOVE_FAILED, "%s could not be deleted completely - look at what is left in %s",
                    g_move.folder, g_move.from);
    }

    free(req);
    return NULL;
}

int dumplib_delete(const char *mount, const char *dir, const char *folder,
                   const char *confirm, char *err, size_t err_size)
{
    #define FAIL(...) do { if (err && err_size) snprintf(err, err_size, __VA_ARGS__); return -1; } while (0)

    if (!mount || !dir || !folder)                FAIL("missing parameters");
    if (!dump_folder_name_ok(folder))             FAIL("not a dump folder");
    if (!confirm || strcmp(confirm, folder) != 0) FAIL("not confirmed");
    if (!fs_path_is_safe(dir))                    FAIL("invalid folder");
    if (target_is_known(mount) != 0)              FAIL("unknown drive");
    if (dumper_busy())                            FAIL("a dump or a move is running");

    char from_dir[256];
    snprintf(from_dir, sizeof(from_dir), "%s%s%s", mount, dir[0] ? "/" : "", dir);

    move_req_t *req = calloc(1, sizeof(*req));
    if (!req) FAIL("out of memory");
    snprintf(req->src, sizeof(req->src), "%s/%s", from_dir, folder);
    snprintf(req->src_info, sizeof(req->src_info), "%s" DUMP_INFO_SUFFIX, req->src);

    #undef FAIL
    #define FAIL(...) do { if (err && err_size) snprintf(err, err_size, __VA_ARGS__); free(req); return -1; } while (0)

    if (!dir_exists(req->src))              FAIL("the dump is not there any more");
    if (!looks_like_dump(from_dir, folder)) FAIL("that folder is not a dump - it is left alone");

    char id[16];
    snprintf(id, sizeof(id), "%.9s", folder);
    library_entry_t title;
    if (library_find(id, &title) == 0 && !strcmp(title.mounted_from, req->src) && title_runs_from_folder(id))
        FAIL("%s is running from this folder right now - close the game first", title.title[0] ? title.title : id);

    pthread_mutex_lock(&g_mtx);
    memset(&g_move, 0, sizeof(g_move));
    g_move.state = MOVE_RUNNING;
    g_move.deleting = 1;
    snprintf(g_move.folder, sizeof(g_move.folder), "%s", folder);
    snprintf(g_move.from, sizeof(g_move.from), "%s", from_dir);
    snprintf(g_move.message, sizeof(g_move.message), "deleting");
    pthread_mutex_unlock(&g_mtx);

    write_log(g_log_path, "Delete: removing %s - asked for and confirmed in the web UI", req->src);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    int started = pthread_create(&tid, &attr, delete_thread, req);
    pthread_attr_destroy(&attr);

    if (started != 0) {
        pthread_mutex_lock(&g_mtx);
        g_move.state = MOVE_FAILED;
        pthread_mutex_unlock(&g_mtx);
        FAIL("could not start deleting");
    }
    return 0;

    #undef FAIL
}

int dumplib_move_active(void)
{
    pthread_mutex_lock(&g_mtx);
    int active = g_move.state == MOVE_RUNNING;
    pthread_mutex_unlock(&g_mtx);
    return active;
}

void dumplib_move_cancel(void)
{
    /* a delete is not stopped half way: what it leaves would be neither a
       dump nor gone */
    pthread_mutex_lock(&g_mtx);
    int stoppable = g_move.state == MOVE_RUNNING && !g_move.deleting;
    pthread_mutex_unlock(&g_mtx);
    if (stoppable) request_abort();
}

void dumplib_move_status(move_status_t *out)
{
    pthread_mutex_lock(&g_mtx);
    *out = g_move;
    if (out->state == MOVE_RUNNING) out->copied_bytes = (uint64_t)total_bytes_copied;
    else if (out->state == MOVE_DONE) out->copied_bytes = out->total_bytes;
    pthread_mutex_unlock(&g_mtx);
}

void dumplib_move_clear(void)
{
    pthread_mutex_lock(&g_mtx);
    if (g_move.state != MOVE_RUNNING) memset(&g_move, 0, sizeof(g_move));
    pthread_mutex_unlock(&g_mtx);
}
