#include <ps5/kernel.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "sbl.h"

void sock_print(int sock, char *str) { printf("%s", str); }

struct sbl_mailbox_metadata {
    uint64_t message_id;
    uint64_t unk_08h;
    uint32_t unk_10h;
};

uint64_t g_sbl_kernel_data_base;
uint64_t g_sbl_dmap_base;
uint64_t g_sbl_kernel_offset_dmpml4i;
uint64_t g_sbl_kernel_offset_dmpdpi;
uint64_t g_sbl_kernel_offset_pml4pml4i;
uint64_t g_sbl_kernel_offset_mailbox_base;
uint64_t g_sbl_kernel_offset_mailbox_flags;
uint64_t g_sbl_kernel_offset_mailbox_meta;
uint64_t g_sbl_kernel_offset_mailbox_mtx;
uint64_t g_sbl_kernel_offset_g_message_id;

static int g_retry_backoff = 0;

#if DEBUG
static void DumpHex(int sock, const void *data, size_t size)
{
    char hexbuf[0x4000] = {0};
    char *cur = hexbuf;
    sprintf(cur, "hex:\n"); cur += strlen(cur);
    char ascii[17]; ascii[16] = '\0';
    for (size_t i = 0; i < size; ++i) {
        sprintf(cur, "%02X ", ((unsigned char *)data)[i]); cur += 3;
        ascii[i % 16] = (((unsigned char *)data)[i] >= ' ' &&
                         ((unsigned char *)data)[i] <= '~')
                        ? ((unsigned char *)data)[i] : '.';
        if ((i+1) % 8 == 0 || i+1 == size) {
            sprintf(cur, " "); cur++;
            if ((i+1) % 16 == 0) {
                sprintf(cur, "|  %s \n", ascii); cur += strlen(cur);
            } else if (i+1 == size) {
                ascii[(i+1) % 16] = '\0';
                if ((i+1) % 16 <= 8) { sprintf(cur, " "); cur++; }
                for (size_t j = (i+1) % 16; j < 16; ++j) { sprintf(cur, "   "); cur += 3; }
                sprintf(cur, "|  %s \n", ascii); cur += strlen(cur);
            }
        }
    }
    printf("%s", hexbuf);
}
#endif

void init_sbl(uint64_t kernel_data_base,
              uint64_t dmpml4i_offset, uint64_t dmpdpi_offset,
              uint64_t pml4pml4i_offset,
              uint64_t mailbox_base_offset, uint64_t mailbox_flags_offset,
              uint64_t mailbox_meta_offset,
              uint64_t mailbox_mtx_offset, uint64_t g_message_id_offset)
{
    uint64_t DMPML4I = 0, DMPDPI = 0, PML4PML4I = 0;

    g_sbl_kernel_data_base            = kernel_data_base;
    g_sbl_kernel_offset_dmpml4i       = dmpml4i_offset;
    g_sbl_kernel_offset_dmpdpi        = dmpdpi_offset;
    g_sbl_kernel_offset_pml4pml4i     = pml4pml4i_offset;
    g_sbl_kernel_offset_mailbox_base  = mailbox_base_offset;
    g_sbl_kernel_offset_mailbox_flags = mailbox_flags_offset;
    g_sbl_kernel_offset_mailbox_meta  = mailbox_meta_offset;
    g_sbl_kernel_offset_mailbox_mtx   = mailbox_mtx_offset;
    g_sbl_kernel_offset_g_message_id  = g_message_id_offset;

    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_dmpml4i,
                   &DMPML4I, sizeof(int));
    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_dmpdpi,
                   &DMPDPI, sizeof(int));
    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_pml4pml4i,
                   &PML4PML4I, sizeof(int));

    g_sbl_dmap_base = (DMPDPI << 30) | (DMPML4I << 39) | 0xFFFF800000000000;
}

