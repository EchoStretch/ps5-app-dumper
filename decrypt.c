// decrypt.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

#include <ps5/kernel.h>

#include "sbl.h"
#include "authmgr.h"
#include "self.h"
#include "elf.h"

#ifdef LOG_TO_SOCKET
#define PC_IP   "10.0.0.35"
#define PC_PORT 9021
#endif

struct tailored_offsets
{
    uint64_t offset_dmpml4i;
    uint64_t offset_dmpdpi;
    uint64_t offset_pml4pml4i;
    uint64_t offset_mailbox_base;
    uint64_t offset_mailbox_flags;
    uint64_t offset_mailbox_meta;
    uint64_t offset_authmgr_handle;
    uint64_t offset_sbl_sxlock;
    uint64_t offset_sbl_mb_mtx;
    uint64_t offset_g_message_id;
    uint64_t offset_datacave_1;
    uint64_t offset_datacave_2;
};

uint64_t g_kernel_data_base;
char *g_bump_allocator_base;
char *g_bump_allocator_cur;
uint64_t g_bump_allocator_len;

char *g_dump_queue_buf = NULL;
int g_dump_queue_buf_pos = 0;
#define G_DUMP_QUEUE_BUF_SIZE 1 * 1024 * 1024 // 1MB

void *bump_alloc(uint64_t len)
{
    void *ptr;
    if (g_bump_allocator_cur + len >= (g_bump_allocator_base + g_bump_allocator_len)) {
        return NULL;
    }

    ptr = (void *) g_bump_allocator_cur;
    g_bump_allocator_cur += len;

    (void)memset(ptr, 0, len);
    return ptr;
}

void *bump_calloc(uint64_t count, uint64_t len)
{
    uint64_t total_len = count * len;
    return bump_alloc(total_len);
}

void bump_reset()
{
    g_bump_allocator_cur = g_bump_allocator_base;
}

static void _mkdir(const char *dir) {
    char tmp[PATH_MAX];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    mkdir(tmp, 0777);
}

uint64_t get_authmgr_sm(int sock, struct tailored_offsets *offsets)
{
    uint64_t authmgr_sm_handle;
    kernel_copyout(g_kernel_data_base + offsets->offset_authmgr_handle, &authmgr_sm_handle, sizeof(authmgr_sm_handle));
    return authmgr_sm_handle;
}

int self_verify_header(int sock, uint64_t authmgr_handle, char *data, uint64_t size, struct tailored_offsets *offsets)
{
    int err;
    uint64_t data_blob_va = g_kernel_data_base + offsets->offset_datacave_2;
    uint64_t data_blob_pa = pmap_kextract(sock, data_blob_va);

    kernel_copyin(data, data_blob_va, size);

    err = _sceSblAuthMgrSmFinalize(sock, authmgr_handle, 0);
    if (err != 0)
        return err;

    return _sceSblAuthMgrVerifyHeader(sock, authmgr_handle, data_blob_pa, size);
}

struct self_block_segment *self_decrypt_segment(
    int sock,
    int authmgr_handle,
    int service_id,
    char *file_data,
    struct sce_self_segment_header *segment,
    int segment_idx,
    struct tailored_offsets *offsets)
{
    int err;
    void *out_segment_data;
    void **digests;
    char *cur_digest;
    struct self_block_segment *segment_info;
    struct sce_self_block_info *cur_block_info;
    struct sce_self_block_info **block_infos;
    struct sbl_chunk_table_header *chunk_table;
    struct sbl_chunk_table_entry *chunk_entry;
    uint64_t chunk_table_va = g_kernel_data_base + offsets->offset_datacave_1;
    uint64_t data_blob_va   = g_kernel_data_base + offsets->offset_datacave_2;
    uint64_t chunk_table_pa = pmap_kextract(sock, chunk_table_va);
    uint64_t data_blob_pa   = pmap_kextract(sock, data_blob_va);
    char chunk_table_buf[0x1000] = {};

    struct sce_self_segment_header *target_segment;
    target_segment = (struct sce_self_segment_header *) (file_data +
                        sizeof(struct sce_self_header) + (SELF_SEGMENT_ID(segment) * sizeof(struct sce_self_segment_header)));

    if (segment->compressed_size < 0x1000)
        kernel_copyin(file_data + segment->offset, data_blob_va, segment->compressed_size);
    else {
        for (int bytes = 0; bytes < segment->compressed_size; bytes += 0x1000) {
            if (segment->compressed_size - bytes < 0x1000)
                kernel_copyin(file_data + segment->offset + bytes, data_blob_va + bytes, (segment->compressed_size - bytes));
            else
                kernel_copyin(file_data + segment->offset + bytes, data_blob_va + bytes, 0x1000);
        }
    }

