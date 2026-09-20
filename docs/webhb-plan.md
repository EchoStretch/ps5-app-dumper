# webhb - carving the web-homebrew core out of the dumper

Status: proposal, nothing moved yet. Working name `webhb`; everything stays in
this repository until a second payload has proven the interface.

## Why

Most of what was built around the web UI has nothing to do with dumping. A
count of `source/http_server.c` (1566 lines) by what each function serves:

| kind | lines | what |
| --- | --- | --- |
| infrastructure | ~680 | sockets, request parsing, responses, string builder, page / manifest / icon routes, log feed |
| platform | ~250 | config, tile, self-store, folder browser, quit, status |
| dumper | ~560 | devices, library, launch, dump, queue, unfinished dumps |

`single_instance.c`, `self_store.c`, `app_installer.c` and `fs_browse.c` depend
on nothing but `utils.h` already. The knowledge that cost the most - the PS5
browser's CSS limits, the tile's HTTPS stack, pldmgr's conventions, the host
harness - is not tied to the dumper at all.

## Target layout

```
webhb/
  include/webhb.h        one public header
  http.c                 listen/accept/threads, request parsing, responses, sb_*, params
  routes.c               built-in routes (see below) + the route table
  log.c                  log ring, write_log, general and per-job log files
  notify.c               printf_notification(_quiet)
  storage.c              drive detection, <drive>/<app>/ folder, legacy migration
  config.c               key=value store with registered keys (type, range, default)
  instance.c             process name, single-instance takeover   (was single_instance.c)
  selfstore.c            embedded ELF -> pldmgr, same-build check  (was self_store.c)
  tile.c                 home-screen tile install + currency check (was app_installer.c)
  fsbrowse.c             folder picker backend                     (was fs_browse.c)
  web/whb.css, whb.js    client kit, inlined into the app's page at build time
  harness/               host build: PS5-only calls mocked, `make sim`
  webhb.mk               bin2c, build stamp, two-stage build
source/                  the dumper: app_scan, app_launch, dump_*, pfs, pkg, decrypt, backport
  routes_dumper.c        the ~560 lines of dumper routes
  main.c                 fills in whb_app_t, registers routes, calls whb_run()
```

## The interface

```c
typedef struct {
    const char *name;            /* "PS5 App Dumper"                         */
    const char *version;         /* "1.12"                                   */
    const char *process_name;    /* "ps5-app-dumper.elf" - must end in .elf  */
    const char *data_dirname;    /* "ps5-app-dumper" -> <drive>/ps5-app-dumper */
    const char *elf_basename;    /* "ps5-app-dumper" -> ..._v1.12.elf        */
    const char *tile_title_id;   /* "APDU00001", NULL for no tile            */
    const char *tile_title;      /* "App Dumper"                             */
    int         default_port;    /* 8081                                     */
    const unsigned char *page;      size_t page_len;   /* bin2c'd index.html */
    const unsigned char *icon_png;  size_t icon_len;   /* 512x512            */
    int (*busy)(void);           /* 1 while work must not be interrupted     */
} whb_app_t;

typedef struct whb_req whb_req_t;           /* method, path, params, socket */
typedef void (*whb_handler_t)(whb_req_t *req);

void        whb_route(const char *method, const char *path, whb_handler_t fn);
const char *whb_param(const whb_req_t *req, const char *key, const char *fallback);
int         whb_param_int(const whb_req_t *req, const char *key, int fallback);
void        whb_send_json(whb_req_t *req, int code, const char *json);
void        whb_send_error(whb_req_t *req, int code, const char *message);
void        whb_send_file(whb_req_t *req, const char *path, const char *content_type);
/* sb_* string builder and whb_send_sb() as today */

int         whb_run(const whb_app_t *app);  /* claims the name, takes over an idle
                                               copy, finds the drive, serves */
```

`busy()` is the one callback the core needs: single-instance takeover, `quit`
and the cached page's reload all ask it instead of knowing about dumps.

Built-in routes, served by the core for every app:
`/`, `/cache.appcache`, `/icon.png` (+ apple-touch names), `/app.webmanifest`,
`/api/whb/status` (log feed, busy), `/api/whb/self`, `/api/whb/self/store`,
`/api/whb/self/compare`, `/api/whb/tile`, `/api/whb/config`, `/api/whb/browse`,
`/api/whb/mkdir`, `/api/whb/quit`. The dumper keeps `/api/devices`,
`/api/library`, `/api/launch`, `/api/dump`, `/api/queue/*`, `/api/dumps/*`.

Client kit (`whb.js` / `whb.css`): `api`, `post`, `toast`, live console, cache
diagnostics, offline banner + pldmgr launcher, tile and self-store rows, the
flex-gap probe with its fallback block, design tokens, switch / segmented /
picker components. The page stays a single embedded file: the build inlines
the kit at `/* @WHB_CSS@ */` and `/* @WHB_JS@ */`.

## Order of work

Each step builds, passes the harness in a browser, and changes no behaviour.

1. **Split `http_server.c`** into `webhb/http.c`, `webhb/routes.c` and
   `source/routes_dumper.c` behind a route table. Pure move.
2. **Introduce `whb_app_t`** and move `single_instance`, `self_store`,
   `app_installer`, `fs_browse` over, replacing the hard-coded names
   (`PAYLOAD_PROCESS_NAME`, `DUMPER_ELF_NAME`, `TILE_TITLE_ID`, port 8081).
3. **Bring the harness into the repo** (`webhb/harness`, `make sim`). Today it
   lives in a session scratchpad and is rebuilt from memory notes.
4. **Split `utils.c`**: log, notify, storage and config leave; the copy
   routines and abort flag stay with the dumper.
5. **Extract the client kit** from `web/index.html`.
6. **Generalise the config store** (registered keys instead of one struct).
7. **Prove it with a second payload** - small and wanted anyway, e.g. a klog
   viewer. If that takes an evening, the interface is right.

Steps 1-3 already pay off for the dumper alone.

## Decide before anything is published

- **Name and route prefix** (`webhb`, `/api/whb/...`).
- **Copyright headers.** The files written in this fork carry the repo's
  "Copyright (C) 2025 EchoStretch" header for consistency, which is not
  accurate for code that did not come from there. GPLv3 applies either way.
- **Access control.** Anyone on the LAN can launch titles, delete unfinished
  dumps and stop the payload. Fine for one tool at home; a framework needs at
  least a token or an origin check.
- **Firmware scope.** Tile install, launch services and the process list are
  proven on FW 12.00 only.
- **iOS.** No offline start there: service workers need HTTPS, AppCache is
  gone. The kit should say so instead of pretending.