int _sceSblServiceRequest(int sock, struct sbl_msg_header *msg_header,
                          void *in_buf, void *out_buf, int request_type)
{
    switch (request_type) {
    case 2: return sceSblDriverSendMsgAnytime(sock, msg_header, in_buf, out_buf);
    case 1: return sceSblDriverSendMsgPol(sock, msg_header, in_buf, out_buf);
    case 0: return sceSblServiceRequest(sock, msg_header, in_buf, out_buf);
    }
    return -37;
}

#define MAILBOX_NUM 0xE

int sceSblServiceRequest(int sock, struct sbl_msg_header *msg_header,
                         void *in_buf, void *out_buf)
{
    int err;
    uint32_t mailbox_to_bitmap;
    uint64_t message_id;
    struct sbl_mailbox_metadata mailbox_metadata;
    uint64_t mailbox_base, mailbox_addr;

    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_mailbox_base,
                   &mailbox_base, sizeof(mailbox_base));
    mailbox_addr = mailbox_base + (0x800 * (0x10 + MAILBOX_NUM));

    int res_before = -69;
    kernel_copyin(&res_before, mailbox_addr + 0x18 + 0x4, sizeof(res_before));

#if DEBUG
    printf("sceSblServiceRequest: mailbox = %p\n", mailbox_addr);
#endif

    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_g_message_id,
                   &message_id, sizeof(message_id));
    if (message_id == 0) message_id = 0x414100;
    msg_header->message_id = message_id++;
    kernel_copyin(&message_id,
                  g_sbl_kernel_data_base + g_sbl_kernel_offset_g_message_id,
                  sizeof(message_id));

    mailbox_metadata.message_id = msg_header->message_id;
    mailbox_metadata.unk_08h    = 0;
    mailbox_metadata.unk_10h    = 0;
    kernel_copyin(&mailbox_metadata,
                  g_sbl_kernel_data_base +
                  g_sbl_kernel_offset_mailbox_meta +
                  (MAILBOX_NUM * 0x28),
                  sizeof(struct sbl_mailbox_metadata));

    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_mailbox_flags,
                   &mailbox_to_bitmap, sizeof(mailbox_to_bitmap));
    mailbox_to_bitmap |= (1 << MAILBOX_NUM);
    kernel_copyin(&mailbox_to_bitmap,
                  g_sbl_kernel_data_base + g_sbl_kernel_offset_mailbox_flags,
                  sizeof(mailbox_to_bitmap));

    while (1) {
        err = sceSblDriverSendMsg(sock, msg_header, in_buf);
        if (err != -11) break;
        usleep(10);
    }

    if (err != 0) {
        int delay_ms = 100 << g_retry_backoff;
        if (delay_ms > 1600) delay_ms = 1600;

        printf("sceSblServiceRequest: send failed (%d), retry in %dms...\n",
               err, delay_ms);
        usleep(delay_ms * 1000);
        g_retry_backoff = (g_retry_backoff < 4) ? g_retry_backoff + 1 : 4;

        /* unlock mailbox */
        kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_mailbox_flags,
                       &mailbox_to_bitmap, sizeof(mailbox_to_bitmap));
        mailbox_to_bitmap &= ~(1 << MAILBOX_NUM);
        kernel_copyin(&mailbox_to_bitmap,
                      g_sbl_kernel_data_base + g_sbl_kernel_offset_mailbox_flags,
                      sizeof(mailbox_to_bitmap));

#if DEBUG
        DumpHex(sock, msg_header, sizeof(*msg_header));
        DumpHex(sock, in_buf, 0x80);
#endif
        return -1;
    }

    g_retry_backoff = 0;

#if DEBUG
    printf("sceSblServiceRequest: send OK\n");
