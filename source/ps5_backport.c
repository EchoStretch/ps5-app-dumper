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
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>

#include "utils.h"

/* --------------------------------------------------------------------- */
/*  ELF Constants                                                        */
/* --------------------------------------------------------------------- */
#define ELF_MAGIC           "\x7F""ELF"
#define PS4_FSELF_MAGIC     "\x4F\x15\x3D\x1D"
#define PS5_FSELF_MAGIC     "\x54\x14\xF5\xEE"

#define PT_SCE_PROCPARAM    0x61000001U
#define PT_SCE_MODULE_PARAM 0x61000002U

#define SCE_PROCESS_PARAM_MAGIC 0x4942524F
#define SCE_MODULE_PARAM_MAGIC  0x3C13F4BF

#define SCE_PARAM_PS5_SDK_OFFSET 0xC
#define SCE_PARAM_PS4_SDK_OFFSET 0x8

#define PHT_OFFSET_OFFSET   0x20   // e_phoff
#define PHT_COUNT_OFFSET    0x38   // e_phnum
#define PHDR_ENTRY_SIZE     0x38
#define PHDR_TYPE_OFFSET    0x00
#define PHDR_OFFSET_OFFSET  0x08

/* --------------------------------------------------------------------- */
/*  SDK Version Compatibility Table                                      */
/* --------------------------------------------------------------------- */
typedef struct {
    uint32_t ps5_sdk;
    uint32_t ps4_sdk;
} sdk_pair_t;

static const sdk_pair_t sdk_version_pairs[] = {
    {0x01000050, 0x07590001}, // 1
    {0x02000009, 0x08050001}, // 2
    {0x03000027, 0x08540001}, // 3
    {0x04000031, 0x09040001}, // 4
    {0x05000033, 0x09590001}, // 5
    {0x06000038, 0x10090001}, // 6
    {0x07000038, 0x10590001}, // 7
    {0x08000041, 0x11090001}, // 8
    {0x09000040, 0x11590001}, // 9
    {0x10000040, 0x12090001}, // 10
};

#define SDK_PAIRS_COUNT    10
#define SDK_PAIRS_MIN      1
#define SDK_PAIRS_MAX      10
#define DEFAULT_BACKPORT_LEVEL 1

/* --------------------------------------------------------------------- */
/*  Global SDK values                                                    */
/* --------------------------------------------------------------------- */
static uint32_t g_target_ps5_sdk = 0;
static uint32_t g_target_ps4_sdk = 0;
static int      g_backport_enabled = 0;   // default: disabled

/* --------------------------------------------------------------------- */
/*  Helper: read a ps5 value from config.ini                              */
/* --------------------------------------------------------------------- */
static int read_ps5_custom_sdk(const char *key, uint32_t *out)
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

