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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pthread.h>
#include <time.h>

#include "app_scan.h"
#include "utils.h"

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

/* The title's name in the language param.json calls its default, else in
   en-US, else the first one in the file. The localized entries are listed
   alphabetically, so the first "titleName" is usually the Arabic one. */
static int json_read_title(const char *path, char *out, size_t out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    buf[n] = '\0';

    char lang[24] = {0};
    const char *p = strstr(buf, "\"defaultLanguage\"");
    if (p && (p = strchr(p + 17, '"')) != NULL) {
        size_t i = 0;
        for (p++; *p && *p != '"' && i + 1 < sizeof(lang); p++) lang[i++] = *p;
        lang[i] = '\0';
    }

    const char *tries[] = { lang[0] ? lang : NULL, "en-US" };
    for (int t = 0; t < 2; t++) {
        if (!tries[t]) continue;
        char needle[40];
        snprintf(needle, sizeof(needle), "\"%s\"", tries[t]);
        const char *block = strstr(buf, needle);
        if (!block) continue;
        const char *end = strchr(block, '}');
        const char *name = strstr(block, "\"titleName\"");
        if (!name || (end && name > end)) continue;
        name = strchr(name + 11, '"');
        if (!name) continue;
        size_t i = 0;
        for (name++; *name && *name != '"' && i + 1 < out_size; name++) {
            if (*name == '\\' && name[1]) name++;
            out[i++] = *name;
        }
        out[i] = '\0';
        if (out[0]) return 0;
    }
    return json_read_string(path, "titleName", out, out_size);
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

            if (!title[0])   json_read_title(path, title, title_size);
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

/* A running title's DLC are mounted next to it under pfsmnt, one folder per
   content id: <region>-<TITLEID>_00-<LABEL>-ac, with a -nest twin that is
   the same package seen another way. Seen on FW 12.00 and 10.60 (Horizon
   Forbidden West, Burning Shores). Two places tell: the directory listing,
   and the mount table - a console that shows the mounts but not the
   listing (a report from FW 10.60 read that way) still has the table. */
static int dlc_name_fits(const char *name, const char *mark)
{
    size_t len = strlen(name);
    if (len < 20 || len >= 96) return 0;
    if (!strstr(name, mark)) return 0;
    return strcmp(name + len - 3, "-ac") == 0;
}

static void dlc_add(app_entry_t *app, const char *name)
{
    for (int i = 0; i < app->dlc_count; i++)
        if (strcmp(app->dlc[i], name) == 0) return;
    if (app->dlc_count >= (int)(sizeof(app->dlc) / sizeof(app->dlc[0]))) return;
    snprintf(app->dlc[app->dlc_count++], sizeof(app->dlc[0]), "%s", name);
}

static void app_find_dlc(app_entry_t *app)
{
    app->dlc_count = 0;

    char mark[24];
    snprintf(mark, sizeof(mark), "-%s_00-", app->title_id);

    DIR *d = opendir(SANDBOX_PATH);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)))
            if (dlc_name_fits(ent->d_name, mark)) dlc_add(app, ent->d_name);
        closedir(d);
    }

    struct statfs *mnt;
    int n = getmntinfo(&mnt, MNT_NOWAIT);
    const size_t plen = strlen(SANDBOX_PATH "/");
    for (int i = 0; i < n; i++) {
        const char *on = mnt[i].f_mntonname;
        if (strncmp(on, SANDBOX_PATH "/", plen) != 0 || strchr(on + plen, '/')) continue;
        if (dlc_name_fits(on + plen, mark)) dlc_add(app, on + plen);
    }
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
        app_find_dlc(&out[count]);
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

    app_find_dlc(out);
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

static int is_title_id(const char *name);