#endif

    int res_after = res_before;
    for (int i = 0; i < 500; ++i) {
        kernel_copyout(mailbox_addr + 0x18 + 0x4, &res_after, sizeof(res_after));
        if (res_after != res_before) break;
        usleep(1000);
    }
    usleep(2000);

    if (res_after == res_before) {
        printf("sceSblServiceRequest: timeout waiting for response\n");
        return -1;
    }

    kernel_copyout(mailbox_addr + 0x18, out_buf, msg_header->recv_len);

    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_mailbox_flags,
                   &mailbox_to_bitmap, sizeof(mailbox_to_bitmap));
    mailbox_to_bitmap &= ~(1 << MAILBOX_NUM);
    kernel_copyin(&mailbox_to_bitmap,
                  g_sbl_kernel_data_base + g_sbl_kernel_offset_mailbox_flags,
                  sizeof(mailbox_to_bitmap));

    return 0;
}

int sceSblDriverSendMsgAnytime(int sock,
                               struct sbl_msg_header *msg_header,
                               void *in_buf, void *out_buf)
{
    (void)sock; (void)msg_header; (void)in_buf; (void)out_buf;
    return -1;
}

int sceSblDriverSendMsgPol(int sock,
                           struct sbl_msg_header *msg_header,
                           void *in_buf, void *out_buf)
{
    (void)sock; (void)msg_header; (void)in_buf; (void)out_buf;
    return -1;
}

uint64_t pmap_kextract(int sock, uint64_t va)
{
    uint64_t DMPML4I = 0, DMPDPI = 0, PML4PML4I = 0;
    uint64_t dmap, dmap_end, pde_addr, pte_addr, pde, pte;

    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_dmpml4i,
                   &DMPML4I, sizeof(int));
    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_dmpdpi,
                   &DMPDPI, sizeof(int));
    kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_pml4pml4i,
                   &PML4PML4I, sizeof(int));

    dmap     = (DMPDPI << 30) | (DMPML4I << 39) | 0xFFFF800000000000;
    dmap_end = ((DMPML4I + 1) << 39) | 0xFFFF800000000000;

    if (dmap <= va && dmap_end > va) return va - dmap;

    pde_addr = ((PML4PML4I << 39) | (PML4PML4I << 30) |
                0xFFFF800000000000) + 8 * ((va >> 21) & 0x7FFFFFF);
    kernel_copyout(pde_addr, &pde, sizeof(pde));
    if (pde & 0x80)
        return (pde & 0xFFFFFFFE00000) | (va & 0x1FFFFF);

    pte_addr = ((va >> 9) & 0xFE0) + dmap + (pde & 0xFFFFFFFFFF000);
    kernel_copyout(pte_addr, &pte, sizeof(pte));
    return (pte & 0xFFFFFFFFFF000) | (va & 0x3FFF);
}

int sceSblDriverSendMsg(int sock, struct sbl_msg_header *msg_header,
                        void *in_buf)
{
    uint64_t mmio_space = g_sbl_dmap_base + 0xE0500000;
    static uint64_t mailbox_base = 0;
    uint64_t mailbox_addr;
    static uint64_t mailbox_pa = 0;
    uint32_t cmd, status;

    if (mailbox_base == 0) {
        kernel_copyout(g_sbl_kernel_data_base + g_sbl_kernel_offset_mailbox_base,
                       &mailbox_base, sizeof(mailbox_base));
    }
    mailbox_addr = mailbox_base + (0x800 * (0x10 + MAILBOX_NUM));

    kernel_copyin(msg_header, mailbox_addr, sizeof(*msg_header));
    kernel_copyin(in_buf, mailbox_addr + 0x18, msg_header->query_len);

    if (mailbox_pa == 0)
        mailbox_pa = pmap_kextract(sock, mailbox_addr);

    cmd = msg_header->cmd << 8;
    kernel_copyin(&mailbox_pa, mmio_space + 0x10568, sizeof(int));
    kernel_copyin(&cmd, mmio_space + 0x10564, sizeof(int));

    do {
        kernel_copyout(mmio_space + 0x10564, &status, sizeof(status));
        if (status & 1) break;
        usleep(1000);
    } while (1);

#if DEBUG
    printf("sceSblDriverSendMsg: status = 0x%08x\n", status);
#endif

    return (int)((status << 0x1E) >> 0x1F) & 0xfffffffb;
}