#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/elf64.h>

#include "utils.h"
#include "decrypt.h"
#include "backport.h"
#include "elf2fself.h"
#include "selfpager.h"

/*=====================================================================
 *  Global progress
 *====================================================================*/
int g_total_files  = 0;
int g_current_file = 0;

/*=====================================================================
 *  Forward declarations
 *====================================================================*/
static int count_files_recursive(const char *dir_path);
static int process_file(const char *input_path, const char *output_path,
                        const char *root_dst, int do_elf2fself, int do_backport);
static int decrypt_and_process_all(const char *input_dir, const char *output_dir,
                                   const char *root_dst, int do_elf2fself, int do_backport);

/*=====================================================================
 *  Public entry point
 *====================================================================*/
int decrypt_all(const char *src_game, const char *dst_game,
                int do_elf2fself, int do_backport)
{
    g_total_files = count_files_recursive(src_game);
    g_current_file = 0;

    int res = decrypt_and_process_all(src_game, dst_game, dst_game,
                                      do_elf2fself, do_backport);
    return res;
}

/*=====================================================================
 *  Count ELF files
 *====================================================================*/
static int count_files_recursive(const char *dir_path)
{
    int total = 0;
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    struct dirent *ent;
    char full[PATH_MAX];

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);

        if (ent->d_type == DT_DIR) {
            total += count_files_recursive(full);
        } else if (ent->d_type == DT_REG) {
            const char *ext = strrchr(ent->d_name, '.');
            if (ext && (!strcasecmp(ext, ".elf") || !strcasecmp(ext, ".self") ||
                        !strcasecmp(ext, ".prx") || !strcasecmp(ext, ".sprx") ||
                        !strcasecmp(ext, ".bin"))) {
                total++;
            }
        }
    }
    closedir(dir);
    return total;
}

/*=====================================================================
 *  Process ONE file: decrypt → copy → backport → fself
 *====================================================================*/
static int process_file(const char *input_path, const char *output_path,
                        const char *root_dst, int do_elf2fself, int do_backport)
{
    /* --- 1. DECRYPT --- */
    int fd = open(input_path, O_RDONLY);
    if (fd < 0) return -1;

    uint64_t out_size = 0;
    char *out_data = NULL;
    int res = decrypt_self(fd, &out_data, &out_size);
    close(fd);

    if (res == DECRYPT_ERROR_INPUT_NOT_SELF) return 0;
    if (res != 0) return res;

    /* Create output dir */
    char *slash = strrchr(output_path, '/');
    if (slash) {
        char dir[PATH_MAX];
        size_t len = slash - output_path;
        memcpy(dir, output_path, len);
        dir[len] = '\0';
        mkdirs(dir);
    }

    int out_fd = open(output_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        munmap(out_data, out_size);
        return -1;
    }
    write(out_fd, out_data, out_size);
    munmap(out_data, out_size);
    close(out_fd);

    /* --- 2. COPY TO decrypted/ --- */
    if (do_elf2fself || do_backport) {
        const char *rel = output_path + strlen(root_dst);
        if (*rel == '/') rel++;

        char dest_path[PATH_MAX];
        snprintf(dest_path, sizeof(dest_path), "%s/decrypted/%s", root_dst, rel);

        char *last = strrchr(dest_path, '/');
        if (last) {
            *last = '\0';
            mkdirs(dest_path);
            *last = '/';
        } else {
            mkdirs(dest_path);
        }

        int src = open(output_path, O_RDONLY);
        int dst = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (src >= 0 && dst >= 0) {
            char buf[8192];
            ssize_t n;
            while ((n = read(src, buf, sizeof(buf))) > 0)
                write(dst, buf, n);
            fsync(dst);
            close(dst);
        }
        if (src >= 0) close(src);
        if (dst >= 0) close(dst);
    }

    /* --- 3. BACKPORT --- */
    if (do_backport) {
        backport_sdk_file(output_path);
    }

    /* --- 4. FSELF --- */
    if (do_elf2fself) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s.tmp", output_path);
        if (rename(output_path, tmp) == 0) {
            if (elf2fself(tmp, output_path) == 0) {
                unlink(tmp);
            } else {
                rename(tmp, output_path);
            }
        }
    }
    
        return 0;
    }

/*=====================================================================
 *  Walk and process each file in order
 *====================================================================*/
static int decrypt_and_process_all(const char *input_dir, const char *output_dir,
                                   const char *root_dst, int do_elf2fself, int do_backport)
{
    DIR *dir = opendir(input_dir);
    if (!dir) return -1;

    struct dirent *ent;
    char in_path[PATH_MAX];
    char out_path[PATH_MAX];

    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        snprintf(in_path, sizeof(in_path), "%s/%s", input_dir, name);
        snprintf(out_path, sizeof(out_path), "%s/%s", output_dir, name);

        if (ent->d_type == DT_DIR) {
            /* Skip union folders */
            if (ent->d_namlen == sizeof("PPSA00000-app0-patch0-union") - 1 &&
                strncmp(input_dir, "/mnt/sandbox/pfsmnt", 19) == 0 &&
                strncmp(name + 9, "-app0-patch0-union", 18) == 0) {
                continue;
            }
            decrypt_and_process_all(in_path, out_path, root_dst, do_elf2fself, do_backport);
            continue;
        }

        if (ent->d_type != DT_REG) continue;

        const char *ext = strrchr(name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".elf") && strcasecmp(ext, ".self") &&
            strcasecmp(ext, ".prx") && strcasecmp(ext, ".sprx") &&
            strcasecmp(ext, ".bin")) continue;

        /* PROGRESS */
        char msg[512];
        g_current_file++;
        snprintf(msg, sizeof(msg), "Decrypting %d/%d: %s", g_current_file, g_total_files, name);
        printf_notification(msg);

        /* PROCESS FULLY */
        process_file(in_path, out_path, root_dst, do_elf2fself, do_backport);
    }

    closedir(dir);
    return 0;
}