int title_runs_from_folder(const char *title_id)
{
    if (!is_title_id(title_id)) return 0;

    char path[160];
    snprintf(path, sizeof(path), "%s/%s-app0", SANDBOX_PATH, title_id);
    if (dir_exists(path)) return 0;

    snprintf(path, sizeof(path), "/mnt/sandbox/%s_000/app0", title_id);
    return dir_exists(path);
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
    const char *hb = get_app_data_path();
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

    /* The console keeps a disc_info.dat with the metadata of every title that
       was installed from a disc, and of no other. Checked on FW 12.00 with
       two disc installs (one with its disc out) against two package titles. */
    char path[96];
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/disc_info.dat", title_id);
    if (file_exists(path)) return 1;

    /* What was assumed before that was known; seen in PS4 kernel logs, never
       on this PS5. Costs nothing to keep. */
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

int library_pic_path(const char *title_id, char *out, size_t out_size)
{
    if (!out || out_size == 0 || !is_title_id(title_id)) return -1;

    static const char *names[] = { "pic0.png", "pic1.png", NULL };
    char dirs[2][256];
    appmeta_dirs(title_id, dirs);

    for (int n = 0; names[n]; n++) {
        for (int i = 0; i < 2; i++) {
            snprintf(out, out_size, "%s/%s", dirs[i], names[n]);
            if (file_exists(out)) return 0;
        }
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
/* ------------------------------------------------------------------ */
/*  How much of a package has arrived                                  */
/* ------------------------------------------------------------------ */

/* <root>/<id>/app.pbm, as found on FW 12.00 for PS4 and PS5 titles alike:
     0x000  "pdbm", the title id, a version string
     0x022  number of 64 KiB blocks in app.pkg, little endian
     0x100  one bit per block, highest bit of a byte first, set once the
            block is on the console
     ...    a 32-byte digest
   Every file looked at was exactly 256 + ceil(blocks / 8) + 32 bytes. A
   title installing from disc had 63 % of its bits set, finished ones all. */
#define PBM_HEADER 256
#define PBM_DIGEST 32

static int pbm_percent(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char head[PBM_HEADER];
    int pct = -1;

    if (fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "pdbm", 4) == 0) {
        uint32_t blocks = (uint32_t)head[0x22] | ((uint32_t)head[0x23] << 8) |
                          ((uint32_t)head[0x24] << 16) | ((uint32_t)head[0x25] << 24);
        uint64_t set = 0, seen = 0;
        unsigned char buf[8192];
        size_t n;

        while (blocks && seen < blocks && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
            for (size_t i = 0; i < n && seen < blocks; i++)
                for (int bit = 0; bit < 8 && seen < blocks; bit++, seen++)
                    if (buf[i] & (0x80u >> bit)) set++;
        }

        /* a file that does not hold what its header promises tells nothing */
        if (blocks && seen == blocks) {
            pct = (int)(set * 100 / blocks);
            if (pct == 100 && set != blocks) pct = 99;
        }
    }

    fclose(f);
    return pct;
}

/* The listing is polled; the bitmap is only read again when it changed. */
static struct { char id[16]; time_t mtime; off_t size; int pct; } g_pbm_cache[64];
static pthread_mutex_t g_pbm_mtx = PTHREAD_MUTEX_INITIALIZER;

static int installed_percent_at(const char *root, const char *title_id)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s/app.pbm", root, title_id);

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    pthread_mutex_lock(&g_pbm_mtx);
    int slot = -1, spare = -1;
    for (int i = 0; i < 64; i++) {
        if (!strcmp(g_pbm_cache[i].id, title_id)) { slot = i; break; }
        if (spare < 0 && !g_pbm_cache[i].id[0]) spare = i;
    }
    if (slot >= 0 && g_pbm_cache[slot].mtime == st.st_mtime && g_pbm_cache[slot].size == st.st_size) {
        int pct = g_pbm_cache[slot].pct;
        pthread_mutex_unlock(&g_pbm_mtx);
        return pct;
    }
    pthread_mutex_unlock(&g_pbm_mtx);

    int pct = pbm_percent(path);

    pthread_mutex_lock(&g_pbm_mtx);
    if (slot < 0) slot = spare;
    if (slot >= 0) {
        snprintf(g_pbm_cache[slot].id, sizeof(g_pbm_cache[slot].id), "%s", title_id);
        g_pbm_cache[slot].mtime = st.st_mtime;
        g_pbm_cache[slot].size  = st.st_size;
        g_pbm_cache[slot].pct   = pct;
    }
    pthread_mutex_unlock(&g_pbm_mtx);
    return pct;
}

/* A package comes in PlayGo chunks, and the console fetches only the ones
   it needs - the languages it is set to, say. So the bitmap can stand below
   100 % for good. What the shell thinks about it is in <root>/<id>/app.xml
   ("playgo-status"): one <chunk> per chunk with locus="3" when it is here,
   and req_locus="3" when this console wants it; a chunk it does not want
   may be left out altogether, chunk_count says how many there are. Seen on
   FW 12.00: Hogwarts Legacy at 73 % with 7 of 21 chunks wanted, all 7
   present, the bitmap untouched for days.
   The shell writes the file when the install starts and does not always come
   back to it: FF7 Rebirth and Remake (FW 10.60) still said locus="0" for most
   wanted chunks three months after the bitmap last moved, Remake's at 100 %.
   So which chunks are wanted can be read here, whether they arrived cannot. */
static int playgo_status_at(const char *root, const char *title_id, playgo_status_t *st)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s/app.xml", root, title_id);

    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *xml = calloc(1, 65536);
    if (!xml) { fclose(f); return -1; }
    size_t n = fread(xml, 1, 65535, f);
    fclose(f);
    xml[n] = '\0';

    memset(st, 0, sizeof(*st));
    if (!strstr(xml, "playgo-status")) { free(xml); return -1; }

    const char *cc = strstr(xml, "chunk_count=\"");
    int declared = cc ? atoi(cc + 13) : 0;

    for (const char *p = strstr(xml, "<chunk "); p; p = strstr(p + 1, "<chunk ")) {
        const char *end = strchr(p, '>');
        if (!end) break;
        const char *l = strstr(p, "locus=\"");
        const char *r = strstr(p, "req_locus=\"");
        /* "locus" also matches inside "req_locus": take the one that is not */
        while (l && l < end && l > p && l[-1] == '_') l = strstr(l + 1, "locus=\"");
        int locus = (l && l < end) ? atoi(l + 7) : 0;
        int want  = (r && r < end) ? atoi(r + 11) : -1;
        st->chunks++;
        if (want >= 0) {
            st->wanted++;
            if (locus == want) st->here++;
        }
    }
    free(xml);
    if (declared > st->chunks) st->chunks = declared;
    return st->chunks ? 0 : -1;
}

