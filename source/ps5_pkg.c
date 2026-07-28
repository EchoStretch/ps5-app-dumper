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
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "ps5_pkg.h"
#include "utils.h"

#define SCAN_BUF_SIZE (4 * 1024 * 1024)            // 4MB Chunk Size
#define FAST_TAIL_SIZE (500ULL * 1024 * 1024)      // 500MB Fallback Scan Window

/* ------------------- Endian Swap ------------------- */
static inline uint16_t bswap_16(uint16_t v) {
    return ((v & 0x00FFU) << 8) | ((v & 0xFF00U) >> 8);
}

static inline uint32_t bswap_32(uint32_t v) {
    return ((v & 0x000000FFUL) << 24) |
           ((v & 0x0000FF00UL) <<  8) |
           ((v & 0x00FF0000UL) >>  8) |
           ((v & 0xFF000000UL) >> 24);
}

/* ------------------- Custom Memory Search ------------------- */
static void *custom_memmem(const void *haystack, size_t haystacklen,
                           const void *needle, size_t needlelen)
{
    if (needlelen == 0 || haystacklen < needlelen) return NULL;

    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;
    size_t max_idx = haystacklen - needlelen;

    for (size_t i = 0; i <= max_idx; i++) {
        if (h[i] == n[0] && memcmp(&h[i], n, needlelen) == 0) {
            return (void *)&h[i];
        }
    }
    return NULL;
}

/* ------------------- Read Null-Terminated String ------------------- */
static char* read_string(int fd) {
    char buf[256];
    int i = 0;
    char c;
    while (read(fd, &c, 1) == 1 && c != '\0' && i < 255)
        buf[i++] = c;
    buf[i] = '\0';
    return strdup(buf);
}

/* ------------------- PARSE APP.JSON FOR APP_SC.PKG OFFSET & SIZE ------------------- */
static int parse_app_json(const char *json_path, uint64_t *out_offset, uint64_t *out_size)
{
    int fd = open(json_path, O_RDONLY);
    if (fd < 0) return -1;

    char buf[8192] = {0};
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return -1;

    /* Find "app_sc.pkg" piece in pieces array */
    char *sc_entry = strstr(buf, "app_sc.pkg");
    if (!sc_entry) return -1;

    /* Backtrack to beginning of JSON object block for app_sc.pkg */
    char *piece_start = sc_entry;
    while (piece_start > buf && *piece_start != '{') {
        piece_start--;
    }

    char *offset_key = strstr(piece_start, "\"fileOffset\":");
    char *size_key   = strstr(piece_start, "\"fileSize\":");

    if (offset_key && size_key) {
        *out_offset = strtoull(offset_key + strlen("\"fileOffset\":"), NULL, 10);
        *out_size   = strtoull(size_key + strlen("\"fileSize\":"), NULL, 10);
        return 0;
    }

    return -1;
}

/* ------------------- FALLBACK TAIL SCAN FOR CNT HEADER ------------------- */
static uint64_t find_cnt_start_fallback(int fd)
{
    static const uint8_t magic[] = { 0x7F, 0x43, 0x4E, 0x54, 0x83 };
    const size_t magic_len = sizeof(magic);

    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0) return UINT64_MAX;

    uint64_t file_offset = 0;
    if ((uint64_t)file_size > FAST_TAIL_SIZE) {
        file_offset = (uint64_t)file_size - FAST_TAIL_SIZE;
    }

    uint8_t *buf = malloc(SCAN_BUF_SIZE);
    if (!buf) return UINT64_MAX;

    size_t overlap = magic_len - 1;

    while (file_offset < (uint64_t)file_size)
    {
        if (lseek(fd, file_offset, SEEK_SET) < 0) break;

        ssize_t bytes_read = read(fd, buf, SCAN_BUF_SIZE);
        if (bytes_read <= 0) break;

        uint8_t *found = custom_memmem(buf, bytes_read, magic, magic_len);
        if (found)
        {
            uint64_t cnt_pos = file_offset + (found - buf);
            free(buf);
            return cnt_pos;
        }

        if (bytes_read > (ssize_t)overlap) {
            file_offset += (bytes_read - overlap);
        } else {
            file_offset += bytes_read;
        }
    }

    free(buf);
    return UINT64_MAX;
}

