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

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sha256.h"
#include "utils.h"

#define SELF_PS4_MAGIC      0x1D3D154F
#define SELF_PS5_MAGIC      0xEEF51454

#define PT_SCE_DYNLIBDATA   0x61000000
#define PT_SCE_RELRO        0x61000010
#define PT_SCE_COMMENT      0x6FFFFF00
#define PT_SCE_VERSION      0x6FFFFF01

#define META_ENTRY_SIZE     0x4000
#define ROUND_UP(val, alignment) (((val) + (alignment - 1)) & ~(alignment - 1))

typedef struct self_head {
    uint32_t magic;
    uint8_t  version;
    uint8_t  mode;
    uint8_t  endian;
    uint8_t  attrs;
    uint32_t key_type;
    uint16_t header_size;
    uint16_t meta_size;
    uint64_t file_size;
    uint16_t num_entries;
    uint16_t flags;
    uint8_t  pad[4];
} self_head_t;

typedef struct self_entry {
    struct __attribute__((packed)) {
        uint8_t  is_ordered     : 1;
        uint8_t  is_encrypted   : 1;
        uint8_t  is_signed      : 1;
        uint8_t  is_compressed  : 1;
        uint8_t  unknown0       : 4;
        uint8_t  window_bits    : 3;
        uint8_t  has_blocks     : 1;
        uint8_t  block_bits     : 4;
        uint8_t  has_digest     : 1;
        uint8_t  has_extents    : 1;
        uint8_t  unknown1       : 2;
        uint16_t segment_index  : 16;
        uint32_t unknown2       : 28;
    } props;
    uint64_t offset;
    uint64_t enc_size;
    uint64_t dec_size;
} self_entry_t;

typedef struct self_exinfo {
    uint64_t authid;
    uint64_t type;
    uint64_t app_version;
    uint64_t fw_version;
    uint8_t  digest[0x20];
} self_exinfo_t;

typedef struct self_npdrm_block {
    uint16_t type;
    uint8_t  unknown[0x0e];
    uint8_t  content_id[0x13];
    uint8_t  random_pad[0x0d];
} self_npdrm_block_t;

typedef struct self_meta_block {
    uint8_t unknown[0x50];
} self_meta_block_t;

typedef struct self_meta_foot {
    uint8_t  unknown0[0x30];
    int32_t  unknown1;
    uint8_t  unknown2[0x1c];
    uint8_t  signature[0x100];
} self_meta_foot_t;

typedef struct self_entry_map {
    Elf64_Phdr   phdr;
    self_entry_t entry;
} self_entry_map_t;

extern char g_log_path[512];
extern int  g_enable_logging;

/* ====================== Helper Functions ====================== */

static const char *get_fname(const char *path)
{
    const char *fname = strrchr(path, '/');
    return fname ? fname + 1 : path;
}

static int fs_nread(int fd, void *buf, size_t n)
{
    int r;
    r = read(fd, buf, n);
    if (r < 0) return -1;
    if ((size_t)r != n) { errno = EIO; return -1; }
    return 0;
}

static int fs_nwrite(int fd, const void *buf, size_t n)
{
    int r;
    r = write(fd, buf, n);
    if (r < 0) return -1;
    if ((size_t)r != n) { errno = EIO; return -1; }
    return 0;
}

static int fs_ncopy(int fd_in, int fd_out, size_t size)
{
    size_t copied = 0;
    char buf[0x4000];
    ssize_t n;

    while (copied < size) {
        n = size - copied;
        if (n > sizeof(buf)) n = sizeof(buf);

        if (fs_nread(fd_in, buf, n)) return -1;
        if (fs_nwrite(fd_out, buf, n)) return -1;
        copied += n;
    }
    return 0;
}

static int fs_align(int fd, int alignment)
{
    long off;

    off = lseek(fd, 0, SEEK_CUR);
    if (off < 0) return -1;

    off = ROUND_UP(off, alignment);
    if (lseek(fd, off, SEEK_SET) < 0) return -1;

    return 0;
}