int title_playgo_status(const char *title_id, playgo_status_t *st)
{
    if (!is_title_id(title_id)) return -1;
    for (int r = 0; g_app_roots[r].path; r++)
        if (playgo_status_at(g_app_roots[r].path, title_id, st) == 0) return 0;
    return -1;
}

/* Blocks still arrive while the bitmap keeps changing. A PlayGo status that
   has every wanted chunk here settles it at once; one that does not is not
   believed (see playgo_status_at), the bitmap decides. */
#define INSTALL_QUIET_SECONDS (10 * 60)

int title_install_pending(const char *title_id)
{
    int pct = title_installed_percent(title_id);
    if (pct < 0 || pct >= 100) return 0;

    playgo_status_t st;
    if (title_playgo_status(title_id, &st) == 0 && st.wanted && st.here >= st.wanted) return 0;

    for (int r = 0; g_app_roots[r].path; r++) {
        char path[320];
        snprintf(path, sizeof(path), "%s/%s/app.pbm", g_app_roots[r].path, title_id);
        struct stat s;
        if (stat(path, &s) == 0) return (time(NULL) - s.st_mtime) < INSTALL_QUIET_SECONDS;
    }
    return 0;
}

/* The folder a title is redirected to, read from <root>/<id>/mount.lnk - a
   single line, the path. Empty when there is no such file. */
static void read_mount_link(const char *root, const char *title_id, char *out, size_t out_size)
{
    out[0] = '\0';

    char path[320];
    snprintf(path, sizeof(path), "%s/%s/mount.lnk", root, title_id);
    FILE *f = fopen(path, "r");
    if (!f) return;

    size_t n = fread(out, 1, out_size - 1, f);
    fclose(f);
    out[n] = '\0';
    out[strcspn(out, "\r\n")] = '\0';

    /* something is there but unreadable as a path: still a redirect */
    if (!out[0] || out[0] != '/') snprintf(out, out_size, "%s", "(unknown folder)");
}