/* --------------------------------------------------------------------- */
/*  Helper: read ps5 backport_level from config.ini                      */
/* --------------------------------------------------------------------- */
static int read_ps5_backport_level(int *level)
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
        if (strncmp(p, "ps5_backport_level", 18) == 0) {
            p += 18;
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
/*  Load configuration                                           */
/* --------------------------------------------------------------------- */
static void load_ps5_sdk_config(void)
{
    int      level = DEFAULT_BACKPORT_LEVEL;
    uint32_t custom_ps5 = 0, custom_ps4 = 0;
    int      has_custom_ps5 = 0, has_custom_ps4 = 0;
    int      enable_backport = 1;

    /* ---- read enable_backport ---- */
    const char *homebrew = get_usb_homebrew_path();
    if (homebrew && homebrew[0]) {
        char cfg_path[512];
        snprintf(cfg_path, sizeof(cfg_path), "%s/config.ini", homebrew);
        FILE *f = fopen(cfg_path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (strncmp(p, "enable_backport", 15) == 0) {
                    p += 15;
                    while (*p == ' ' || *p == '\t' || *p == '=') p++;
                    if (*p == '0') enable_backport = 0;
                }
            }
            fclose(f);
        }
    }

    g_backport_enabled = enable_backport;

    if (!g_backport_enabled) {
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "SDK Backport: DISABLED via config");
        return;
    }

    /* ---- optional custom SDK overrides ---- */
    has_custom_ps5 = (read_ps5_custom_sdk("min_ps5_sdk_version", &custom_ps5) == 0);
    has_custom_ps4 = (read_ps5_custom_sdk("min_ps4_sdk_version", &custom_ps4) == 0);

    if (has_custom_ps5 || has_custom_ps4) {
        g_target_ps5_sdk = has_custom_ps5 ? custom_ps5 : sdk_version_pairs[DEFAULT_BACKPORT_LEVEL-1].ps5_sdk;
        g_target_ps4_sdk = has_custom_ps4 ? custom_ps4 : sdk_version_pairs[DEFAULT_BACKPORT_LEVEL-1].ps4_sdk;

        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "SDK Backport: CUSTOM PS5=0x%08X PS4=0x%08X",
                      g_target_ps5_sdk, g_target_ps4_sdk);
        return;
    }

    /* ---- read backport_level (1-10) ---- */
    if (read_ps5_backport_level(&level) == 0) {
        g_target_ps5_sdk = sdk_version_pairs[level-1].ps5_sdk;
        g_target_ps4_sdk = sdk_version_pairs[level-1].ps4_sdk;
    } else {
        /* fallback to default level */
        g_target_ps5_sdk = sdk_version_pairs[DEFAULT_BACKPORT_LEVEL-1].ps5_sdk;
        g_target_ps4_sdk = sdk_version_pairs[DEFAULT_BACKPORT_LEVEL-1].ps4_sdk;
    }

    if (g_enable_logging && g_log_path[0])
        write_log(g_log_path, "SDK Backport: LEVEL %d -> PS5=0x%08X PS4=0x%08X",
                  level, g_target_ps5_sdk, g_target_ps4_sdk);
}