/* ------------------- Fallback Name Mapping ------------------- */
static char *get_entry_name_by_type(uint32_t type) {
    switch (type) {
        case 0x0400: return "license.dat";
        case 0x0401: return "license.info";
        case 0x0402: return "nptitle.dat";
        case 0x0403: return "npbind.dat";
        case 0x0404: return "selfinfo.dat";
        case 0x0406: return "imageinfo.dat";
        case 0x0407: return "target-deltainfo.dat";
        case 0x0408: return "origin-deltainfo.dat";
        case 0x0409: return "psreserved.dat";
        case 0x1000: return "param.json";
        case 0x1001: return "playgo-chunk.dat";
        case 0x1002: return "playgo-chunk.sha";
        case 0x1003: return "playgo-manifest.xml";
        case 0x1004: return "pronunciation.xml";
        case 0x1005: return "pronunciation.sig";
        case 0x1006: return "pic1.png";
        case 0x1007: return "pubtoolinfo.dat";
        case 0x1200: return "icon0.png";
        case 0x1220: return "pic0.png";
        case 0x1240: return "snd0.at9";
        case 0x1260: return "changeinfo/changeinfo.xml";
        case 0x1280: return "icon0.dds";
        case 0x12A0: return "pic0.dds";
        case 0x12C0: return "pic1.dds";
        default: return NULL;
    }
}

/* ------------------- PKG Validation ------------------- */
int isfpkg_ps5(const char *pkgfn) {
    write_log(g_log_path, "isfpkg: Checking %s", pkgfn);

    int fd = open(pkgfn, O_RDONLY);
    if (fd == -1) {
        write_log(g_log_path, "isfpkg: open failed (errno: %d)", errno);
        return 1;
    }

    uint8_t header[4];
    if (read(fd, header, 4) != 4) {
        write_log(g_log_path, "isfpkg: read failed (errno: %d)", errno);
        close(fd);
        return 2;
    }
    close(fd);

    uint32_t magic = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);

    if (magic == PS5_PKG_MAGIC || magic == 0x544E437F) {
        write_log(g_log_path, "isfpkg: Valid PKG Header");
        return 0;
    }

    write_log(g_log_path, "isfpkg: Invalid magic 0x%08X", magic);
    return 2;
}