/* "DBSZ0CHAPOPACK00": sixteen capitals and digits, as the content id ends */
static int is_addcont_label(const char *s)
{
    if (strlen(s) != 16) return 0;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9'))) return 0;
    return 1;
}

static void library_find_addcont(library_entry_t *e)
{
    static const char *const roots[] = { "/user/addcont", "/mnt/ext0/user/addcont", "/mnt/ext1/user/addcont", NULL };

    e->addcont_count = 0;
    for (int r = 0; roots[r]; r++) {
        char dir[160];
        snprintf(dir, sizeof(dir), "%s/%s", roots[r], e->title_id);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) && e->addcont_count < DLC_MAX) {
            if (!is_addcont_label(ent->d_name)) continue;
            int seen = 0;
            for (int i = 0; i < e->addcont_count; i++)
                if (strcmp(e->addcont[i], ent->d_name) == 0) seen = 1;
            if (seen) continue;

            char pkg[224];
            struct stat st;
            snprintf(pkg, sizeof(pkg), "%s/%s/ac.pkg", dir, ent->d_name);
            if (stat(pkg, &st) != 0) continue;
            snprintf(e->addcont[e->addcont_count], sizeof(e->addcont[0]), "%s", ent->d_name);
            e->addcont_bytes[e->addcont_count++] = (uint64_t)st.st_size;
        }
        closedir(d);
    }
}

static void library_fill(library_entry_t *e, const char *title_id, const char *root, const char *label)
{
    memset(e, 0, sizeof(*e));
    read_mount_link(root, title_id, e->mounted_from, sizeof(e->mounted_from));
    e->installed_pct = installed_percent_at(root, title_id);
    e->install_pending = title_install_pending(title_id);
    if (title_playgo_status(title_id, &e->playgo) != 0) memset(&e->playgo, 0, sizeof(e->playgo));
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
    e->has_pic  = (library_pic_path(e->title_id, icon, sizeof(icon)) == 0);

    char mounted[320];
    snprintf(mounted, sizeof(mounted), "%s/%s-app0", SANDBOX_PATH, e->title_id);
    e->is_running = dir_exists(mounted) || title_runs_from_folder(e->title_id);
    e->on_disc = title_on_disc(e->title_id);
    e->is_disc = e->on_disc || title_is_disc_game(e->title_id);
    /* a title ShadowMount serves from a dump folder does not need its disc */
    if (e->mounted_from[0] && !e->on_disc) e->is_disc = 0;

    library_find_addcont(e);
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

            library_fill(&out[count], dp->d_name, g_app_roots[r].path, g_app_roots[r].label);
            count++;
        }
        closedir(d);
    }

    return count;
}

int title_drop_mount_link(const char *title_id, const char *target)
{
    if (!is_title_id(title_id) || !target || !target[0]) return 0;

    for (int r = 0; g_app_roots[r].path; r++) {
        char now[160], path[320];
        read_mount_link(g_app_roots[r].path, title_id, now, sizeof(now));
        if (strcmp(now, target) != 0) continue;

        snprintf(path, sizeof(path), "%s/%s/mount.lnk", g_app_roots[r].path, title_id);
        if (unlink(path) == 0) {
            write_log(g_log_path, "Removed %s - it pointed at %s, which has been moved", path, target);
            return 1;
        }
    }
    return 0;
}

int title_installed_percent(const char *title_id)
{
    if (!is_title_id(title_id)) return -1;

    for (int r = 0; g_app_roots[r].path; r++) {
        int pct = installed_percent_at(g_app_roots[r].path, title_id);
        if (pct >= 0) return pct;
    }
    return -1;
}

int library_find(const char *title_id, library_entry_t *out)
{
    if (!out || !is_title_id(title_id)) return -1;

    for (int r = 0; g_app_roots[r].path; r++) {
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", g_app_roots[r].path, title_id);
        if (!dir_exists(path)) continue;

        library_fill(out, title_id, g_app_roots[r].path, g_app_roots[r].label);
        return 0;
    }

    return -1;
}
