# webhb - carving the web-homebrew core out of the dumper

Status: steps 1 to 6 are done (server split, `whb_app_t` and the four modules moved, harness in the repo, log / notifications / storage out of `utils.c`, the client kit, the config store, `whb_start()` / `whb_serve()`); `webhb/` no longer includes a single header of the dumper, and no route that is not about dumping is left outside it. The second payload (step 7) exists as well: `examples/klogview/`, a kernel log viewer, running on the console next to the dumper. What is left are the loose ends listed with the steps and the decisions at the end. Working name `webhb`; everything stays in
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
  storage.c              the data folder <root>/homebrew/<app>, legacy migration
  drives.c               target_scan(): the drives and the console's own storage
  config.c               key=value store with registered keys (type, range, default)
  start.c                whb_start(): name, hello, settings, takeover
  instance.c             process name, single-instance takeover   (was single_instance.c)
  selfstore.c            embedded ELF -> pldmgr, same-build check  (was self_store.c)
  tile.c                 home-screen tile install + currency check (was app_installer.c)
  fsbrowse.c             folder picker backend                     (was fs_browse.c)
  web/whb.css, whb.js    client kit, inlined into the app's page at build time (tools/inline.sh)
  harness/               host build: PS5-only calls mocked (today: /harness)
  webhb.mk               bin2c, build stamp, two-stage build
source/                  the dumper: app_scan, app_launch, dump_*, pfs, pkg, decrypt, backport
  routes_dumper.c        the ~560 lines of dumper routes
  dumper_config.c        the dumper's keys for the config store, dumper_config_t as a copy
  main.c                 whb_start(), headless / auto_start, routes, whb_serve()
