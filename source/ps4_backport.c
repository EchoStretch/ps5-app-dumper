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
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <ctype.h>

#include "utils.h"

/* --------------------------------------------------------------------- */
/*  ELF Constants                                                        */
/* --------------------------------------------------------------------- */

#define ELF_MAGIC           "\x7F""ELF"
#define PS4_FSELF_MAGIC     "\x4F\x15\x3D\x1D"
#define SFO_MAGIC           "\x00PSF"

#define PT_SCE_PROCPARAM    0x61000001U
#define PT_SCE_MODULE_PARAM 0x61000002U

#define SCE_PROCESS_PARAM_MAGIC 0x4942524F  // "ORBI"
#define SCE_MODULE_PARAM_MAGIC  0x3C13F4BF

/* --------------------------------------------------------------------- */
/*  SDK Version Compatibility Table                                      */
/* --------------------------------------------------------------------- */
static const uint32_t sdk_version_pairs[] = {
    0x05050001, // 1 → 5.05
    0x06008001, // 2 → 6.00
    0x07008001, // 3 → 7.00
    0x09008001, // 4 → 9.00
    0x10008001, // 5 → 10.00
    0x11008001, // 6 → 11.00
};

#define SDK_PAIRS_MIN      1
#define SDK_PAIRS_MAX      6
#define DEFAULT_BACKPORT_LEVEL 1

/* --------------------------------------------------------------------- */
/*  Global SDK values                                                    */
/* --------------------------------------------------------------------- */
static uint32_t g_target_ps4_sdk = 0;
static int      g_backport_enabled = 0;

/* --------------------------------------------------------------------- */
/*  Config reading helpers                                               */
/* --------------------------------------------------------------------- */
static int read_uint32_from_ini(const char *key, uint32_t *out)
{
    const char *homebrew = get_usb_homebrew_path();
    if (!homebrew || !homebrew[0]) return -1;

    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.ini", homebrew);

    FILE *f = fopen(cfg_path, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, strlen(key)) != 0) continue;
        p += strlen(key);
        while (*p == ' ' || *p == '\t' || *p == '=') p++;
        char *end;
        unsigned long val = strtoul(p, &end, 0);
        if (end != p) {
            *out = (uint32_t)val;
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int read_backport_level_from_ini(const char *key, int *level)
{
    const char *homebrew = get_usb_homebrew_path();
    if (!homebrew || !homebrew[0]) return -1;

    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.ini", homebrew);

    FILE *f = fopen(cfg_path, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, key, strlen(key)) == 0) {
            p += strlen(key);
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            char *end;
            long val = strtol(p, &end, 0);
            if (end != p && val >= SDK_PAIRS_MIN && val <= SDK_PAIRS_MAX) {
                *level = (int)val;
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    return -1;
}

/* --------------------------------------------------------------------- */
/*  Load configuration                                                   */
/* --------------------------------------------------------------------- */
static void load_sdk_config(void)
{
    int level = DEFAULT_BACKPORT_LEVEL;
    uint32_t custom_ps4 = 0;
    int has_custom_ps4 = 0;
    int enable_backport = 1;

    if (read_uint32_from_ini("enable_backport", (uint32_t*)&enable_backport) == 0) {
        g_backport_enabled = enable_backport;
    }

    if (!g_backport_enabled) {
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "SDK Backport: DISABLED via config");
        return;
    }

    if (read_uint32_from_ini("min_ps4_sdk_version", &custom_ps4) == 0) {
        has_custom_ps4 = 1;
    }

    read_backport_level_from_ini("ps4_backport_level", &level);

    if (has_custom_ps4) {
        g_target_ps4_sdk = custom_ps4;
    } else if (level >= SDK_PAIRS_MIN && level <= SDK_PAIRS_MAX) {
        g_target_ps4_sdk = sdk_version_pairs[level - 1];
    } else {
        g_target_ps4_sdk = sdk_version_pairs[DEFAULT_BACKPORT_LEVEL - 1];
    }

    if (g_enable_logging && g_log_path[0])
        write_log(g_log_path, "SDK Backport: LEVEL %d -> PS4=0x%08X", level, g_target_ps4_sdk);
}

/* --------------------------------------------------------------------- */
/*  Patch single ELF                                                     */
/* --------------------------------------------------------------------- */
static int patch_elf(const char* path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0x40) { close(fd); return -1; }

    uint8_t *map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return -1; }

    int patched = 0;

    if (memcmp(map, PS4_FSELF_MAGIC, 4) == 0) goto cleanup;  // skip signed
    if (memcmp(map, ELF_MAGIC, 4) != 0) goto cleanup;

    uint64_t phoff = *(uint64_t*)(map + 0x20);
    uint16_t phnum = *(uint16_t*)(map + 0x38);

    char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : (char*)path;

    for (uint16_t i = 0; i < phnum; ++i) {
        uint8_t *phdr = map + phoff + i * 0x38;
        uint32_t p_type   = *(uint32_t*)(phdr + 0x00);
        uint64_t p_offset = *(uint64_t*)(phdr + 0x08);

        if (p_type != PT_SCE_PROCPARAM && p_type != PT_SCE_MODULE_PARAM) continue;
        if (p_offset + 0x30 > (uint64_t)st.st_size) continue;

        uint8_t *param = map + p_offset;
        uint32_t magic = *(uint32_t*)(param + 0x08);

        if (magic != SCE_PROCESS_PARAM_MAGIC && magic != SCE_MODULE_PARAM_MAGIC) continue;

        uint32_t old = *(uint32_t*)(param + 0x10);

        if (old > g_target_ps4_sdk && old != 0) {
            *(uint32_t*)(param + 0x10) = g_target_ps4_sdk;
            if (g_enable_logging && g_log_path[0])
                write_log(g_log_path, "Backported PS4 SDK 0x%08X -> 0x%08X in %s", old, g_target_ps4_sdk, path);
            patched = 1;
        }
    }

    if (patched) {
        printf_notification("Backported: %s", fname);
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "Backported: %s", fname);
    } else {
        printf_notification("Skipped Backport: %s", fname);
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "Backport: SKIPPED %s – SDK already compatible (no backport needed)", path);
    }