    chunk_table = (struct sbl_chunk_table_header *) chunk_table_buf;
    chunk_entry = (struct sbl_chunk_table_entry *) (chunk_table_buf + sizeof(struct sbl_chunk_table_header));

    chunk_table->first_pa = data_blob_pa;
    chunk_table->used_entries = 1;
    chunk_table->data_size = segment->compressed_size;

    chunk_entry->pa = data_blob_pa;
    chunk_entry->size = segment->compressed_size;

    kernel_copyin(chunk_table, chunk_table_va, 0x30);

    for (int tries = 0; tries < 3; tries++) {
        err = _sceSblAuthMgrSmLoadSelfSegment(sock, authmgr_handle, service_id, chunk_table_pa, segment_idx);
        if (err == 0) break;
        sleep(1);
    }
    if (err != 0) return NULL;

    out_segment_data = mmap(NULL, segment->uncompressed_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (out_segment_data == NULL) return NULL;

    kernel_copyout(data_blob_va, out_segment_data, segment->uncompressed_size);

    segment_info = bump_alloc(sizeof(struct self_block_segment));
    if (segment_info == NULL) return NULL;

    segment_info->data = out_segment_data;
    segment_info->size = segment->uncompressed_size;

    if (SELF_SEGMENT_HAS_BLOCKINFO(segment)){
        if (SELF_SEGMENT_HAS_DIGESTS(segment)){
            segment_info->block_count = segment_info->size / (0x20 + 0x8);
        } else {
            segment_info->block_count = segment_info->size / 0x8;
        }
    } else {
        segment_info->block_count = ceil((double)target_segment->uncompressed_size / SELF_SEGMENT_BLOCK_SIZE(target_segment));
    }

    digests = bump_calloc(segment_info->block_count, sizeof(void *));
    if (digests == NULL) return NULL;

    if (SELF_SEGMENT_HAS_DIGESTS(segment)) {
        cur_digest = (char *) out_segment_data;
        for (int i = 0; i < segment_info->block_count; i++) {
            digests[i] = (void *) cur_digest;
            cur_digest += 0x20;
        }
    }
    segment_info->digests = digests;

    block_infos = bump_calloc(segment_info->block_count, sizeof(struct sce_self_block_info *));
    if (block_infos == NULL) return NULL;

    if (SELF_SEGMENT_HAS_BLOCKINFO(segment)) {
        cur_block_info = (struct sce_self_block_info *)out_segment_data;
        if (SELF_SEGMENT_HAS_DIGESTS(segment)) {
            cur_block_info = (struct sce_self_block_info *) (out_segment_data + (0x20 * segment_info->block_count));
        }
        for (int i = 0; i < segment_info->block_count; i++) {
            block_infos[i] = cur_block_info++;
        }
    } else {
        for (int i = 0; i < segment_info->block_count; i++) {
            block_infos[i] = bump_alloc(sizeof(struct sce_self_block_info));
            if (block_infos[i] == NULL) return NULL;
            block_infos[i]->offset = i * SELF_SEGMENT_BLOCK_SIZE(target_segment);
            if (i == segment_info->block_count - 1) {
                block_infos[i]->len = target_segment->uncompressed_size % SELF_SEGMENT_BLOCK_SIZE(target_segment);
            } else {
                block_infos[i]->len = SELF_SEGMENT_BLOCK_SIZE(target_segment);
            }
        }
    }

    segment_info->extents = block_infos;
    return segment_info;
}

void *self_decrypt_block(
    int sock,
    int authmgr_handle,
    int service_id,
    char *file_data,
    struct sce_self_segment_header *segment,
    int segment_idx,
    struct self_block_segment *block_segment,
    int block_idx,
    struct tailored_offsets *offsets)
{
    int err;
    uint64_t data_out_va  = g_kernel_data_base + offsets->offset_datacave_1;
    uint64_t data_blob_va = g_kernel_data_base + offsets->offset_datacave_2;
    uint64_t data_out_pa  = pmap_kextract(sock, data_out_va);
    uint64_t data_blob_pa = pmap_kextract(sock, data_blob_va);
    uint64_t input_addr = (uint64_t)(file_data + segment->offset + block_segment->extents[block_idx]->offset);
    void *out_block_data;

