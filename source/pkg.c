#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "pkg.h"
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
static char* read_string(int fd) {
    char buf[256];
    int i = 0;
    char c;
    while (read(fd, &c, 1) == 1 && c != '\0' && i < 255)
        buf[i++] = c;
    buf[i] = '\0';
    return strdup(buf);
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
int isfpkg(const char *pkgfn) {
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

    if (magic != PS4_PKG_MAGIC) {
        write_log(g_log_path, "isfpkg: Invalid magic 0x%08X (expected 0x544E437F)", magic);
        return 2;
    }

    write_log(g_log_path, "isfpkg: Valid PS4 PKG");
    return 0;
}

/* ------------------- Main Extractor ------------------- */
int unpkg(const char *pkgfn, const char *tidpath) {
    write_log(g_log_path, "unpkg: Opening %s", pkgfn);
    int fdin = open(pkgfn, O_RDONLY);
    if (fdin == -1) { write_log(g_log_path, "unpkg: open failed (errno: %d)", errno); return 1; }

    struct cnt_pkg_main_header hdr;
    if (read(fdin, &hdr, sizeof(hdr)) != sizeof(hdr)) { close(fdin); return 2; }

    if (hdr.magic != PS4_PKG_MAGIC) { close(fdin); return 3; }
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
        entries[i].type   = bswap_32(entries[i].type);
        entries[i].offset = bswap_32(entries[i].offset);
        entries[i].size   = bswap_32(entries[i].size);
    }

    char out_dir[512];
    snprintf(out_dir, sizeof(out_dir), "%s/sce_sys", tidpath);
    mkdirs(out_dir);

    // === COLLECT NAME TABLE ===
    char *name_table[256] = {0};
    int name_idx = 0;
    for (int i = 0; i < n_entries; i++) {
        if (entries[i].type == PS4_PKG_ENTRY_TYPE_NAME_TABLE) {
            off_t pos = entries[i].offset + 1;
            lseek(fdin, pos, SEEK_SET);
            while (name_idx < 256) {
                name_table[name_idx] = read_string(fdin);
                if (!name_table[name_idx][0]) { free(name_table[name_idx]); break; }
                name_idx++;
            }
            break;
        }
    }

    // === CLEAN NAME TABLE PATHS ===
    for (int i = 0; i < name_idx; i++) {
        char *name = name_table[i];

        if (strncmp(name, "/sce_sys/", 9) == 0) memmove(name, name + 9, strlen(name + 9) + 1);
        else if (strncmp(name, "sce_sys/", 8) == 0) memmove(name, name + 8, strlen(name + 8) + 1);

        if (strncmp(name, "/mnt/usb0/", 10) == 0) memmove(name, name + 10, strlen(name + 10) + 1);
        else if (strncmp(name, "mnt/usb0/", 9) == 0) memmove(name, name + 9, strlen(name + 9) + 1);

        if (name[0] == '/') memmove(name, name + 1, strlen(name));
        write_log(g_log_path, "unpkg: cleaned name[%d] = %s", i, name);
    }

    // === EXTRACT FILES ===
    int extracted = 0;
    int name_count = 0;

    for (int i = 0; i < n_entries; i++) {
        uint32_t type = entries[i].type;
        uint32_t off  = entries[i].offset;
        uint32_t sz   = entries[i].size;

        if (sz == 0 || sz > 100*1024*1024) continue;

        char *name = get_entry_name_by_type(type);
        if (!name && name_count < name_idx) name = name_table[name_count++];

        if (!name || !name[0]) continue;

        char full[512];
        snprintf(full, sizeof(full), "%s/%s", out_dir, name);

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
            write_log(g_log_path, "unpkg: Extracted %s (%u bytes)", name, sz);
        }
        free(buf);
    }

    for (int i = 0; i < name_idx; i++) free(name_table[i]);
    free(entries);
    close(fdin);

    write_log(g_log_path, "unpkg: SUCCESS - %d files extracted", extracted);
    return 0;
}