/* --------------------------------------------------------------------- */
/*  Patch single ELF                                                     */
/* --------------------------------------------------------------------- */
static int patch_elf(const char *path)
{
    int fd = -1;
    uint8_t *map = NULL;
    struct stat st;
    int patched = 0;

    fd = open(path, O_RDWR);
    if (fd < 0) {
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "backport: open(%s) failed: %s", path, strerror(errno));
        return -1;
    }

    /* Notify start of backport */
    if (g_backport_enabled) {
        char *fname = strrchr(path, '/');
        fname = fname ? fname + 1 : (char*)path;
    }

    if (fstat(fd, &st) < 0) goto cleanup;
    if (st.st_size < 0x40) goto cleanup;

    map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) goto cleanup;

    /* Skip signed SELF */
    if (memcmp(map, PS4_FSELF_MAGIC, 4) == 0 || memcmp(map, PS5_FSELF_MAGIC, 4) == 0) {
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "backport: %s is signed SELF – skipping", path);
        goto cleanup;
    }

    if (memcmp(map, ELF_MAGIC, 4) != 0) goto cleanup;

    uint64_t phoff = *(uint64_t *)(map + PHT_OFFSET_OFFSET);
    uint16_t phnum = *(uint16_t *)(map + PHT_COUNT_OFFSET);

    if (phoff + phnum * PHDR_ENTRY_SIZE > (uint64_t)st.st_size) goto cleanup;

    for (uint16_t i = 0; i < phnum; ++i) {
        uint8_t *phdr = map + phoff + i * PHDR_ENTRY_SIZE;
        uint32_t p_type   = *(uint32_t *)(phdr + PHDR_TYPE_OFFSET);
        uint64_t p_offset = *(uint64_t *)(phdr + PHDR_OFFSET_OFFSET);

        if (p_type != PT_SCE_PROCPARAM && p_type != PT_SCE_MODULE_PARAM) continue;
        if (p_offset + 0x18 > (uint64_t)st.st_size) continue;

        uint8_t *param = map + p_offset;
        uint32_t magic = *(uint32_t *)param;

        /* Skip possible 8-byte header */
        if ((p_type == PT_SCE_PROCPARAM && magic != SCE_PROCESS_PARAM_MAGIC) ||
            (p_type == PT_SCE_MODULE_PARAM && magic != SCE_MODULE_PARAM_MAGIC)) {
            param += 8;
            magic = *(uint32_t *)param;
        }

        if ((p_type == PT_SCE_PROCPARAM && magic != SCE_PROCESS_PARAM_MAGIC) ||
            (p_type == PT_SCE_MODULE_PARAM && magic != SCE_MODULE_PARAM_MAGIC))
            continue;

        load_ps5_sdk_config();  // Refresh config in case it changed

        /* Patch PS5 SDK */
        if (g_backport_enabled && p_offset + SCE_PARAM_PS5_SDK_OFFSET + 4 <= (uint64_t)st.st_size) {
            uint32_t old = *(uint32_t *)(param + SCE_PARAM_PS5_SDK_OFFSET);
            if (old > g_target_ps5_sdk) {
                *(uint32_t *)(param + SCE_PARAM_PS5_SDK_OFFSET) = g_target_ps5_sdk;
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "Backported PS5 SDK 0x%08X -> 0x%08X in %s", old, g_target_ps5_sdk, path);
                patched = 1;
            } else if (old < g_target_ps5_sdk) {
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "Preserved PS5 SDK 0x%08X (already lower than target 0x%08X) in %s", old, g_target_ps5_sdk, path);
            } else {
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "PS5 SDK already at target 0x%08X in %s", old, path);
            }
        }

        /* Patch PS4 SDK */
        if (g_backport_enabled && p_offset + SCE_PARAM_PS4_SDK_OFFSET + 4 <= (uint64_t)st.st_size) {
            uint32_t old = *(uint32_t *)(param + SCE_PARAM_PS4_SDK_OFFSET);
            if (old > g_target_ps4_sdk) {
                *(uint32_t *)(param + SCE_PARAM_PS4_SDK_OFFSET) = g_target_ps4_sdk;
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "Backported PS4 SDK 0x%08X -> 0x%08X in %s", old, g_target_ps4_sdk, path);
                patched = 1;
            } else if (old < g_target_ps4_sdk) {
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "Preserved PS4 SDK 0x%08X (already lower than target 0x%08X) in %s", old, g_target_ps4_sdk, path);
            } else {
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "PS4 SDK already at target 0x%08X in %s", old, path);
            }
        }
    }

    /* ---------- BACKPORT RESULT NOTIFICATION ---------- */
    if (g_backport_enabled) {
        char *fname = strrchr(path, '/');
        fname = fname ? fname + 1 : (char*)path;

        if (patched) {
            printf_notification("Backported: %s", fname);
            if (g_enable_logging && g_log_path[0])
                write_log(g_log_path, "Backported: %s", fname);
        } else {
            printf_notification("Skipped Backport: %s", fname);
            if (g_enable_logging && g_log_path[0])
                write_log(g_log_path, "Backport: SKIPPED %s – SDK already compatible (no backport needed)", path);
        }
    }
    /* ------------------------------------------------ */

cleanup:
    if (map && map != MAP_FAILED) munmap(map, st.st_size);
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
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "backport: Skipping entire 'decrypted' folder: %s", root);
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
    int rc = 0;

    while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        snprintf(fullpath, sizeof(fullpath), "%s/%s", root, ent->d_name);

        struct stat st;
        if (stat(fullpath, &st) < 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (backport_dir(fullpath) != 0) rc = -1;
            continue;
        }

        const char* ext = strrchr(ent->d_name, '.');
        if (!ext) continue;
        if (strcmp(ext, ".bin") && strcmp(ext, ".elf") && strcmp(ext, ".self") &&
            strcmp(ext, ".prx") && strcmp(ext, ".sprx"))
            continue;

        if (patch_elf(fullpath) != 0) {
            // patch_elf returns 0 on success (patched or skipped), -1 on error
            rc = -1;
        }
    }

    closedir(dir);
    return rc;
}

