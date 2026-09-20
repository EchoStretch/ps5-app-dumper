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
#include <dirent.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <pthread.h>

#include "app_scan.h"
#include "utils.h"

/* Mount points that may hold a dump, probed in this order. */
static const char *g_candidate_mounts[] = {
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
    "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
    "/mnt/ext0", "/mnt/ext1",
    NULL
};

/* ------------------------------------------------------------------ */
/*  param.sfo (PS4 titles)                                             */
/* ------------------------------------------------------------------ */

#define SFO_MAGIC 0x46535000u  /* "\0PSF" little endian */

struct sfo_header {
    uint32_t magic;
    uint32_t version;
    uint32_t key_table_offset;
    uint32_t data_table_offset;
    uint32_t num_entries;
};

struct sfo_index {
    uint16_t key_offset;
    uint16_t param_fmt;
    uint32_t param_len;
    uint32_t param_max_len;
    uint32_t data_offset;
};

/* Reads a UTF-8 string value out of a param.sfo. Returns 0 on success. */
static int sfo_read_string(const char *path, const char *key,
                           char *out, size_t out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    struct sfo_header hdr;
    int ret = -1;

    if (fread(&hdr, sizeof(hdr), 1, f) != 1) goto done;
    if (hdr.magic != SFO_MAGIC) goto done;
    if (hdr.num_entries == 0 || hdr.num_entries > 1024) goto done;

    for (uint32_t i = 0; i < hdr.num_entries; i++) {
        struct sfo_index idx;

        if (fseek(f, sizeof(hdr) + i * sizeof(idx), SEEK_SET) != 0) goto done;
        if (fread(&idx, sizeof(idx), 1, f) != 1) goto done;

        /* the key table holds NUL terminated names back to back */
        char name[64] = {0};
        if (fseek(f, hdr.key_table_offset + idx.key_offset, SEEK_SET) != 0) goto done;
        size_t got = fread(name, 1, sizeof(name) - 1, f);
        if (got == 0) continue;
        name[got] = '\0';

        if (strcmp(name, key) != 0) continue;
        if (idx.param_fmt != 0x0204) goto done;   /* not a UTF-8 string */

        size_t len = idx.param_len;
        if (len == 0) goto done;
        if (len > out_size - 1) len = out_size - 1;

        if (fseek(f, hdr.data_table_offset + idx.data_offset, SEEK_SET) != 0) goto done;
        if (fread(out, 1, len, f) != len) goto done;

        out[len] = '\0';
        ret = 0;
        break;
    }

done:
    fclose(f);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  param.json (PS5 titles)                                            */
/* ------------------------------------------------------------------ */

/* Pulls the first "<key>": "<value>" pair out of a param.json. The file is
   small (a few KB) and we only need two fields, so a scan beats a parser. */
static int json_read_string(const char *path, const char *key,
                            char *out, size_t out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';

    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);

    char *p = strstr(buf, needle);
    if (!p) return -1;

    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && p[1]) p++;   /* keep escaped characters verbatim */
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Metadata lookup                                                    */
/* ------------------------------------------------------------------ */

/* sce_sys lives in the mount itself; appmeta copies are the fallback for
   titles whose sandbox does not expose the metadata. */
static void meta_dirs(const app_entry_t *app, char dirs[3][256])
{
    snprintf(dirs[0], 256, "%s/%s/sce_sys", SANDBOX_PATH, app->dir);
    snprintf(dirs[1], 256, "/system_data/priv/appmeta/%s", app->title_id);
    snprintf(dirs[2], 256, "/user/appmeta/%s", app->title_id);
}

/* Reads title and version out of the first directory that carries them. */
static void read_metadata(char dirs[][256], int dir_count, int is_ps4,
                          char *title, size_t title_size,
                          char *version, size_t version_size)
{
    for (int i = 0; i < dir_count; i++) {
        char path[320];

        if (is_ps4) {
            snprintf(path, sizeof(path), "%s/param.sfo", dirs[i]);
            if (!file_exists(path)) continue;

            if (!title[0])   sfo_read_string(path, "TITLE", title, title_size);
            if (!version[0]) sfo_read_string(path, "APP_VER", version, version_size);
        } else {
            snprintf(path, sizeof(path), "%s/param.json", dirs[i]);
            if (!file_exists(path)) continue;

            if (!title[0])   json_read_string(path, "titleName", title, title_size);
            if (!version[0]) json_read_string(path, "contentVersion", version, version_size);
        }

        if (title[0] && version[0]) break;
    }
}