/* ------------------- MAIN UNPKG (DIRECT IN-MEMORY READ) ------------------- */
int unpkg_ps5(const char *pkgfn, const char *tidpath)
{
    write_log(g_log_path, "unpkg: Processing %s", pkgfn);

    uint64_t cnt_offset = UINT64_MAX;
    uint64_t cnt_size = 0;

    /* 1. Get path to app.json */
    char json_path[512] = {0};
    char pkg_dir[512] = {0};
    strncpy(pkg_dir, pkgfn, sizeof(pkg_dir) - 1);

    char *last_slash = strrchr(pkg_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        snprintf(json_path, sizeof(json_path), "%s/app.json", pkg_dir);
    }

    /* 2. Check app.json for offset */
    if (json_path[0] != '\0' && parse_app_json(json_path, &cnt_offset, &cnt_size) == 0)
    {
        write_log(g_log_path, "Parsed %s -> app_sc.pkg offset: 0x%llX",
                  json_path, (unsigned long long)cnt_offset);
    }
    else
    {
        /* Fallback: Scan file */
        int fdin_scan = open(pkgfn, O_RDONLY);
        if (fdin_scan >= 0) {
            cnt_offset = find_cnt_start_fallback(fdin_scan);
            close(fdin_scan);
        }
    }

    if (cnt_offset == UINT64_MAX)
    {
        write_log(g_log_path, "unpkg: Could not determine CNT offset");
        return 2;
    }

    /* 3. Open source package directly */
    int fdin = open(pkgfn, O_RDONLY);
    if (fdin < 0)
    {
        write_log(g_log_path, "Failed to open package %s", pkgfn);
        return 3;
    }

    /* Seek directly to CNT header inside app.pkg */
    if (lseek(fdin, cnt_offset, SEEK_SET) < 0)
    {
        write_log(g_log_path, "Failed seeking to CNT offset 0x%llX", (unsigned long long)cnt_offset);
        close(fdin);
        return 4;
    }

    struct cnt_pkg_main_header hdr;
    if (read(fdin, &hdr, sizeof(hdr)) != sizeof(hdr))
    {
        write_log(g_log_path, "Failed reading CNT header");
        close(fdin);
        return 5;
    }

    /* Check magic */
    uint32_t magic = hdr.magic;
    if (magic == 0x544E437F || bswap_32(hdr.magic) == 0x544E437F)
    {
        write_log(g_log_path, "Valid CNT magic detected at offset 0x%llX", (unsigned long long)cnt_offset);
    }
    else
    {
        write_log(g_log_path, "Invalid CNT magic at offset 0x%llX: 0x%08X", (unsigned long long)cnt_offset, magic);
        close(fdin);
        return 6;
    }

    uint32_t table_offset = bswap_32(hdr.file_table_offset);
    uint16_t n_entries    = bswap_16(hdr.table_entries_num);

    write_log(g_log_path, "CNT: %d entries, table at offset + 0x%X", n_entries, table_offset);

    /* Seek to Table offset relative to cnt_offset */
    lseek(fdin, cnt_offset + table_offset, SEEK_SET);

    struct cnt_pkg_table_entry *entries = calloc(n_entries, sizeof(*entries));
    if (!entries) {
        close(fdin);
        return 7;
    }

    if (read(fdin, entries, sizeof(*entries) * n_entries) != sizeof(*entries) * n_entries)
    {
        write_log(g_log_path, "Failed to read file table");
        free(entries);
        close(fdin);
        return 8;
    }

    /* Byte-swap entries */
    for (int i = 0; i < n_entries; i++)
    {
        entries[i].type   = bswap_32(entries[i].type);
        entries[i].offset = bswap_32(entries[i].offset);
        entries[i].size   = bswap_32(entries[i].size);
    }

    /* Target Directory */
    char out_dir[512];
    snprintf(out_dir, sizeof(out_dir), "%s/sce_sys", tidpath);
    mkdirs(out_dir);

    /* === NAME TABLE === */
    char *name_table[256] = {0};
    int name_idx = 0;

    for (int i = 0; i < n_entries; i++)
    {
        if (entries[i].type == 0x0200)   // name table type
        {
            lseek(fdin, cnt_offset + entries[i].offset + 1, SEEK_SET);

            while (name_idx < 256)
            {
                name_table[name_idx] = read_string(fdin);
                if (!name_table[name_idx] || name_table[name_idx][0] == '\0')
                {
                    free(name_table[name_idx]);
                    name_table[name_idx] = NULL;
                    break;
                }
                name_idx++;
            }
            break;
        }
    }

    /* === EXTRACT FILES DIRECTLY FROM APP.PKG INTO SCE_SYS === */
    int extracted = 0;
    int name_count = 0;

    for (int i = 0; i < n_entries; i++)
    {
        uint32_t type = entries[i].type;
        uint32_t off  = entries[i].offset;
        uint32_t sz   = entries[i].size;

        if (sz == 0) continue;

        char *name = get_entry_name_by_type(type);
        if (!name && name_count < name_idx)
            name = name_table[name_count++];

        if (!name || !name[0]) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", out_dir, name);

        /* Create subdirs if needed */
        char *dir = strdup(full);
        char *p = strrchr(dir, '/');
        if (p) {
            *p = '\0';
            mkdirs(dir);
        }
        free(dir);

        uint8_t *buf_file = malloc(sz);
        if (!buf_file) continue;

        /* Seek to relative offset inside app.pkg */
        lseek(fdin, cnt_offset + off, SEEK_SET);

        if (read(fdin, buf_file, sz) != sz)
        {
            free(buf_file);
            continue;
        }

        int outfd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        if (outfd >= 0)
        {
            write(outfd, buf_file, sz);
            close(outfd);
            extracted++;
            write_log(g_log_path, "Extracted directly: %s (%u bytes)", name, sz);
        }

        free(buf_file);
    }

    /* Cleanup */
    for (int i = 0; i < name_idx; i++)
        if (name_table[i]) free(name_table[i]);

    free(entries);
    close(fdin);

    write_log(g_log_path, "unpkg: SUCCESS - %d files extracted directly without temporary carving", extracted);
    return 0;
}