static int fs_sha256sum(int fd, uint8_t hash[SHA256_BLOCK_SIZE])
{
    uint8_t buf[0x1000];
    SHA256_CTX ctx;
    off_t cur, end;
    ssize_t n;

    cur = lseek(fd, 0, SEEK_CUR);
    if (cur < 0) return -1;

    end = lseek(fd, 0, SEEK_END);
    if (end < 0) return -1;

    if (lseek(fd, 0, SEEK_SET) < 0) return -1;

    sha256_init(&ctx);
    for (off_t i = 0; i < end; i += sizeof(buf)) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0) return -1;
        sha256_update(&ctx, buf, n);
    }
    sha256_final(&ctx, hash);

    if (lseek(fd, cur, SEEK_SET) < 0) return -1;
    return 0;
}

/* ====================== Main Function ====================== */

int elf2fself(const char *elf_path, const char *fself_path)
{
    self_head_t head = {
        .magic       = SELF_PS5_MAGIC,
        .version     = 0,
        .mode        = 1,
        .endian      = 1,
        .attrs       = 0x12,
        .key_type    = 0x101,
        .header_size = 0,
        .meta_size   = 0,
        .file_size   = 0,
        .num_entries = 0,
        .flags       = 0x22
    };

    self_meta_foot_t   metafoot = { .unknown1 = 0x10000 };
    self_meta_block_t  metablk  = {0};
    self_npdrm_block_t npdrm    = {0};
    self_exinfo_t      exinfo   = {0};
    self_entry_map_t  *entry_map;
    Elf64_Ehdr ehdr;
    Elf64_Phdr phdr;
    Elf64_Phdr version_seg;
    off_t offset;
    int elf_fd;
    int self_fd;

    /* --- Initialization --- */
    npdrm.type = 3;
    exinfo.authid = 0x3100000000000002;
    exinfo.type   = 1;

    /* --- Open input ELF --- */
    elf_fd = open(elf_path, O_RDONLY, 0);
    if (elf_fd < 0) {
        write_log(g_log_path, "elf2fself: open(%s) failed: %s", elf_path, strerror(errno));
        return -1;
    }

    if (fs_nread(elf_fd, &ehdr, sizeof(ehdr))) {
        write_log(g_log_path, "elf2fself: read ELF header failed: %s", strerror(errno));
        close(elf_fd);
        return -1;
    }

    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F') {
        write_log(g_log_path, "elf2fself: Invalid ELF magic");
        close(elf_fd);
        return -1;
    }

    //ehdr.e_shnum     = 0;
    //ehdr.e_shentsize = 0;
    //ehdr.e_shstrndx  = 0;

    /* --- Count relevant program headers --- */
    if (lseek(elf_fd, ehdr.e_phoff, SEEK_SET) < 0) {
        write_log(g_log_path, "elf2fself: seek ELF program headers failed: %s", strerror(errno));
        close(elf_fd);
        return -1;
    }

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (fs_nread(elf_fd, &phdr, sizeof(phdr))) {
            write_log(g_log_path, "elf2fself: read ELF program header failed: %s", strerror(errno));
            close(elf_fd);
            return -1;
        }
        if (phdr.p_type == PT_SCE_VERSION) {
            memcpy(&version_seg, &phdr, sizeof(phdr));
        }
        if (phdr.p_type == PT_LOAD || phdr.p_type == PT_SCE_RELRO ||
            phdr.p_type == PT_SCE_DYNLIBDATA || phdr.p_type == PT_SCE_COMMENT) {
            head.num_entries += 2;
        }
    }

    /* --- Calculate header and meta sizes --- */
    head.header_size = sizeof(self_head_t);
    head.header_size += head.num_entries * sizeof(self_entry_t);
    head.header_size = ROUND_UP(head.header_size, 0x10);
    head.header_size += ehdr.e_phoff + ehdr.e_phnum * sizeof(Elf64_Phdr);
    head.header_size = ROUND_UP(head.header_size, 0x10);
    head.header_size += sizeof(self_exinfo_t);
    head.header_size += sizeof(self_npdrm_block_t);

    head.meta_size = head.num_entries * sizeof(self_meta_block_t);
    head.meta_size += sizeof(self_meta_foot_t);
	head.meta_size += 0x100;

    /* --- Allocate entry map --- */
    entry_map = calloc(head.num_entries, sizeof(*entry_map));
    if (!entry_map) {
        write_log(g_log_path, "elf2fself: calloc failed: %s", strerror(errno));
        close(elf_fd);
        return -1;
    }

    /* --- Map ELF program headers to SELF entries --- */
    if (lseek(elf_fd, ehdr.e_phoff, SEEK_SET) < 0) {
        write_log(g_log_path, "elf2fself: seek ELF program headers (2) failed: %s", strerror(errno));
        close(elf_fd);
        free(entry_map);
        return -1;
    }

    offset = head.header_size + head.meta_size;

    for (int i = 0, j = 0; i < ehdr.e_phnum; i++) {
        if (fs_nread(elf_fd, &phdr, sizeof(phdr))) {
            write_log(g_log_path, "elf2fself: read ELF program header (2) failed: %s", strerror(errno));
            close(elf_fd);
            free(entry_map);
            return -1;
        }

        if (phdr.p_type == PT_LOAD || phdr.p_type == PT_SCE_RELRO ||
            phdr.p_type == PT_SCE_DYNLIBDATA || phdr.p_type == PT_SCE_COMMENT) {

            /* Digest entry */
            memcpy(&entry_map[j], &phdr, sizeof(phdr));
            entry_map[j].entry.props.is_signed     = 1;
            entry_map[j].entry.props.has_digest    = 1;
            entry_map[j].entry.props.segment_index = j + 1;
            entry_map[j].entry.enc_size = (ROUND_UP(phdr.p_filesz, META_ENTRY_SIZE) / META_ENTRY_SIZE) * SHA256_BLOCK_SIZE;
            entry_map[j].entry.dec_size = entry_map[j].entry.enc_size;
            entry_map[j].entry.offset   = offset;
            offset = ROUND_UP(offset + entry_map[j].entry.enc_size, 0x10);
            j++;

            /* Block entry */
            memcpy(&entry_map[j], &phdr, sizeof(phdr));
            entry_map[j].entry.props.is_signed     = 1;
            entry_map[j].entry.props.has_blocks    = 1;
            entry_map[j].entry.props.segment_index = i;
            entry_map[j].entry.props.block_bits    = 2;
            entry_map[j].entry.enc_size = phdr.p_filesz;
            entry_map[j].entry.dec_size = phdr.p_filesz;
            entry_map[j].entry.offset   = offset;
            offset = ROUND_UP(offset + entry_map[j].entry.enc_size, 0x10);
            j++;
        }
    }

    /* --- Final file size --- */
    head.file_size = head.header_size + head.meta_size;
    for (int i = 0; i < head.num_entries; i++) {
        head.file_size += ROUND_UP(entry_map[i].entry.enc_size, 0x10);
    }
    head.file_size = ROUND_UP(head.file_size, 0x10);

    /* --- Open output FSELF --- */
    self_fd = open(fself_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (self_fd < 0) {
        write_log(g_log_path, "elf2fself: open(%s) failed: %s", fself_path, strerror(errno));
        free(entry_map);
        close(elf_fd);
        return -1;
    }

    /* --- Write SELF header --- */
    if (fs_nwrite(self_fd, &head, sizeof(head))) {
        write_log(g_log_path, "elf2fself: write SELF header failed: %s", strerror(errno));
        goto fail;
    }

    /* --- Write SELF entries --- */
    for (int i = 0; i < head.num_entries; i++) {
        if (fs_nwrite(self_fd, &entry_map[i].entry, sizeof(self_entry_t))) {
            write_log(g_log_path, "elf2fself: write SELF entry failed: %s", strerror(errno));
            goto fail;
        }
    }

    /* --- Write stripped ELF header --- */
    if (fs_nwrite(self_fd, &ehdr, sizeof(ehdr))) {
        write_log(g_log_path, "elf2fself: write ELF header failed: %s", strerror(errno));
        goto fail;
    }

    /* --- Write ELF program headers --- */
    if (lseek(elf_fd, ehdr.e_phoff, SEEK_SET) < 0) {
        write_log(g_log_path, "elf2fself: seek ELF (2) failed: %s", strerror(errno));
        goto fail;
    }

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (fs_nread(elf_fd, &phdr, sizeof(phdr))) {
            write_log(g_log_path, "elf2fself: read ELF program header (3) failed: %s", strerror(errno));
            goto fail;
        }
        if (fs_nwrite(self_fd, &phdr, sizeof(phdr))) {
            write_log(g_log_path, "elf2fself: write ELF program header failed: %s", strerror(errno));
            goto fail;
        }
    }

    if (fs_align(self_fd, 0x10)) {
        write_log(g_log_path, "elf2fself: fs_align failed: %s", strerror(errno));
        goto fail;
    }

    /* --- Compute and write ELF digest --- */
    if (fs_sha256sum(elf_fd, exinfo.digest)) {
        write_log(g_log_path, "elf2fself: fs_sha256sum failed");
        goto fail;
    }

    if (fs_nwrite(self_fd, &exinfo, sizeof(exinfo))) {
        write_log(g_log_path, "elf2fself: write exinfo failed: %s", strerror(errno));
        goto fail;
    }

    if (fs_nwrite(self_fd, &npdrm, sizeof(npdrm))) {
        write_log(g_log_path, "elf2fself: write npdrm failed: %s", strerror(errno));
        goto fail;
    }

    /* --- Write meta blocks --- */
    for (int i = 0; i < head.num_entries; i++) {
        if (fs_nwrite(self_fd, &metablk, sizeof(metablk))) {
            write_log(g_log_path, "elf2fself: write meta block failed: %s", strerror(errno));
            goto fail;
        }
    }

    if (fs_nwrite(self_fd, &metafoot, sizeof(metafoot))) {
        write_log(g_log_path, "elf2fself: write meta footer failed: %s", strerror(errno));
        goto fail;
    }

    uint8_t signature[0x100] = {0};
    if (fs_nwrite(self_fd, signature, sizeof(signature))) {
        write_log(g_log_path, "elf2fself: write signature failed: %s", strerror(errno));
        goto fail;
    }

    /* --- Copy segment data (only block entries) --- */
    for (int i = 0; i < head.num_entries; i++) {
        if (!entry_map[i].entry.props.has_blocks) continue;

        if (lseek(elf_fd, entry_map[i].phdr.p_offset, SEEK_SET) < 0) {
            write_log(g_log_path, "elf2fself: seek ELF segment failed: %s", strerror(errno));
            goto fail;
        }
        if (lseek(self_fd, entry_map[i].entry.offset, SEEK_SET) < 0) {
            write_log(g_log_path, "elf2fself: seek SELF segment failed: %s", strerror(errno));
            goto fail;
        }
        if (fs_ncopy(elf_fd, self_fd, entry_map[i].entry.enc_size)) {
            write_log(g_log_path, "elf2fself: copy segment failed: %s", strerror(errno));
            goto fail;
        }
    }

    if (version_seg.p_filesz > 0) {
        if (lseek(elf_fd, version_seg.p_offset, SEEK_SET) < 0) {
            write_log(g_log_path, "elf2fself: seek PT_SCE_VERSION failed: %s", strerror(errno));
            goto fail;
        }
        if (fs_ncopy(elf_fd, self_fd, version_seg.p_filesz)) {
            write_log(g_log_path, "elf2fself: copy PT_SCE_VERSION failed: %s", strerror(errno));
            goto fail;
        }
    }
	
    /* --- Success --- */
    free(entry_map);
    close(self_fd);
    close(elf_fd);
	
	const char *fname = get_fname(fself_path);

    write_log(g_log_path, "fself created: %s", fname);
    printf_notification("fself created: %s", fname);

    return 0;

fail:
    if (self_fd >= 0) close(self_fd);
    if (elf_fd  >= 0) close(elf_fd);
    free(entry_map);
    return -1;
}