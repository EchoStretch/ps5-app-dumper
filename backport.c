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
#define DEFAULT_BACKPORT_LEVEL 4

/* --------------------------------------------------------------------- */
/*  Global SDK values                                                    */
/* --------------------------------------------------------------------- */
static uint32_t g_target_ps5_sdk = 0;
static uint32_t g_target_ps4_sdk = 0;
static int      g_backport_enabled = 1;   // default: enabled

/* --------------------------------------------------------------------- */
/*  Helper: read a uint32 value from config.ini                          */
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

/* --------------------------------------------------------------------- */
/*  Helper: read backport_level from config.ini                          */
/* --------------------------------------------------------------------- */
static int read_backport_level_from_ini(int *level)
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
        if (strncmp(p, "backport_level", 14) == 0) {
            p += 14;
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
/*  Load configuration once                                              */
/* --------------------------------------------------------------------- */
static void load_sdk_config(void)
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
    has_custom_ps5 = (read_uint32_from_ini("min_ps5_sdk_version", &custom_ps5) == 0);
    has_custom_ps4 = (read_uint32_from_ini("min_ps4_sdk_version", &custom_ps4) == 0);

    if (has_custom_ps5 || has_custom_ps4) {
        g_target_ps5_sdk = has_custom_ps5 ? custom_ps5 : sdk_version_pairs[DEFAULT_BACKPORT_LEVEL-1].ps5_sdk;
        g_target_ps4_sdk = has_custom_ps4 ? custom_ps4 : sdk_version_pairs[DEFAULT_BACKPORT_LEVEL-1].ps4_sdk;

        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "SDK Backport: CUSTOM PS5=0x%08X PS4=0x%08X",
                      g_target_ps5_sdk, g_target_ps4_sdk);
        return;
    }

    /* ---- read backport_level (1-10) ---- */
    if (read_backport_level_from_ini(&level) == 0) {
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

        /* Patch PS5 SDK */
        if (g_backport_enabled && p_offset + SCE_PARAM_PS5_SDK_OFFSET + 4 <= (uint64_t)st.st_size) {
            uint32_t old = *(uint32_t *)(param + SCE_PARAM_PS5_SDK_OFFSET);
            if (old != g_target_ps5_sdk) {
                *(uint32_t *)(param + SCE_PARAM_PS5_SDK_OFFSET) = g_target_ps5_sdk;
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "Patched PS5 SDK 0x%08X -> 0x%08X in %s", old, g_target_ps5_sdk, path);
                patched = 1;
            }
        }

        /* Patch PS4 SDK */
        if (g_backport_enabled && p_offset + SCE_PARAM_PS4_SDK_OFFSET + 4 <= (uint64_t)st.st_size) {
            uint32_t old = *(uint32_t *)(param + SCE_PARAM_PS4_SDK_OFFSET);
            if (old != g_target_ps4_sdk) {
                *(uint32_t *)(param + SCE_PARAM_PS4_SDK_OFFSET) = g_target_ps4_sdk;
                if (g_enable_logging && g_log_path[0])
                    write_log(g_log_path, "Patched PS4 SDK 0x%08X -> 0x%08X in %s", old, g_target_ps4_sdk, path);
                patched = 1;
            }
        }
    }

cleanup:
    if (map && map != MAP_FAILED) munmap(map, st.st_size);
    close(fd);
    return patched ? 0 : -1;
}

/* --------------------------------------------------------------------- */
/*  Recursively backport all ELF-like files                              */
/* --------------------------------------------------------------------- */
int backport_recursive(const char *root)
{
    DIR *dir = opendir(root);
    if (!dir) {
        if (g_enable_logging && g_log_path[0])
            write_log(g_log_path, "backport: opendir(%s) failed: %s", root, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    char fullpath[1024];
    int rc = 0;

    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

        snprintf(fullpath, sizeof(fullpath), "%s/%s", root, ent->d_name);

        struct stat st;
        if (stat(fullpath, &st) < 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (backport_recursive(fullpath) != 0) rc = -1;
            continue;
        }

        const char *ext = strrchr(ent->d_name, '.');
        if (!ext) continue;
        if (strcmp(ext, ".bin")  && strcmp(ext, ".elf") && strcmp(ext, ".self") &&
            strcmp(ext, ".prx")  && strcmp(ext, ".sprx")) continue;

        if (patch_elf(fullpath) == 0) {
            if (g_enable_logging && g_log_path[0])
                write_log(g_log_path, "backport: patched %s", fullpath);
        }
    }

    closedir(dir);
    return rc;
}

/* --------------------------------------------------------------------- */
/*  Public API                                                           */
/* --------------------------------------------------------------------- */
int backport_sdk_file(const char *path)
{
    load_sdk_config();
    return patch_elf(path);
}