    for (int i = 0; i < 4; i++) {
        kernel_copyin((void *)(input_addr + (i * 0x1000)), data_blob_va + (i * 0x1000), 0x1000);
    }

    for (int tries = 0; tries < 5; tries++) {
        err = _sceSblAuthMgrSmLoadSelfBlock(
            sock, authmgr_handle, service_id, data_blob_pa, data_out_pa,
            segment, SELF_SEGMENT_ID(segment), block_segment, block_idx
        );
        if (err == 0) break;
        usleep(100000);
    }

    if (err != 0) {
        SOCK_LOG(sock, "[!] failed to decrypt block %d, err: %d\n", block_idx, err);
        return NULL;
    }

    out_block_data = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (out_block_data == NULL) return NULL;

    for (int i = 0; i < 4; i++) {
        kernel_copyout(data_out_va + (i * 0x1000), out_block_data + (i * 0x1000), 0x1000);
    }

    return out_block_data;
}

int decrypt_self(int sock, uint64_t authmgr_handle, char *path, int out_fd, struct tailored_offsets *offsets)
{
    int err = 0;
    int service_id;
    int self_file_fd;
    struct stat self_file_stat;
    void *self_file_data;
    void *out_file_data;
    struct elf64_hdr *elf_header;
    struct elf64_phdr *start_phdrs;
    struct elf64_phdr *cur_phdr;
    struct sce_self_header *header;
    struct sce_self_segment_header *segment;
    struct sce_self_segment_header *target_segment;
    struct self_block_segment **block_segments;
    struct self_block_segment *block_info;
    void **block_data;
    uint64_t tail_block_size;
    uint64_t final_file_size;

    self_file_fd = open(path, 0, 0);
    if (self_file_fd < 0) {
        SOCK_LOG(sock, "[!] failed to open %s\n", path);
        close(out_fd);
        return self_file_fd;
    }

    fstat(self_file_fd, &self_file_stat);
    self_file_data = mmap(NULL, self_file_stat.st_size, PROT_READ, MAP_SHARED, self_file_fd, 0);
    if (self_file_data == NULL || self_file_data == MAP_FAILED) {
        SOCK_LOG(sock, "[!] file mmap failed, falling back to reading the file\n");
        self_file_data = mmap(NULL, self_file_stat.st_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        size_t total_read = 0;
        while (total_read < self_file_stat.st_size) {
            size_t read_bytes = read(self_file_fd, self_file_data + total_read, self_file_stat.st_size - total_read);
            if (read_bytes <= 0) break;
            total_read += read_bytes;
        }
        SOCK_LOG(sock, "[+] read file into memory\n");
    }

    if (*(uint32_t *)self_file_data != SELF_PROSPERO_MAGIC) {
        SOCK_LOG(sock, "[!] %s is not a PS5 SELF file\n", path);
        err = -22;
        goto cleanup_in_file_data;
    }

    SOCK_LOG(sock, "[+] decrypting %s...\n", path);

    header = (struct sce_self_header *)self_file_data;
    service_id = self_verify_header(sock, authmgr_handle, self_file_data, header->header_size + header->metadata_size, offsets);
    if (service_id < 0) {
        SOCK_LOG(sock, "[!] failed to acquire a service ID\n");
        err = -1;
        goto cleanup_in_file_data;
    }

    elf_header  = (struct elf64_hdr *)(self_file_data + sizeof(struct sce_self_header) + (sizeof(struct sce_self_segment_header) * header->segment_count));
    start_phdrs = (struct elf64_phdr *)((char *)elf_header + sizeof(struct elf64_hdr));

    cur_phdr = start_phdrs;
    final_file_size = 0;
    for (int i = 0; i < elf_header->e_phnum; i++) {
        if (cur_phdr->p_type == PT_NOTE)
            final_file_size = cur_phdr->p_offset + cur_phdr->p_filesz;
        cur_phdr++;
    }

    if (final_file_size == 0) {
        SOCK_LOG(sock, "  [?] file segments are irregular, falling back on last LOAD segment\n");
        cur_phdr = start_phdrs;
        for (int i = 0; i < elf_header->e_phnum; i++) {
            if (cur_phdr->p_type == PT_LOAD)
                final_file_size = cur_phdr->p_offset + cur_phdr->p_filesz;
            cur_phdr++;
        }
    }

    out_file_data = mmap(NULL, final_file_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (out_file_data == NULL || (intptr_t)out_file_data == -1) {
        err = -12;
        goto cleanup_in_file_data;
    }

    memcpy(
        out_file_data + sizeof(struct elf64_hdr) + (elf_header->e_phnum * sizeof(struct elf64_phdr)),
        (char *) (start_phdrs) + (elf_header->e_phnum * sizeof(struct elf64_phdr)),
        0x40
    );

    memcpy(out_file_data, elf_header, sizeof(struct elf64_hdr));
    memcpy(out_file_data + sizeof(struct elf64_hdr), start_phdrs, elf_header->e_phnum * sizeof(struct elf64_phdr));

    memcpy(
        out_file_data + sizeof(struct elf64_hdr) + (elf_header->e_phnum * sizeof(struct elf64_phdr)),
        (char *) (start_phdrs) + (elf_header->e_phnum * sizeof(struct elf64_phdr)),
        0x40
    );

    block_segments = bump_calloc(header->segment_count, sizeof(struct self_block_segment *));
    if (block_segments == NULL) {
        err = -12;
        goto cleanup_out_file_data;
    }

    for (int i = 0; i < header->segment_count; i++) {
        segment = (struct sce_self_segment_header *)(self_file_data + sizeof(struct sce_self_header) + (i * sizeof(struct sce_self_segment_header)));
        if (SELF_SEGMENT_HAS_DIGESTS(segment)) {
            target_segment = (struct sce_self_segment_header *)(self_file_data + sizeof(struct sce_self_header) + (SELF_SEGMENT_ID(segment) * sizeof(struct sce_self_segment_header)));
            SOCK_LOG(sock, "  [?] decrypting block info segment for %lu\n", SELF_SEGMENT_ID(target_segment));
            block_segments[SELF_SEGMENT_ID(segment)] = self_decrypt_segment(sock, authmgr_handle, service_id, self_file_data, segment, SELF_SEGMENT_ID(target_segment), offsets);
            if (block_segments[SELF_SEGMENT_ID(segment)] == NULL) {
                SOCK_LOG(sock, "[!] failed to decrypt segment info for segment %lu\n", SELF_SEGMENT_ID(segment));
                err = -11;
                goto cleanup_out_file_data;
            }
        }
    }

    for (int i = 0; i < header->segment_count; i++) {
        segment = (struct sce_self_segment_header *)(self_file_data + sizeof(struct sce_self_header) + (i * sizeof(struct sce_self_segment_header)));
        if (!SELF_SEGMENT_HAS_BLOCKS(segment) || SELF_SEGMENT_HAS_DIGESTS(segment)) continue;

        cur_phdr = start_phdrs;
        for (int phnum = 0; phnum < header->segment_count; phnum++) {
            if (cur_phdr->p_filesz == segment->uncompressed_size) break;
            cur_phdr++;
        }

        block_info = block_segments[i];
        if (block_info == NULL) {
            SOCK_LOG(sock, "[!] we don't have block info for segment %d\n", i);
            continue;
        }

        block_data = bump_calloc(block_info->block_count, sizeof(void *));
        if (block_data == NULL) {
            err = -12;
            goto cleanup_out_file_data;
        }

        tail_block_size = segment->uncompressed_size % SELF_SEGMENT_BLOCK_SIZE(segment);

        for (int block = 0; block < block_info->block_count; block++) {
            SOCK_LOG(sock, "  [?] %s: decrypting segment=%d, block=%d/%lu\n", path, i, block + 1, block_info->block_count);
            block_data[block] = self_decrypt_block(sock, authmgr_handle, service_id, self_file_data, segment, i, block_info, block, offsets);
            if (block_data[block] == NULL) {
                SOCK_LOG(sock, "[!] failed to decrypt block %d\n", block);
                err = -11;
                goto cleanup_out_file_data;
            }

            void *out_addr = out_file_data + cur_phdr->p_offset + (block * SELF_SEGMENT_BLOCK_SIZE(segment));
            if (block == block_info->block_count - 1) {
                memcpy(out_addr, block_data[block], tail_block_size);
            } else {
                memcpy(out_addr, block_data[block], SELF_SEGMENT_BLOCK_SIZE(segment));
            }
            munmap(block_data[block], SELF_SEGMENT_BLOCK_SIZE(segment));
        }
    }

    SOCK_LOG(sock, "[?] writing decrypted SELF to file...\n");
    if (write(out_fd, out_file_data, final_file_size) != final_file_size) {
        SOCK_LOG(sock, "[!] failed to dump to file\n");
        err = -5;
    } else {
        SOCK_LOG(sock, "  [+] wrote 0x%08lx bytes...\n", final_file_size);
    }

cleanup_out_file_data:
    munmap(out_file_data, final_file_size);
cleanup_in_file_data:
    munmap(self_file_data, self_file_stat.st_size);
    close(self_file_fd);
    close(out_fd);
    bump_reset();
    return err;
}

int dump_queue_init(int sock)
{
    if (g_dump_queue_buf != NULL && g_dump_queue_buf != MAP_FAILED) return 0;
    g_dump_queue_buf = mmap(NULL, G_DUMP_QUEUE_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (g_dump_queue_buf == NULL || g_dump_queue_buf == MAP_FAILED) {
        SOCK_LOG(sock, "[!] failed to allocate buffer for directory entries\n");
        exit(-1);
    }
    return 0;
}

int dump_queue_reset()
{
    if (g_dump_queue_buf == NULL) return 0;
    g_dump_queue_buf_pos = 0;
    g_dump_queue_buf[0] = '\0';
    return 0;
}

int dump_queue_add_file(int sock, char *path)
{
    dump_queue_init(sock);

    static const char* allowed_exts[] = { ".elf", ".self", ".prx", ".sprx", ".bin" };
    static const int allowed_exts_count = sizeof(allowed_exts) / sizeof(allowed_exts[0]);

    int len = strlen(path);
   /* if (len >= 35 && strncmp(path, "/mnt/sandbox/pfsmnt/", 20) == 0 &&
        (strncmp(path + 29, "-app0/", 6) == 0 || strncmp(path + 29, "-patch0/", 8) == 0)) {
        return -1;
    } */

    char* dot = strrchr(path, '.');
    if (dot == NULL) return -2;

    int allowed = 0;
    for (int i = 0; i < allowed_exts_count; i++) {
        if (strcasecmp(dot, allowed_exts[i]) == 0) { allowed = 1; break; }
    }
    if (!allowed) return -3;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { SOCK_LOG(sock, "[!] failed to open file: %s\n", path); return -4; }

    uint32_t magic = 0;
    read(fd, &magic, sizeof(magic));
    close(fd);

    if (magic != SELF_PROSPERO_MAGIC) return -5;

    int new_pos = g_dump_queue_buf_pos + len + 1;
    if (new_pos >= G_DUMP_QUEUE_BUF_SIZE) {
        SOCK_LOG(sock, "[!] dump queue buffer full\n");
        exit(-2);
    }

    memcpy(g_dump_queue_buf + g_dump_queue_buf_pos, path, len + 1);
    g_dump_queue_buf_pos = new_pos;
    g_dump_queue_buf[g_dump_queue_buf_pos] = '\0';
    return 0;
}

#define DENTS_BUF_SIZE 0x10000
int dump_queue_add_dir(int sock, char* path, int recursive)
{
    int dir_fd = open(path, O_RDONLY, 0);
    if (dir_fd < 0) { SOCK_LOG(sock, "[!] failed to open directory: %s\n", path); return -1; }

    char* dents = malloc(DENTS_BUF_SIZE);
    while (1) {
        int n = getdents(dir_fd, dents, DENTS_BUF_SIZE);
        if (n <= 0) break;

        struct dirent* entry = (struct dirent*)dents;
        while ((char*)entry < dents + n) {
            if (entry->d_type == DT_REG) {
                char full_path[PATH_MAX];
                sprintf(full_path, "%s/%s", path, entry->d_name);
                dump_queue_add_file(sock, full_path);
            } else if (recursive && entry->d_type == DT_DIR && entry->d_name[0] != '.') {
                char full_path[PATH_MAX];
                sprintf(full_path, "%s/%s", path, entry->d_name);
                dump_queue_add_dir(sock, full_path, recursive);
            }
            entry = (struct dirent*)((char*)entry + entry->d_reclen);
        }
    }
    close(dir_fd);
    free(dents);
    return 0;
}

int dump(int sock, uint64_t authmgr_handle, struct tailored_offsets *offsets, const char *src_root, const char *out_dir_path)
{
    if (g_dump_queue_buf == NULL) return -1;

    int err = 0;
    int out_fd;
    char* entry;
    char out_file_path[PATH_MAX];
    struct stat out_file_stat;
    uint64_t spinlock_lock = 0x13371337;
    uint64_t spinlock_unlock = 0;

    uintptr_t sbl_sxlock_addr = g_kernel_data_base + offsets->offset_sbl_sxlock + 0x18;
    kernel_copyout(sbl_sxlock_addr, &spinlock_unlock, sizeof(spinlock_unlock));

    for (int i = 0; i < 0x100; i++) {
        kernel_copyin(&spinlock_lock, sbl_sxlock_addr, sizeof(spinlock_lock));
        usleep(1000);
    }

    entry = g_dump_queue_buf;
    while (*entry != '\0') {
        SOCK_LOG(sock, "[+] processing %s\n", entry);
        int entry_len = strlen(entry);

        /* map src_root -> out_dir_path if the entry starts with src_root */
        if (src_root != NULL && out_dir_path != NULL && strncmp(entry, src_root, strlen(src_root)) == 0) {
            /* ensure proper concatenation */
            const char *relative = entry + strlen(src_root);
            if (relative[0] == '/') relative++; /* drop leading slash */
            snprintf(out_file_path, sizeof(out_file_path), "%s/%s", out_dir_path, relative);
        } else {
            /* fallback: prefix entry with out_dir_path (legacy behaviour) */
            snprintf(out_file_path, sizeof(out_file_path), "%s%s", out_dir_path, entry);
        }

        /* Ensure parent directory exists */
        char parent_dir[PATH_MAX];
        char *last_slash_ptr = strrchr(out_file_path, '/');
        int last_slash = (last_slash_ptr != NULL) ? (last_slash_ptr - out_file_path) : -1;
        if (last_slash > 0) {
            strncpy(parent_dir, out_file_path, last_slash);
            parent_dir[last_slash] = '\0';
            _mkdir(parent_dir);
        }

        if (stat(out_file_path, &out_file_stat) == 0) {
            if (out_file_stat.st_size > 0) {
                SOCK_LOG(sock, "[*] %s already exists — overwriting\n", out_file_path);
                unlink(out_file_path);
            } else {
                unlink(out_file_path);
            }
        }

        out_fd = open(out_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            SOCK_LOG(sock, "[!] failed to open %s for writing, errno: %d\n", out_file_path, errno);
            entry += entry_len + 1;
            continue;
        }

        err = decrypt_self(sock, authmgr_handle, entry, out_fd, offsets);
        if (err == -11) {
            for (int attempt = 0; attempt < 2; attempt++) {
                /* reopen truncated for retry (decrypt_self closes the fd on return) */
                out_fd = open(out_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (out_fd < 0) {
                    SOCK_LOG(sock, "[!] failed to reopen %s for retry, errno: %d\n", out_file_path, errno);
                    break;
                }
                err = decrypt_self(sock, authmgr_handle, entry, out_fd, offsets);
                if (err == 0) break;
            }
        }

        if (err != 0) {
            unlink(out_file_path);
            SOCK_LOG(sock, "[!] failed to dump %s\n", entry);
        }

        if (err == -5) goto out;

        entry += entry_len + 1;
    }

    SOCK_LOG(sock, "[+] done\n");

out:
    kernel_copyin(&spinlock_unlock, sbl_sxlock_addr, sizeof(spinlock_unlock));
    return err;
}

int decrypt_all(const char *src_game, const char *dst_game)
{
    int sock = -1;
    uint64_t authmgr_handle;
    struct tailored_offsets offsets;
    int err = 0;

#ifdef LOG_TO_SOCKET
    struct sockaddr_in addr = {0};
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("[!] socket() failed\n");
        return -1;
    }
    inet_pton(AF_INET, PC_IP, &addr.sin_addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PC_PORT);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[!] connect() failed\n");
        close(sock);
        return -1;
    }
#endif

    g_bump_allocator_len = 0x100000;
    g_bump_allocator_base = mmap(NULL, g_bump_allocator_len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (g_bump_allocator_base == NULL) {
        printf("[!] failed to allocate backing space for bump allocator\n");
        goto out;
    }
    g_bump_allocator_cur = g_bump_allocator_base;
    g_kernel_data_base = KERNEL_ADDRESS_DATA_BASE;

    uint32_t version = kernel_get_fw_version() & 0xffff0000;
    printf("[+] firmware version 0x%x\n", version);

    switch (version) {
        case 0x3000000: case 0x3100000: case 0x3200000: case 0x3210000:
            offsets.offset_authmgr_handle = 0xC9EE50; offsets.offset_sbl_mb_mtx = 0x2712A98; offsets.offset_mailbox_base = 0x2712AA0; offsets.offset_sbl_sxlock = 0x2712AA8;
            offsets.offset_mailbox_flags = 0x2CF5F98; offsets.offset_mailbox_meta = 0x2CF5D38; offsets.offset_dmpml4i = 0x31BE4A0; offsets.offset_dmpdpi = 0x31BE4A4;
            offsets.offset_pml4pml4i = 0x31BE1FC; offsets.offset_g_message_id = 0x0008000; offsets.offset_datacave_1 = 0x8720000; offsets.offset_datacave_2 = 0x8724000;
            break;
        case 0x4000000: case 0x4020000: case 0x4030000: case 0x4500000: case 0x4510000:
            offsets.offset_authmgr_handle = 0xD0FBB0; offsets.offset_sbl_mb_mtx = 0x2792AB8; offsets.offset_mailbox_base = 0x2792AC0; offsets.offset_sbl_sxlock = 0x2792AC8;
            offsets.offset_mailbox_flags = 0x2D8DFC0; offsets.offset_mailbox_meta = 0x2D8DD60; offsets.offset_dmpml4i = 0x3257D00; offsets.offset_dmpdpi = 0x3257D04;
            offsets.offset_pml4pml4i = 0x3257A5C; offsets.offset_g_message_id = 0x0008000; offsets.offset_datacave_1 = 0x8720000; offsets.offset_datacave_2 = 0x8724000;
            break;
        case 0x5000000: case 0x5020000: case 0x5100000:
            offsets.offset_authmgr_handle = 0x0DFF410; offsets.offset_sbl_mb_mtx = 0x28C3038; offsets.offset_mailbox_base = 0x28C3040; offsets.offset_sbl_sxlock = 0x28C3048;
            offsets.offset_mailbox_flags = 0x2EADFC0; offsets.offset_mailbox_meta = 0x2EADD60; offsets.offset_dmpml4i = 0x3398D24; offsets.offset_dmpdpi = 0x3398D28;
            offsets.offset_pml4pml4i = 0x3397A2C; offsets.offset_g_message_id = 0x4270000; offsets.offset_datacave_1 = 0x8730000; offsets.offset_datacave_2 = 0x8734000;
            break;
        case 0x5500000:
            offsets.offset_authmgr_handle = 0x0DFF410; offsets.offset_sbl_mb_mtx = 0x28C3038; offsets.offset_mailbox_base = 0x28C3040; offsets.offset_sbl_sxlock = 0x28C3048;
            offsets.offset_mailbox_flags = 0x2EA9FC0; offsets.offset_mailbox_meta = 0x2EA9D60; offsets.offset_dmpml4i = 0x3394D24; offsets.offset_dmpdpi = 0x3394D28;
            offsets.offset_pml4pml4i = 0x3393A2C; offsets.offset_g_message_id = 0x4270000; offsets.offset_datacave_1 = 0x8730000; offsets.offset_datacave_2 = 0x8734000;
            break;
        case 0x6000000: case 0x6020000: case 0x6500000:
            offsets.offset_authmgr_handle = 0x0E1F8D0; offsets.offset_sbl_mb_mtx = 0x280F3A8; offsets.offset_mailbox_base = 0x280F3B0; offsets.offset_sbl_sxlock = 0x280F3B8;
            offsets.offset_mailbox_flags = 0x2DF9FC0; offsets.offset_mailbox_meta = 0x2DF9D60; offsets.offset_dmpml4i = 0x32E45F4; offsets.offset_dmpdpi = 0x32E45F8;
            offsets.offset_pml4pml4i = 0x32E32FC; offsets.offset_g_message_id = 0x4270000; offsets.offset_datacave_1 = 0x8730000; offsets.offset_datacave_2 = 0x8734000;
            break;
        case 0x7000000: case 0x7010000:
            offsets.offset_authmgr_handle = 0x0E20270; offsets.offset_sbl_mb_mtx = 0x27FF808; offsets.offset_mailbox_base = 0x27FF810; offsets.offset_sbl_sxlock = 0x27FF818;
            offsets.offset_mailbox_flags = 0x2CCDFC0; offsets.offset_mailbox_meta = 0x2CCDD60; offsets.offset_dmpml4i = 0x2E2CAE4; offsets.offset_dmpdpi = 0x2E2CAE8;
            offsets.offset_pml4pml4i = 0x2E2B79C; offsets.offset_g_message_id = 0x4270000; offsets.offset_datacave_1 = 0x5060000; offsets.offset_datacave_2 = 0x5064000;
            break;
        case 0x7200000: case 0x7400000: case 0x7600000: case 0x7610000:
            offsets.offset_authmgr_handle = 0x0E20330; offsets.offset_sbl_mb_mtx = 0x27FF808; offsets.offset_mailbox_base = 0x27FF810; offsets.offset_sbl_sxlock = 0x27FF818;
            offsets.offset_mailbox_flags = 0x2CCDFC0; offsets.offset_mailbox_meta = 0x2CCDD60; offsets.offset_dmpml4i = 0x2E2CAE4; offsets.offset_dmpdpi = 0x2E2CAE8;
            offsets.offset_pml4pml4i = 0x2E2B79C; offsets.offset_g_message_id = 0x4270000; offsets.offset_datacave_1 = 0x5060000; offsets.offset_datacave_2 = 0x5064000;
            break;
        case 0x8000000: case 0x8200000: case 0x8400000: case 0x8600000:
            offsets.offset_authmgr_handle = 0x0E203C0; offsets.offset_sbl_mb_mtx = 0x27FF888; offsets.offset_mailbox_base = 0x27FF890; offsets.offset_sbl_sxlock = 0x27FF898;
            offsets.offset_mailbox_flags = 0x2CEA820; offsets.offset_mailbox_meta = 0x2CEA5C0; offsets.offset_dmpml4i = 0x2E48AE4; offsets.offset_dmpdpi = 0x2E48AE8;
            offsets.offset_pml4pml4i = 0x2E4779C; offsets.offset_g_message_id = 0x4270000; offsets.offset_datacave_1 = 0x5060000; offsets.offset_datacave_2 = 0x5064000;
            break;
        case 0x9000000: case 0x9200000: case 0x9400000: case 0x9600000:
            offsets.offset_authmgr_handle = 0xDB8D60; offsets.offset_sbl_mb_mtx = 0x26E71F8; offsets.offset_mailbox_base = 0x26E7200; offsets.offset_sbl_sxlock = 0x26E7208;
            offsets.offset_mailbox_flags = 0x2BCA860; offsets.offset_mailbox_meta = 0x2BCA600; offsets.offset_dmpml4i = 0x2D28E14; offsets.offset_dmpdpi = 0x2D28E18;
            offsets.offset_pml4pml4i = 0x2D279CC; offsets.offset_g_message_id = 0x4270000; offsets.offset_datacave_1 = 0x5060000; offsets.offset_datacave_2 = 0x5064000;
            break;
        case 0x10000000: case 0x10010000: case 0x10200000: case 0x10400000: case 0x10600000:
            offsets.offset_authmgr_handle = 0x0DB8DF0; offsets.offset_sbl_mb_mtx = 0x26F71F8; offsets.offset_mailbox_base = 0x26F7200; offsets.offset_sbl_sxlock = 0x26F7208;
            offsets.offset_mailbox_flags = 0x2BEE860; offsets.offset_mailbox_meta = 0x2BEE600; offsets.offset_dmpml4i = 0x2CF1194; offsets.offset_dmpdpi = 0x2CF1198;
            offsets.offset_pml4pml4i = 0x2CEFD4C; offsets.offset_g_message_id = 0x4270000; offsets.offset_datacave_1 = 0x5060000; offsets.offset_datacave_2 = 0x5064000;
            break;
        default:
            printf("[!] unsupported firmware, dumping then bailing!\n");
            char *dump_buf = mmap(NULL, 0x7800 * 0x1000, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
            for (int pg = 0; pg < 0x7800; pg++) {
                kernel_copyout(g_kernel_data_base + (pg * 0x1000), dump_buf + (pg * 0x1000), 0x1000);
            }
            int dump_fd = open("/mnt/usb0/PS5/data_dump.bin", O_WRONLY | O_CREAT, 0644);
            write(dump_fd, dump_buf, 0x7800 * 0x1000);
            close(dump_fd);
            printf("  [+] dumped\n");
            goto out;
    }

    init_sbl(g_kernel_data_base, offsets.offset_dmpml4i, offsets.offset_dmpdpi, offsets.offset_pml4pml4i,
             offsets.offset_mailbox_base, offsets.offset_mailbox_flags, offsets.offset_mailbox_meta,
             offsets.offset_sbl_mb_mtx, offsets.offset_g_message_id);

    authmgr_handle = get_authmgr_sm(sock, &offsets);
    printf("[+] got auth manager: %lu\n", authmgr_handle);

    dump_queue_reset();
    dump_queue_add_dir(sock, (char *)src_game, 1);

    err = dump(sock, authmgr_handle, &offsets, src_game, dst_game);

out:
#ifdef LOG_TO_SOCKET
    if (sock >= 0) close(sock);
#endif
    return err;
}