/* The appmeta copies, which exist whether or not the title is running. */
static void appmeta_dirs(const char *title_id, char dirs[2][256])
{
    snprintf(dirs[0], 256, "/system_data/priv/appmeta/%s", title_id);
    snprintf(dirs[1], 256, "/user/appmeta/%s", title_id);
}

static void app_read_metadata(app_entry_t *app)
{
    char dirs[3][256];
    meta_dirs(app, dirs);

    read_metadata(dirs, 3, app->is_ps4,
                  app->title, sizeof(app->title),
                  app->version, sizeof(app->version));

    char icon[512];
    app->has_icon = (app_icon_path(app, icon, sizeof(icon)) == 0);
    app->on_disc = title_on_disc(app->title_id);
    app->is_disc = app->on_disc || title_is_disc_game(app->title_id);
}

int app_icon_path(const app_entry_t *app, char *out, size_t out_size)
{
    if (!app || !out || out_size == 0) return -1;

    char dirs[3][256];
    meta_dirs(app, dirs);

    for (int i = 0; i < 3; i++) {
        snprintf(out, out_size, "%s/icon0.png", dirs[i]);
        if (file_exists(out)) return 0;
    }

    out[0] = '\0';
    return -1;
}

/* ------------------------------------------------------------------ */
/*  App discovery                                                      */
/* ------------------------------------------------------------------ */

/* Turns "PPSA01234-app0" into a filled entry. Returns 0 when the folder
   name describes an app mount. */
static int app_from_dirname(const char *name, app_entry_t *out)
{
    size_t len = strlen(name);
    if (len <= 5 || len >= sizeof(out->dir)) return -1;

    int is_ppsa = (strncmp(name, "PPSA", 4) == 0);
    int is_cusa = (strncmp(name, "CUSA", 4) == 0);
    if (!is_ppsa && !is_cusa) return -1;
    if (strcmp(name + len - 5, "-app0") != 0) return -1;

    memset(out, 0, sizeof(*out));
    strncpy(out->dir, name, sizeof(out->dir) - 1);
    out->is_ps4 = is_cusa;

    size_t id_len = len - 5;
    if (id_len >= sizeof(out->title_id)) id_len = sizeof(out->title_id) - 1;
    memcpy(out->title_id, name, id_len);
    out->title_id[id_len] = '\0';

    return 0;
}

/* PS4 titles keep their update data in a separate -patch0 mount. */
static void app_find_patch(app_entry_t *app)
{
    if (!app->is_ps4) return;

    char patch[128], path[320];
    snprintf(patch, sizeof(patch), "%s-patch0", app->title_id);
    snprintf(path, sizeof(path), "%s/%s", SANDBOX_PATH, patch);

    if (dir_exists(path))
        strncpy(app->patch_dir, patch, sizeof(app->patch_dir) - 1);
}

int app_scan(app_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;

    DIR *d = opendir(SANDBOX_PATH);
    if (!d) return 0;

    int count = 0;
    struct dirent *dp;

    while ((dp = readdir(d)) && count < max) {
        if (dp->d_type != DT_DIR) continue;
        if (app_from_dirname(dp->d_name, &out[count]) != 0) continue;

        app_find_patch(&out[count]);
        app_read_metadata(&out[count]);
        count++;
    }

    closedir(d);
    return count;
}

int app_find(const char *dir, app_entry_t *out)
{
    if (!dir || !out) return -1;
    if (app_from_dirname(dir, out) != 0) return -1;

    char path[320];
    snprintf(path, sizeof(path), "%s/%s", SANDBOX_PATH, dir);
    if (!dir_exists(path)) return -1;

    app_find_patch(out);
    app_read_metadata(out);
    return 0;
}

uint64_t app_size(const app_entry_t *app)
{
    if (!app) return 0;

    size_t total = 0;
    char path[320];

    snprintf(path, sizeof(path), "%s/%s", SANDBOX_PATH, app->dir);
    size_walker(path, &total);

    if (app->patch_dir[0]) {
        snprintf(path, sizeof(path), "%s/%s", SANDBOX_PATH, app->patch_dir);
        size_walker(path, &total);
    }

    return (uint64_t)total;
}

/* ------------------------------------------------------------------ */
/*  Storage targets                                                    */
/* ------------------------------------------------------------------ */