int ps5_backport_recursive(const char *root)
{
    load_ps5_sdk_config();

    if (!g_backport_enabled) {
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "PS5 SDK Backport: DISABLED via config");
        return 0;
    }

    if (!root || !*root) return -1;

    if (g_enable_logging && g_log_path[0])
        write_log(g_log_path, "PS5 backport: Starting recursive on %s (target PS5=0x%08X PS4=0x%08X)",
                  root, g_target_ps5_sdk, g_target_ps4_sdk);

    return backport_dir(root);
}

/* --------------------------------------------------------------------- */
/*  Backport param.json - requiredSystemSoftwareVersion & sdkVersion     */
/* --------------------------------------------------------------------- */
int ps5_backport_param_json(const char *json_path)
{
    load_ps5_sdk_config();
    if (!g_backport_enabled) return 0;
    if (!json_path || !*json_path) return -1;

    int fd = open(json_path, O_RDWR);
    if (fd < 0) {
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "param.json: open failed %s: %s", json_path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 100) { close(fd); return -1; }

    char *map = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return -1; }

    int patched = 0;

    // Major version (e.g., "01" for level 1)
    char major_hex[3];
    snprintf(major_hex, sizeof(major_hex), "%02X", (g_target_ps5_sdk >> 24) & 0xFF);

    // Exact target string we want: "0xMM00000000000000" (18 chars, no quotes here)
    char target_hex[20];
    snprintf(target_hex, sizeof(target_hex), "0x%s00000000000000", major_hex);

    const char *keys[] = { "requiredSystemSoftwareVersion", "sdkVersion" };

    for (int i = 0; i < 2; i++) {
        char search[128];
        snprintf(search, sizeof(search), "\"%s\"", keys[i]);

        char *pos = map;
        char *end = map + st.st_size;

        while ((pos = strstr(pos, search)) != NULL) {
            pos += strlen(search);

            // Skip whitespace and colon
            while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':')) pos++;

            if (pos >= end || *pos != '"') { pos++; continue; }

            char *quote_start = pos;        // points to opening "
            pos++;                          // move to first char after "
            char *val_start = pos;          // points to '0' of 0x...

            // Find closing quote
            while (pos < end && *pos != '"') pos++;
            if (pos >= end) break;

            // Extract current major (first 2 hex digits after 0x)
            char current_major[3] = {0};
            if (pos - val_start > 4) memcpy(current_major, val_start + 2, 2);

            uint8_t current_maj = (uint8_t)strtoul(current_major, NULL, 16);
            uint8_t target_maj = (g_target_ps5_sdk >> 24) & 0xFF;

            // Save original value for log (including quotes)
            char current_full[40] = {0};
            size_t full_len = pos - quote_start + 1; // include closing "
            if (full_len > 39) full_len = 39;
            memcpy(current_full, quote_start, full_len);
            current_full[full_len] = '\0';

            if (current_maj > target_maj) {
                // Overwrite from opening " to closing " with: "0xMM00000000000000"
                // Total length: 1 (") + 18 (0xMM...) + 1 (") = 20 chars
                char new_value[21] = "\"0x";
                strcat(new_value, major_hex);
                strcat(new_value, "00000000000000\"");

                memcpy(quote_start, new_value, 20);

                patched = 1;

                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "Backported param.json %s: %s -> \"%s\"", 
                              keys[i], current_full, target_hex);
            } else {
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "param.json %s already at or below target major (0x%02X)", keys[i], current_maj);
            }

            break;
        }
    }

    if (patched) {
        printf_notification("Backported: param.json");
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "Backported: param.json");
        msync(map, st.st_size, MS_SYNC);
    }

    munmap(map, st.st_size);
    close(fd);
    return patched ? 0 : -1;
}

/* --------------------------------------------------------------------- */
/*  Public API                                                           */
/* --------------------------------------------------------------------- */
int ps5_backport_sdk_file(const char *path)
{
    load_ps5_sdk_config();
    return patch_elf(path);
}