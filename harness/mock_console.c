/* Host harness: a pretend console behind the real web server, job, queue and
   dump store. Everything that needs a PS5 - the mounted games, the library,
   the launch services, the dumpers, notifications, the tile - is faked here;
   everything else is the payload's own code, compiled for this machine.

   The pretend console is built to be awkward in the ways a real one is:

     PPSA01234  Astro's Playroom      nothing special
     PPSA04567  Spider-Man            a disc game; its disc goes in 25 s after start
     CUSA07211  Spyro                 a PS4 title
     CUSA00900  Bloodborne            900 TB - can never fit (free-space check)
     PPSA09999  Broken Title          launches, but never mounts (mount timeout)
     PPSA07777  Folder Game           runs from a folder: up, but nothing to mount

   A launch takes 5 s to show up as a mount, a close takes 3 s to let go, and
   launching while the old title is still held answers "still open", as the
   console does. A dump "copies" 600 MB in about eight seconds, leaves a few
   files behind, and takes three seconds to honour a stop. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "app_scan.h"
#include "app_launch.h"
#include "routes.h"
#include "ps4_dumper.h"
#include "ps5_dumper.h"
#include "utils.h"

static const char *g_usb;      /* the one "drive", a folder relative to the cwd */
static const char *g_art;      /* a PNG served as every title's key art         */
static time_t      g_boot;

static struct { const char *id, *title, *ver; int disc; } LIB[] = {
    { "PPSA01234", "Astro's Playroom",          "01.000.000", 0 },
    { "PPSA04567", "Spider-Man: Miles Morales", "01.012.000", 1 },
    { "CUSA07211", "Spyro Reignited Trilogy",   "01.03",      0 },
    { "CUSA00900", "Bloodborne",                "01.09",      0 },
    { "PPSA09999", "Broken Title",              "01.000.000", 0 },
    { "PPSA07777", "Folder Game",               "01.000.000", 0 },
};
#define NLIB ((int)(sizeof(LIB) / sizeof(LIB[0])))

static int lib_index(const char *id)
{
    for (int i = 0; i < NLIB; i++)
        if (id && !strcmp(LIB[i].id, id)) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  The one title the console has up                                   */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static char   g_running[16];
static time_t g_visible_at;    /* when its mount appears        */
static time_t g_gone_at;       /* when a close has taken effect */

/* 1 when a title is mounted, its id in out. */
static int running_now(char *out)
{
    pthread_mutex_lock(&g_mtx);
    time_t now = time(NULL);
    if (g_running[0] && g_gone_at && now >= g_gone_at) { g_running[0] = 0; g_gone_at = 0; }
    int up = g_running[0] && now >= g_visible_at;
    if (up && out) strcpy(out, g_running);
    pthread_mutex_unlock(&g_mtx);
    return up;
}

/* ------------------------------------------------------------------ */
/*  app_scan.h                                                         */
/* ------------------------------------------------------------------ */

int title_on_disc(const char *id)
{
    int i = lib_index(id);
    return i >= 0 && LIB[i].disc && time(NULL) - g_boot > 25;
}

int  title_is_disc_game(const char *id) { int i = lib_index(id); return i >= 0 && LIB[i].disc; }
void title_remember_disc(const char *id) { (void)id; }

static void fill_app(app_entry_t *a, int i)
{
    memset(a, 0, sizeof(*a));
    snprintf(a->dir, sizeof(a->dir), "%s-app0", LIB[i].id);
    strcpy(a->title_id, LIB[i].id);
    strcpy(a->title, LIB[i].title);
    strcpy(a->version, LIB[i].ver);
    a->is_ps4  = !strncmp(LIB[i].id, "CUSA", 4);
    a->on_disc = title_on_disc(LIB[i].id);
    a->is_disc = LIB[i].disc;
}

int app_scan(app_entry_t *out, int max)
{
    char id[16];
    if (max < 1 || !running_now(id)) return 0;
    fill_app(&out[0], lib_index(id));
    return 1;
}

int app_find(const char *dir, app_entry_t *out)
{
    char id[16], want[32];
    if (!dir || !running_now(id)) return -1;
    snprintf(want, sizeof(want), "%s-app0", id);
    if (strcmp(want, dir)) return -1;
    fill_app(out, lib_index(id));
    return 0;
}

int app_icon_path(const app_entry_t *a, char *o, size_t n) { (void)a; if (n) o[0] = 0; return -1; }

uint64_t app_size(const app_entry_t *a)
{
    return !strcmp(a->title_id, "CUSA00900") ? (900ull << 40) : (600ull << 20);
}

static void fill_lib(library_entry_t *e, int i)
{
    char id[16] = {0};
    memset(e, 0, sizeof(*e));
    strcpy(e->title_id, LIB[i].id);
    strcpy(e->title, LIB[i].title);
    strcpy(e->version, LIB[i].ver);
    strcpy(e->source, i == 3 ? "ext0" : "internal");
    e->is_ps4  = !strncmp(LIB[i].id, "CUSA", 4);
    e->on_disc = title_on_disc(LIB[i].id);
    e->is_disc = LIB[i].disc;
    e->has_pic = 1;
    e->is_running = (running_now(id) && !strcmp(id, LIB[i].id)) || title_runs_from_folder(LIB[i].id);
}

int library_scan(library_entry_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < NLIB && n < max; i++) fill_lib(&out[n++], i);
    return n;
}

int library_find(const char *id, library_entry_t *out)
{
    int i = lib_index(id);
    if (i < 0) return -1;
    fill_lib(out, i);
    return 0;
}

int library_pic_path(const char *id, char *o, size_t n) { (void)id; snprintf(o, n, "%s", g_art); return 0; }
int library_icon_path(const char *id, char *o, size_t n) { (void)id; if (n) o[0] = 0; return -1; }