```

## The interface

```c
typedef struct {
    const char *name;            /* "PS5 App Dumper"                         */
    const char *short_name;      /* "App Dumper" - tile and phone home screen */
    const char *version;         /* "1.12"                                   */
    const char *process_name;    /* "ps5-app-dumper.elf" - must end in .elf  */
    const char *data_dirname;    /* "ps5-app-dumper" -> <drive>/ps5-app-dumper */
    const char *elf_basename;    /* "ps5-app-dumper" -> ..._v1.12.elf        */
    const char *tile_title_id;   /* "APDU00001", NULL for no tile            */
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

As it stands after step 2, `webhb/include/webhb.h` has the struct (with
`short_name` where this sketch first had `tile_title`: the tile and a phone's
home screen want the same thing, a name that fits under an icon), plus
`whb_app_set()`, `whb_app()`, `whb_busy()`, `whb_elf_name()` and
`whb_routes_init()`. `whb_run()` became two calls, because headless mode
branches off in between: `whb_start(app)` (name, hello, settings, takeover of
an idle copy; 1 when a busy copy stays) and `whb_serve(port)` (built-in
routes, then the server; port 0 takes the one from the settings). The struct
gained `on_start` - what must be set up before anything talks to the system
services - and `busy_with` ("a dump") for messages. The request type is in as sketched: handlers are `void fn(whb_req_t *req)`,
registered with `whb_route()`, and see the request only through `whb_param()`,
`whb_param_int()`, `whb_peer_is_local()` and the `whb_send*()` family - the
socket and the parameter table are `http.c`'s own.

`busy()` is the one callback the core needs: single-instance takeover, `quit`
and the cached page's reload all ask it instead of knowing about dumps.

Built-in routes, served by the core for every app (as built - see the prefix decision at the end):
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

1. **Split `http_server.c`** - done. `webhb/http.c` + `webhb/include/webhb_http.h`
   hold the server and the route table (`http_route()`), `source/routes_platform.c`
   the ~250 platform lines, `source/routes_dumper.c` the dumper's. The platform
   routes stay in `source/` until step 2 gives them `whb_app_t` to stand on.
   Verified by diffing the answers of 40 requests before and after: identical.
2. **Introduce `whb_app_t`** - done. `source/dumper_app.c` describes the dumper;
   `instance.c`, `selfstore.c`, `tile.c`, `fsbrowse.c` and the routes that need
   nothing else (`webhb/routes.c`: page, cache manifest, icons, web manifest,
   `/api/self*`, `/api/tile`, `/api/quit`) live in `webhb/`. The macros
   `PAYLOAD_PROCESS_NAME`, `DUMPER_ELF_NAME`, `TILE_TITLE_ID`, `APP_DATA_DIRNAME`
   and the literal port and app name are gone. Left with the dumper as
   `source/routes_settings.c`: `/api/config` (stands on `dumper_config_t`, step 6)
   and `/api/browse`, `/api/mkdir` (ask `target_is_known()` in `app_scan.c`,
   step 4). Verified by the snapshot, an SDK build and a byte comparison of the
   tile's param.json, which decides whether an installed tile counts as current.

   Loose ends, deliberately not touched because they would change behaviour:
   - `instance.c` still recognises a copy by the dumper's `/api/status` answer
     (`"job":`). Two webhb apps on neighbouring ports would need a status that
     names the app - that is `/api/whb/status`.
   - The core's 409 texts still say "a dump is running".
   - Routes keep their `/api/...` paths; the `/api/whb/` prefix is a change to
     the page and waits for the name decision below.
3. **Bring the harness into the repo** - done, as `harness/` (`make -C harness
   sim|run|pldmgr|snapshot|render`). It stays outside `webhb/` for now because
   its mock fakes the dumper's console. It builds `webhb/` without `tile.c`
   and `instance.c`, which only make sense on the console.
4. **Split `utils.c`** - done for log, notifications and storage: `webhb/log.c`
   (ring, `write_log`, general and per-job files), `webhb/notify.c`,
   `webhb/storage.c` (the data folder `<root>/homebrew/<data_dirname>` on a drive
   or on the console, "a drive with a config.ini wins", migration of the older
   layouts, `dir_exists` / `file_exists` / `mkdirs`). Their declarations moved
   into `webhb.h`; `utils.h` includes it, so the dumper's sources did not change.
   The core's files include `webhb.h` and nothing else; a link of them alone
   leaves only `sceKernelSendNotificationRequest`, the embedded `self_elf` and
   `tile_*` (not built on the host) undefined. Snapshot identical (52 requests).
   **Config stays** in `utils.c` with `dumper_config_t` until step 6, and with it
   `find_usb_and_setup()` (creates the default config.ini) and
   `source/routes_settings.c`. The copy routines and the abort flag stay with
   the dumper for good.
5. **Extract the client kit** from `web/index.html` - done. `webhb/web/whb.js`
   and `whb.css`, put back into the page by `tools/inline.sh` where it says
   `/* @WHB_JS@ */` and `/* @WHB_CSS@ */` (both Makefiles; the build stamp goes
   in afterwards). The script is text inside the page's own function, not a
   module: the app fills in `whb.app` (its names, its views, the hooks
   `onStatus`, `onOnline`, `onView`, `onConfig`) and calls `whbBoot()`. The kit
   owns `api` / `post` and the token, toasts, the live console, online / offline
   with the payload-manager launcher, the status poll (`whbPoll()`, which keeps
   `whb.busy` and `whb.online`), the cache's paper trail, menu and views, the
   access dialog, and the tile, self-store, copy-settings and shut-down rows.
   `whb.css` has the tokens, header, cards, rows, switches, segmented controls,
   inputs, buttons, banners, the modal, console, action bar, toasts and their
   flex-gap fallback lines; the app's rules follow it.

   Not a byte-identical move - the served page changed - so it was verified in
   a browser: `harness/stylecheck.js` (`make -C harness styles`) compares the
   computed style of every element across all views and dialogs, and a drive
   through the kit's controls read the same before and after.

   Loose ends: the markup the kit works on (it expects some forty ids, listed
   at the top of `whb.js`) and the flex-gap probe in the page's head are still
   the page's - a second app copies them. The folder picker's script stayed
   with the dumper because it also moves dumps; its styles are in the kit. The
   fallback lines carry `:not(.drive-free)` in the kit too: taking it out would
   change their specificity.
6. **Generalise the config store** - done. `webhb/config.c` keeps the values
   of keys an app registers (`whb_cfg_key_t`: ini name, web name, type, range,
   default, the comment for config.ini, and the flags `WHB_CFG_SECRET` - never
   served, never taken from a request -, `WHB_CFG_LOCAL` - only 127.0.0.1 may
   change it - and `WHB_CFG_SUBDIR` - a folder below a mount point). Loading,
   saving, "changed while no drive was there", `/api/config` and
   `/api/config/console` follow from the table. The keys the core acts on
   itself (`enable_logging`, `web_port`, `require_code` / `access_code` /
   `access_token`) are macros (`WHB_CFG_STD_*`) the app places in its table,
   because the table's order is the order of `/api/config` and that answer had
   to stay byte-identical; an app that leaves them out logs, serves on its
   `default_port` and gets a new code with every start. The dumper's table is
   `source/dumper_config.c`; `dumper_config_t` stayed, as a copy taken with
   `cfg_snapshot()`, so job, queue and the dumpers did not change.
   `target_scan()` / `target_is_known()` moved to `webhb/drives.c`, which let
   `/api/browse` and `/api/mkdir` join the built-in routes;
   `source/routes_settings.c` and `find_usb_and_setup()` are gone. config.ini
   is written in the table's order now - its text changed, its meaning did not.
   Verified by the snapshot (61 requests, identical), a side by side run of
   old and new on clamps, folder checks, hand-edited files and a restart, and
   a link of the core alone: `sceKernelSendNotificationRequest`, `self_elf`
   and what `tile.c` / `drives.c` define are all it lacks on the host.

   Loose ends: an empty folder setting is still written as `homebrew`
   (`if_empty`), as before, so "the root of the drive" does not survive a
   restart. The legacy `read_*_config()` readers in `utils.c` are dead code
   from upstream and were left alone.
7. **Prove it with a second payload** - done: `examples/klogview/`, the kernel
   log in a browser (live view, filter, marks, optional `logs/klog.txt`).
   `main.c` has about 200 lines, the page sixty lines of script; it took an
   evening's fraction. What it needed from the core and did not find went in:
   `webhb/webhb.mk` (inlining, stamp, icon, two-stage build, `make sim`),
   `webhb/host/stubs.c`, a `/api/status` of the core's own, telling a running
   copy by the file name in `/api/self` instead of by the dumper's status (the
   viewer would have asked the dumper to quit), a kit that leaves alone what a
   page does not have, `whb.app.onLog`, and two names that were the dumper's
   (`log_basename`, `lockedFrom`). On the console (FW 12.00): kernel lines
   arrive, a resent copy replaces only its own predecessor, the dumper on
   8081 is not touched.

   Still the app's to copy: the markup the kit works on (header, offline
   banner, unlock dialog, toast, the settings rows) and the flex-gap probe in
   the head. The dumper's own Makefile and harness do not use `webhb.mk` /
   `host/stubs.c` yet - they predate them and work.