int target_scan(target_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;

    int count = 0;

    for (int i = 0; g_candidate_mounts[i] && count < max; i++) {
        const char *mount = g_candidate_mounts[i];
        if (!dir_exists(mount)) continue;

        target_entry_t *t = &out[count];
        memset(t, 0, sizeof(*t));
        strncpy(t->mount, mount, sizeof(t->mount) - 1);

        struct statfs sf;
        if (statfs(mount, &sf) == 0) {
            /* Unused mount points exist as empty directories on the root
               file system, so they answer statfs with the root's own type
               and a couple of megabytes - not somewhere a dump can go. */
            if (strcmp(sf.f_fstypename, "exfatfs") != 0 &&
                strcmp(sf.f_fstypename, "msdosfs") != 0 &&
                strcmp(sf.f_fstypename, "ntfs")    != 0 &&
                strcmp(sf.f_fstypename, "ufs")     != 0 &&
                strcmp(sf.f_fstypename, "fusefs")  != 0)
                continue;

            uint64_t total = (uint64_t)sf.f_blocks * sf.f_bsize;
            if (total < 64ull * 1024 * 1024) continue;

            strncpy(t->fs, sf.f_fstypename, sizeof(t->fs) - 1);
            t->total_bytes = total;
            t->free_bytes  = (uint64_t)sf.f_bavail * sf.f_bsize;
            t->writable    = (sf.f_flags & MNT_RDONLY) ? 0 : 1;
        } else {
            /* statfs is unavailable for this mount - assume it is usable
               and let the dump report the real error. */
            t->writable = 1;
        }

        count++;
    }

    return count;
}

int target_is_known(const char *mount)
{
    if (!mount || !mount[0]) return -1;

    /* checked against the scan so a placeholder mount cannot be selected */
    target_entry_t list[TARGET_SCAN_MAX];
    int count = target_scan(list, TARGET_SCAN_MAX);

    for (int i = 0; i < count; i++)
        if (strcmp(mount, list[i].mount) == 0) return 0;

    return -1;
}

/* ------------------------------------------------------------------ */
/*  Installed titles                                                   */
/* ------------------------------------------------------------------ */

/* Where the console keeps installed applications. */
static const struct { const char *path, *label; } g_app_roots[] = {
    { "/user/app",          "internal" },
    { "/mnt/ext0/user/app", "ext0"     },
    { "/mnt/ext1/user/app", "ext1"     },
    { NULL, NULL }
};

/* "PPSA01234" / "CUSA01234" - anything else is not a game folder. */
static int is_title_id(const char *name)
{
    if (!name) return 0;
    if (strlen(name) != 9) return 0;
    if (strncmp(name, "PPSA", 4) != 0 && strncmp(name, "CUSA", 4) != 0) return 0;

    for (int i = 4; i < 9; i++)
        if (name[i] < '0' || name[i] > '9') return 0;

    return 1;
}

/* An inserted disc is mounted here, one folder per title it carries - for
   PS4 and PS5 discs alike. */
#define DISC_APP_ROOT "/mnt/disc/app"

/* ---- disc games seen so far ---------------------------------------- */

#define DISC_MEMORY_MAX  128
#define DISC_MEMORY_FILE "disc_titles.txt"

static pthread_mutex_t g_disc_mtx = PTHREAD_MUTEX_INITIALIZER;
static char            g_disc_known[DISC_MEMORY_MAX][16];
static int             g_disc_known_count = 0;
static int             g_disc_file_read = 0;

static int disc_known_locked(const char *title_id)
{
    for (int i = 0; i < g_disc_known_count; i++)
        if (strcmp(g_disc_known[i], title_id) == 0) return 1;
    return 0;
}

static int disc_memory_path(char *out, size_t out_size)
{
    const char *hb = get_usb_homebrew_path();
    if (!hb || !hb[0]) return -1;
    snprintf(out, out_size, "%s/%s", hb, DISC_MEMORY_FILE);
    return 0;
}

/* The list lives on the drive, which may turn up after the payload started,
   so reading it is retried until it worked once. */
static void disc_memory_load_locked(void)
{
    if (g_disc_file_read) return;

    char path[256];
    if (disc_memory_path(path, sizeof(path)) != 0) return;
    g_disc_file_read = 1;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[64];
    while (fgets(line, sizeof(line), f) && g_disc_known_count < DISC_MEMORY_MAX) {
        line[strcspn(line, "\r\n")] = '\0';
        if (is_title_id(line) && !disc_known_locked(line))
            strcpy(g_disc_known[g_disc_known_count++], line);
    }
    fclose(f);
}