/* up two seconds after its launch, and never mounted */
int title_runs_from_folder(const char *id)
{
    pthread_mutex_lock(&g_mtx);
    int up = id && !strcmp(id, "PPSA07777") && !strcmp(g_running, id) && !g_gone_at &&
             time(NULL) >= g_visible_at - 100000 + 2;
    pthread_mutex_unlock(&g_mtx);
    return up;
}

int target_scan(target_entry_t *out, int max)
{
    if (max < 1) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->mount, sizeof(out->mount), "%s", g_usb);
    strcpy(out->fs, "exfatfs");
    out->writable = 1;
    out->total_bytes = 512ull << 30;
    out->free_bytes  = 300ull << 30;
    if (max < 2) return 1;

    /* the console's own storage: plenty of room, but not for everything */
    memset(&out[1], 0, sizeof(out[1]));
    snprintf(out[1].mount, sizeof(out[1].mount), "%s", storage_internal_root());
    mkdirs(out[1].mount);
    strcpy(out[1].fs, "ufs");
    out[1].writable = 1;
    out[1].internal = 1;
    out[1].total_bytes = 825ull << 30;
    out[1].free_bytes  = 120ull << 30;
    return 2;
}

int target_is_known(const char *m) { return (m && (!strcmp(m, g_usb) || !strcmp(m, storage_internal_root()))) ? 0 : -1; }

/* ------------------------------------------------------------------ */
/*  app_launch.h                                                       */
/* ------------------------------------------------------------------ */

void app_launch_init(void) {}
int  app_launch_available(void) { return 1; }
int  app_launch_probably_available(void) { return 1; }

/* the console counts a title as running from the launch on, mounted or not */
int app_running_id(void)
{
    running_now(NULL);
    pthread_mutex_lock(&g_mtx);
    int up = g_running[0] != 0;
    pthread_mutex_unlock(&g_mtx);
    return up ? 0x1234 : 0;
}

int app_close_running(char *err, size_t n)
{
    (void)err; (void)n;
    pthread_mutex_lock(&g_mtx);
    if (g_running[0] && !g_gone_at) g_gone_at = time(NULL) + 3;
    pthread_mutex_unlock(&g_mtx);
    printf("[console] close requested\n");
    return 0;
}

int app_launch_title(const char *id, int close_running, char *err, size_t n)
{
    if (lib_index(id) < 0) { snprintf(err, n, "the console does not know this title"); return -1; }

    if (app_running_id()) {
        if (!close_running) { snprintf(err, n, "another game is running"); return -1; }
        app_close_running(err, n);   /* the launch below then meets "still open" */
    }

    pthread_mutex_lock(&g_mtx);
    if (g_running[0]) {
        pthread_mutex_unlock(&g_mtx);
        snprintf(err, n, "the console still has this title open (0x8094000c)");
        return -1;
    }
    strcpy(g_running, id);
    g_gone_at = 0;
    g_visible_at = time(NULL) + ((!strcmp(id, "PPSA09999") || !strcmp(id, "PPSA07777")) ? 100000 : 5);
    pthread_mutex_unlock(&g_mtx);

    printf("[console] launching %s\n", id);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  The dumpers                                                        */
/* ------------------------------------------------------------------ */

static int fake_dump(const char *dir, const char *dest)
{
    write_log(g_log_path, "Dumping %s to %s", dir, dest);
    folder_size_current = 600u << 20;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s/sce_sys", dest, dir);
    mkdirs(path);
    snprintf(path, sizeof(path), "%s/%s/eboot.bin", dest, dir);
    FILE *f = fopen(path, "w");
    if (f) { fputs("fake", f); fclose(f); }

    for (int i = 0; i < 40; i++) {
        if (abort_requested()) { sleep(3); return -1; }
        total_bytes_copied += 15u << 20;
        snprintf(current_copied, sizeof(current_copied), "%s/data/chunk_%03d.pak", dir, i);
        usleep(200000);
    }

    write_log(g_log_path, "Dump complete: %s", dir);
    return 0;
}

int dump_ps4_cusa_app(const char *s, const char *dir, const char *patch, const char *dest,
                      int a, int b, int c)
{ (void)s; (void)patch; (void)a; (void)b; (void)c; return fake_dump(dir, dest); }

int dump_ps5_ppsa_app(const char *s, const char *dir, const char *dest, int a, int b, int c)
{ (void)s; (void)a; (void)b; (void)c; return fake_dump(dir, dest); }

/* ------------------------------------------------------------------ */
/*  Notifications and the home-screen tile                             */
/* ------------------------------------------------------------------ */

int sceKernelSendNotificationRequest(int d, SceNotificationRequest *r, size_t n, int b)
{ (void)d; (void)n; (void)b; printf("[notify] %s\n", r->message); return 0; }

/* a tile from an older build is lying around until the user updates it */
static int g_tile_ok = 0;
int tile_exists(void) { return 1; }
int tile_is_current(int port) { (void)port; return g_tile_ok; }
int tile_install(int port, char *err, size_t n) { (void)port; (void)err; (void)n; g_tile_ok = 1; return 0; }

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    g_usb  = argc > 1 ? argv[1] : "usb0";
    g_art  = getenv("HARNESS_ART") ? getenv("HARNESS_ART") : "art.png";
    g_boot = time(NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);

    whb_app_set(dumper_app());
    find_usb_and_setup();   /* no /mnt/usbX here: settings go to the pretend console */
    whb_routes_init();
    routes_settings_init();
    routes_dumper_init();
    return http_server_run(argc > 2 ? atoi(argv[2]) : 8099);
}