cleanup:
    munmap(map, st.st_size);
    close(fd);
    return patched ? 0 : -1;
}

/* --------------------------------------------------------------------- */
/*  Recursive ELF backport - skips 'decrypted' folder                     */
/* --------------------------------------------------------------------- */
static int backport_dir(const char* root)
{
    // Prevent backporting inside the decrypted backup folder
    const char *basename = strrchr(root, '/');
    if (basename) basename++;
    else basename = root;

    if (strcmp(basename, "decrypted") == 0) {
        return 0;  // Skip entirely
    }

    DIR* dir = opendir(root);
    if (!dir) {
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "backport: opendir(%s) failed: %s", root, strerror(errno));
        return -1;
    }

    struct dirent* ent;
    char fullpath[1024];

    while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        snprintf(fullpath, sizeof(fullpath), "%s/%s", root, ent->d_name);

        struct stat st;
        if (stat(fullpath, &st) < 0) continue;

        if (S_ISDIR(st.st_mode)) {
            backport_dir(fullpath);
            continue;
        }

        const char* ext = strrchr(ent->d_name, '.');
        if (!ext) continue;
        if (strcmp(ext, ".bin") && strcmp(ext, ".elf") && strcmp(ext, ".prx") && strcmp(ext, ".sprx"))
            continue;

        patch_elf(fullpath);
    }

    closedir(dir);
    return 0;
}

int ps4_backport_recursive(const char* root)
{
    load_sdk_config();

    if (!g_backport_enabled) return 0;

    if (!root || !*root) return -1;

    if (g_enable_logging && g_log_path[0])
        write_log(g_log_path, "PS4 backport: Starting recursive on %s (target SDK 0x%08X)", root, g_target_ps4_sdk);

    return backport_dir(root);
}

/* --------------------------------------------------------------------- */
/*  Backport param.sfo only                                              */
/* --------------------------------------------------------------------- */
int ps4_backport_param_sfo(const char* sfo_path)
{
    load_sdk_config();

    if (!g_backport_enabled) return 0;

    if (!sfo_path || !*sfo_path) return -1;

    int fd = open(sfo_path, O_RDWR);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 0x20) { close(fd); return -1; }

    uint8_t *map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return -1; }

    int patched = 0;

    if (memcmp(map, SFO_MAGIC, 4) != 0) goto cleanup;

    uint32_t key_table_offset  = *(uint32_t*)(map + 0x08);
    uint32_t data_table_offset = *(uint32_t*)(map + 0x0C);
    uint32_t num_entries       = *(uint32_t*)(map + 0x10);

    for (uint32_t i = 0; i < num_entries; ++i) {
        uint8_t *entry = map + 0x14 + i * 0x10;

        uint16_t key_offset   = *(uint16_t*)(entry + 0x00);
        uint16_t data_fmt     = *(uint16_t*)(entry + 0x02);
        uint32_t data_len     = *(uint32_t*)(entry + 0x04);
        uint32_t data_offset  = *(uint32_t*)(entry + 0x0C);

        uint8_t *key_ptr  = map + key_table_offset + key_offset;
        uint8_t *data_ptr = map + data_table_offset + data_offset;

        char key[64] = {0};
        for (int k = 0; k < sizeof(key)-1 && key_ptr[k]; ++k) key[k] = key_ptr[k];

        uint32_t old_val = 0;

        if (strcmp(key, "PUBTOOLINFO") == 0 && data_fmt == 0x204) {
            char pubtool[256] = {0};
            size_t len = data_len < sizeof(pubtool)-1 ? data_len : sizeof(pubtool)-1;
            memcpy(pubtool, data_ptr, len);

            char *pos = strstr(pubtool, "sdk_ver=");
            if (pos && isxdigit((unsigned char)pos[8])) {
                pos += 8;
                if (sscanf(pos, "%08X", &old_val) == 1 && old_val > g_target_ps4_sdk && old_val != 0) {
                    char new_str[9];
                    snprintf(new_str, 9, "%08X", g_target_ps4_sdk);
                    memcpy(pos, new_str, 8);
                    memcpy(data_ptr + (pos - pubtool), new_str, 8);
                    patched = 1;
                    if (g_enable_logging && g_log_path[0])
                        write_log(g_log_path, "Backported param.sfo PUBTOOLINFO sdk_ver 0x%08X -> 0x%08X", old_val, g_target_ps4_sdk);
                }
            }
        }
        else if (strcmp(key, "SYSTEM_VER") == 0 && data_fmt == 0x404 && data_len == 4) {
            old_val = *(uint32_t*)data_ptr;
            if (old_val > g_target_ps4_sdk && old_val != 0) {
                *(uint32_t*)data_ptr = g_target_ps4_sdk;
                patched = 1;
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "Backported param.sfo SYSTEM_VER 0x%08X -> 0x%08X", old_val, g_target_ps4_sdk);
            }
        }
    }

    if (patched) {
        printf_notification("Backported: param.sfo");
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "Backported: param.sfo");
    }

cleanup:
    munmap(map, st.st_size);
    close(fd);
    return 0;
}