Steps 1-3 already pay off for the dumper alone.

## Decide before anything is published

- **Name and route prefix** - decided 2026-09-20: the name stays `webhb`, and
  the core's routes live below `/api/whb/` (`WHB_API` in `webhb.h`, `WHB_API`
  in `whb.js`): `status`, `self`, `self/store`, `self/compare`, `tile`,
  `config`, `config/console`, `browse`, `mkdir`, `quit`, `access`,
  `access/show`, `unlock`. Everything else below `/api/` is the app's. An app
  with a status route of its own that carries `log` and `busy` points the page
  there (`whb.app.statusPath`, the dumper's `/api/status`) so that one question
  a second is enough; the next copy of a payload always asks `/api/whb/status`.
  `instance.c` still asks the unprefixed paths second, for copies built before
  the move. The page, its cache manifest, the icons and the web manifest keep
  their paths - browsers and the tile know them.
- **Copyright headers** - decided 2026-09-20: the files written in this fork
  name both, "Copyright (C) 2025 EchoStretch" and "Copyright (C) 2026 slopmaster33"
  (`webhb/`, the job / queue / store / library / scan / launch modules and
  their headers). Upstream's files that were only changed here keep their
  header as it is. GPLv3 applies either way.
- **Access control** - built, `webhb/access.c`. Reads are open; a POST needs a
  token unless it comes from 127.0.0.1 (the console's browser, and the next
  copy asking this one to quit). The start toast shows a six-digit code,
  `POST /api/unlock` trades it for the token (five wrong codes: a minute's
  hold), `POST /api/access/show` puts the code on the TV again, `GET
  /api/access` tells a browser where it stands. The app stores code and token
  wherever it keeps its settings and hands them to `whb_access_init()`; an app
  that stores nothing gets a new code with every start. Not covered: reads
  (titles, log, folder names on the drives are visible to the LAN), and plain
  HTTP - whoever can sniff the network can take the token.
- **Firmware scope** - tested by the user on FW 5.10 and 12.00 (tile install,
  launch services, the process list, the takeover of a running copy). The
  README says so; other firmwares are untested, not known to fail.
- **iOS.** No offline start there: service workers need HTTPS, AppCache is
  gone. The kit should say so instead of pretending.
