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

#include "ps4_pkg.h"
#include "utils.h"

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

/* ------------------- Read Null-Terminated String ------------------- */

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
        case 0x1000: return "param.sfo";
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
int isfpkg_ps4(const char *pkgfn) {
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
    write_log(g_log_path, "isfpkg: Raw bytes: %02X %02X %02X %02X → magic: 0x%08X",
              header[0], header[1], header[2], header[3], magic);

    if (magic != bswap_32(PS4_PKG_MAGIC)) {
        write_log(g_log_path, "isfpkg: Invalid magic 0x%08X (expected 0x%08X)", magic, bswap_32(PS4_PKG_MAGIC));
        return 2;
    }

    write_log(g_log_path, "isfpkg: Valid PS4 PKG");
    return 0;
}

/* Package names may carry a leading path; the dump wants the bare name
   below sce_sys. */
static void clean_entry_name(char *name)
{
    if (strncmp(name, "/sce_sys/", 9) == 0)      memmove(name, name + 9, strlen(name + 9) + 1);
    else if (strncmp(name, "sce_sys/", 8) == 0)  memmove(name, name + 8, strlen(name + 8) + 1);

    if (strncmp(name, "/mnt/usb0/", 10) == 0)    memmove(name, name + 10, strlen(name + 10) + 1);
    else if (strncmp(name, "mnt/usb0/", 9) == 0) memmove(name, name + 9, strlen(name + 9) + 1);

    if (name[0] == '/') memmove(name, name + 1, strlen(name));
}

/* ------------------- Main Extractor ------------------- */
int unpkg_ps4(const char *pkgfn, const char *tidpath) {
    write_log(g_log_path, "unpkg: Opening %s", pkgfn);
    int fdin = open(pkgfn, O_RDONLY);
    if (fdin == -1) { write_log(g_log_path, "unpkg: open failed (errno: %d)", errno); return 1; }

    struct cnt_pkg_main_header hdr;
    if (read(fdin, &hdr, sizeof(hdr)) != sizeof(hdr)) { close(fdin); return 2; }

    if (hdr.magic != bswap_32(PS4_PKG_MAGIC)) { close(fdin); return 3; }
    write_log(g_log_path, "unpkg: Valid PS4 PKG");

    uint32_t table_offset = bswap_32(hdr.file_table_offset);
    uint16_t n_entries = bswap_16(hdr.table_entries_num);
    write_log(g_log_path, "unpkg: Table: %d entries @ 0x%X", n_entries, table_offset);

    lseek(fdin, table_offset, SEEK_SET);
    struct cnt_pkg_table_entry *entries = calloc(n_entries, sizeof(*entries));
    if (!entries) { close(fdin); return 6; }
    if (read(fdin, entries, sizeof(*entries) * n_entries) != sizeof(*entries) * n_entries) {
        free(entries); close(fdin); return 7;
    }

    for (int i = 0; i < n_entries; i++) {
        entries[i].type              = bswap_32(entries[i].type);
        entries[i].name_table_offset = bswap_32(entries[i].name_table_offset);
        entries[i].offset            = bswap_32(entries[i].offset);
        entries[i].size              = bswap_32(entries[i].size);
    }

    char out_dir[512];
    snprintf(out_dir, sizeof(out_dir), "%s/sce_sys", tidpath);
    mkdirs(out_dir);

    /* The name table is one blob of NUL terminated strings; every entry
       points into it by byte offset. Handing the strings out in sequence
       instead - as this used to - shifts the names by one as soon as a
       single entry is named by its type, so files end up under each
       other's names. */
    char  *name_blob = NULL;
    size_t name_blob_size = 0;

    for (int i = 0; i < n_entries; i++) {
        if (entries[i].type != PS4_PKG_ENTRY_TYPE_NAME_TABLE) continue;
        if (entries[i].size == 0 || entries[i].size > 64 * 1024) break;

        name_blob = malloc(entries[i].size + 1);
        if (!name_blob) break;

        lseek(fdin, entries[i].offset, SEEK_SET);
        if (read(fdin, name_blob, entries[i].size) != (ssize_t)entries[i].size) {
            free(name_blob);
            name_blob = NULL;
            break;
        }

        name_blob[entries[i].size] = '\0';   /* so a damaged table cannot run off */
        name_blob_size = entries[i].size;
        break;
    }

    // === EXTRACT FILES ===
    int extracted = 0;

    for (int i = 0; i < n_entries; i++) {
        uint32_t type = entries[i].type;
        uint32_t off  = entries[i].offset;
        uint32_t sz   = entries[i].size;

        if (sz == 0 || sz > 100*1024*1024) continue;

        /* the table name wins: it is what the package itself says */
        const char *name = NULL;
        if (name_blob && entries[i].name_table_offset < name_blob_size)
            name = name_blob + entries[i].name_table_offset;
        if (!name || !name[0])
            name = get_entry_name_by_type(type);
        if (!name || !name[0]) continue;

        char clean[256];
        strncpy(clean, name, sizeof(clean) - 1);
        clean[sizeof(clean) - 1] = '\0';
        clean_entry_name(clean);
        if (!clean[0]) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", out_dir, clean);

        char *dir = strdup(full);
        char *p = strrchr(dir, '/');
        if (p) { *p = 0; mkdirs(dir); }
        free(dir);

        uint8_t *buf = malloc(sz);
        if (!buf) continue;

        lseek(fdin, off, SEEK_SET);
        if (read(fdin, buf, sz) != sz) { free(buf); continue; }

        int out = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0777);
        if (out != -1) {
            write(out, buf, sz);
            close(out);
            extracted++;
            write_log(g_log_path, "unpkg: Extracted %s (%u bytes)", clean, sz);
        }
        free(buf);
    }

    free(name_blob);
    free(entries);
    close(fdin);

    write_log(g_log_path, "unpkg: SUCCESS - %d files extracted", extracted);
    return 0;
}
