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

/* What every web homebrew serves besides its own business: the page and its
   icons, the home-screen tile, a place in Payload Manager, shutting down. */

#include <stdio.h>
#include <stdint.h>

#include "webhb.h"
#include "utils.h"

/* What this copy is, and whether it carries an ELF it could store. */
static void handle_self(int fd, const params_t *p)
{
    (void)p;
    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"version\":");
    sb_json_str(&sb, whb_app()->version);
    sb_puts(&sb, ",\"file\":");
    sb_json_str(&sb, whb_elf_name());
    sb_printf(&sb, ",\"canStore\":%s}", self_store_available() ? "true" : "false");
    send_sb(fd, 200, &sb);
}

/* Is the file pldmgr holds this very build? The page passes the path it
   got from pldmgr's list; self_store_matches() only accepts a file of our
   name inside a payload manager's storage. */
static void handle_self_compare(int fd, const params_t *p)
{
    int rc = self_store_matches(param_get(p, "path", ""));

    char json[64];
    snprintf(json, sizeof(json), "{\"match\":%s}",
             rc == 1 ? "\"same\"" : rc == 0 ? "\"other\"" : "\"unknown\"");
    send_json(fd, 200, json);
}

static void handle_self_store(int fd, const params_t *p)
{
    (void)p;
    char err[192] = {0};
    if (self_store_to_pldmgr(err, sizeof(err)) != 0) {
        send_error(fd, 409, err[0] ? err : "could not store the payload");
        return;
    }

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"stored\":true,\"file\":");
    sb_json_str(&sb, whb_elf_name());
    sb_puts(&sb, "}");
    send_sb(fd, 200, &sb);
}

/* Whether the home-screen shortcut is in place. Reads two small files and
   nothing else, so the settings tab can ask without waking a system service. */
static void handle_tile_state(int fd, const params_t *p)
{
    (void)p;
    /* "installed" without "current" is a tile from an older build, or one
       that points at a port this server is not on */
    int current = tile_is_current(http_server_port());

    char json[96];
    snprintf(json, sizeof(json), "{\"installed\":%s,\"current\":%s}",
             (current || tile_exists()) ? "true" : "false",
             current ? "true" : "false");
    send_json(fd, 200, json);
}

static void handle_install_tile(int fd, const params_t *p)
{
    (void)p;
    /* Registering a title makes the shell rework its app database. Keep that
       away from work that may be reading the very same titles. */
    if (whb_busy()) {
        send_error(fd, 409, "wait for the dump to finish first");
        return;
    }

    char err[160];
    if (tile_install(http_server_port(), err, sizeof(err)) != 0) {
        send_error(fd, 500, err[0] ? err : "could not install the shortcut (see log.txt)");
        return;
    }
    send_json(fd, 200, "{\"installed\":true}");
}

static void handle_quit(int fd, const params_t *p)
{
    (void)p;
    if (whb_busy()) {
        send_error(fd, 409, "a dump is running");
        return;
    }

    send_json(fd, 200, "{\"stopping\":true}");
    printf_notification("%s: web UI closed", whb_app()->name);
    http_server_stop();
}

/* The logo as PNG, the one picture the payload carries anyway (the tile
   uses it). iOS wants a PNG for "Add to Home Screen" - it ignores an SVG
   favicon - and Android takes it from the web manifest. */
static void handle_app_icon(int fd, const params_t *p)
{
    (void)p;
    send_response_cc(fd, 200, "image/png", whb_app()->icon_png, whb_app()->icon_len,
                     "max-age=86400", NULL);
}

static void handle_web_manifest(int fd, const params_t *p)
{
    (void)p;
    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"name\":");
    sb_json_str(&sb, whb_app()->name);
    sb_puts(&sb, ",\"short_name\":");
    sb_json_str(&sb, whb_app()->short_name);
    sb_puts(&sb,
        ",\"start_url\":\"/\",\"display\":\"standalone\","
        "\"background_color\":\"#07090d\",\"theme_color\":\"#07090d\","
        "\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"512x512\",\"type\":\"image/png\","
        "\"purpose\":\"any maskable\"}]}");

    if (sb.oom) { send_sb(fd, 500, &sb); return; }
    send_response(fd, 200, "application/manifest+json", sb.buf, sb.len, NULL);
    sb_free(&sb);
}

static void handle_index(int fd, const params_t *p)
{
    (void)p;
    /* "no-store" would keep the page out of the browser's application cache,
       which is what lets the home-screen tile open it while the payload is
       not running. The manifest below takes care of freshness. */
    send_response_cc(fd, 200, "text/html; charset=utf-8",
                     whb_app()->page, whb_app()->page_len, "no-cache", NULL);
}

/* The application cache manifest. A browser that knows the mechanism - the
   PS5's does - keeps the page and serves it even when nothing answers on
   this port; the page then says so and offers to start the payload. The
   comment line carries a checksum of the page: any change to it makes the
   browser fetch the new one. Everything else always goes to the network. */
static void handle_manifest(int fd, const params_t *p)
{
    (void)p;
    uint32_t sum = 2166136261u;   /* FNV-1a */
    for (size_t i = 0; i < whb_app()->page_len; i++)
        sum = (sum ^ whb_app()->page[i]) * 16777619u;

    /* tells, in the live console, whether a browser uses the cache at all */
    write_log(g_log_path, "Web UI: cache manifest requested (page %08x)", (unsigned)sum);

    char body[160];
    int n = snprintf(body, sizeof(body),
                     "CACHE MANIFEST\n"
                     "# page %08x-%zu\n"
                     "\n"
                     "CACHE:\n"
                     "/\n"
                     "\n"
                     "NETWORK:\n"
                     "*\n",
                     (unsigned)sum, whb_app()->page_len);

    send_response(fd, 200, "text/cache-manifest", body, (size_t)n, NULL);
}

static void on_listening(int port)
{
    /* Said, not done: installing goes through the app-install service, and
       that only ever happens because the user asked for it. */
    if (tile_exists() && !tile_is_current(port))
        write_log(g_log_path, "Tile: the home-screen shortcut is out of date - "
                              "update it under Menu > Settings");
}

void whb_routes_init(void)
{
    http_on_listening(on_listening);
    whb_access_routes_init();

    http_route("GET",  "/",                     handle_index);
    http_route("GET",  "/index.html",           handle_index);
    http_route("GET",  "/cache.appcache",       handle_manifest);
    http_route("GET",  "/icon.png",             handle_app_icon);
    http_route("GET",  "/apple-touch-icon.png", handle_app_icon);
    http_route("GET",  "/apple-touch-icon-precomposed.png", handle_app_icon);
    http_route("GET",  "/app.webmanifest",      handle_web_manifest);
    http_route("GET",  "/api/self",             handle_self);
    http_route("GET",  "/api/self/compare",     handle_self_compare);
    http_route("POST", "/api/self/store",       handle_self_store);
    http_route("GET",  "/api/tile",             handle_tile_state);
    http_route("POST", "/api/tile",             handle_install_tile);
    http_route("POST", "/api/quit",             handle_quit);
}