void title_remember_disc(const char *title_id)
{
    if (!is_title_id(title_id)) return;

    pthread_mutex_lock(&g_disc_mtx);
    disc_memory_load_locked();

    if (!disc_known_locked(title_id) && g_disc_known_count < DISC_MEMORY_MAX) {
        strcpy(g_disc_known[g_disc_known_count++], title_id);

        /* rewritten whole: what was learned before a drive showed up has
           to reach the file as well */
        char path[256];
        FILE *f = disc_memory_path(path, sizeof(path)) == 0 ? fopen(path, "w") : NULL;
        if (f) {
            for (int i = 0; i < g_disc_known_count; i++)
                fprintf(f, "%s\n", g_disc_known[i]);
            fclose(f);
        }
    }
    pthread_mutex_unlock(&g_disc_mtx);
}

int title_on_disc(const char *title_id)
{
    if (!is_title_id(title_id)) return 0;

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", DISC_APP_ROOT, title_id);
    if (!dir_exists(path)) return 0;

    title_remember_disc(title_id);
    return 1;
}

int title_is_disc_game(const char *title_id)
{
    if (!is_title_id(title_id)) return 0;
    if (title_on_disc(title_id)) return 1;

    pthread_mutex_lock(&g_disc_mtx);
    disc_memory_load_locked();
    int known = disc_known_locked(title_id);
    pthread_mutex_unlock(&g_disc_mtx);
    if (known) return 1;

    /* A disc install keeps a bitmap of what was copied off the disc. Seen in
       PS4 kernel logs; unconfirmed on PS5, hence only one signal of three. */
    char path[96];
    snprintf(path, sizeof(path), "/system_data/playgo/%s/bdcopy.pbm", title_id);
    return file_exists(path) ? 1 : 0;
}

int library_icon_path(const char *title_id, char *out, size_t out_size)
{
    if (!title_id || !out || out_size == 0) return -1;

    char dirs[2][256];
    appmeta_dirs(title_id, dirs);

    for (int i = 0; i < 2; i++) {
        snprintf(out, out_size, "%s/icon0.png", dirs[i]);
        if (file_exists(out)) return 0;
    }

    out[0] = '\0';
    return -1;
}

static int library_seen(const library_entry_t *list, int count, const char *title_id)
{
    for (int i = 0; i < count; i++)
        if (strcmp(list[i].title_id, title_id) == 0) return 1;
    return 0;
}

/* Fills an entry for a title found below one of the app roots. */
static void library_fill(library_entry_t *e, const char *title_id, const char *label)
{
    memset(e, 0, sizeof(*e));
    strncpy(e->title_id, title_id, sizeof(e->title_id) - 1);
    strncpy(e->source, label, sizeof(e->source) - 1);
    e->is_ps4 = (strncmp(title_id, "CUSA", 4) == 0);

    char dirs[2][256];
    appmeta_dirs(e->title_id, dirs);
    read_metadata(dirs, 2, e->is_ps4,
                  e->title, sizeof(e->title),
                  e->version, sizeof(e->version));

    char icon[512];
    e->has_icon = (library_icon_path(e->title_id, icon, sizeof(icon)) == 0);

    char mounted[320];
    snprintf(mounted, sizeof(mounted), "%s/%s-app0", SANDBOX_PATH, e->title_id);
    e->is_running = dir_exists(mounted);
    e->on_disc = title_on_disc(e->title_id);
    e->is_disc = e->on_disc || title_is_disc_game(e->title_id);
}

int library_scan(library_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;

    int count = 0;

    for (int r = 0; g_app_roots[r].path && count < max; r++) {
        DIR *d = opendir(g_app_roots[r].path);
        if (!d) continue;

        struct dirent *dp;
        while ((dp = readdir(d)) && count < max) {
            if (!is_title_id(dp->d_name)) continue;
            if (library_seen(out, count, dp->d_name)) continue;

            library_fill(&out[count], dp->d_name, g_app_roots[r].label);
            count++;
        }
        closedir(d);
    }

    return count;
}

int library_find(const char *title_id, library_entry_t *out)
{
    if (!out || !is_title_id(title_id)) return -1;

    for (int r = 0; g_app_roots[r].path; r++) {
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", g_app_roots[r].path, title_id);
        if (!dir_exists(path)) continue;

        library_fill(out, title_id, g_app_roots[r].label);
        return 0;
    }

    return -1;
}
