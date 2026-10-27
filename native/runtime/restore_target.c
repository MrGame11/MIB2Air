/* A receipt belongs to this recovery attempt and survives additional reboots. */
#include "qnx_runtime.h"
static int restore_begin(void);
void restore_result(void *, uint32_t, uint32_t, const void *);
#define WAA_RESTORE_BEGIN restore_begin
#define WAA_UUID_CALLBACK restore_result
#include "../waa-ext/waa_ext.c"

static char restore_token[33];
static int restore_fd = -1;

static int restore_begin(void) {
    if (qr_init() || !qr_match(qr_base + QR_FSYNC, qr_sig_fsync))
        return -1;
    int fd = qr_open("/tsd/var/wireless-aa/run/restore-token", 0);
    if (fd < 0)
        return -1;
    unsigned used = 0;
    while (used < sizeof(restore_token)) {
        int n = qr_read(fd, restore_token + used, sizeof(restore_token) - used);
        if (n <= 0) {
            qr_close(fd);
            return -1;
        }
        used += (unsigned)n;
    }
    char extra;
    int end = qr_read(fd, &extra, 1);
    qr_close(fd);
    if (end || restore_token[32] != '\n')
        return -1;
    for (unsigned i = 0; i < 32; i++)
        if (!((restore_token[i] >= '0' && restore_token[i] <= '9') ||
              (restore_token[i] >= 'a' && restore_token[i] <= 'f')))
            return -1;
    fd = qr_open("/tsd/var/wireless-aa/run/restore-result", 2);
    if (fd < 0)
        return -1;
    if (qr_read(fd, &extra, 1) != 0) {
        qr_close(fd);
        return -1;
    }
    restore_fd = fd;
    return 0;
}

void restore_result(void *ctx, uint32_t opcode, uint32_t count, const void *node) {
    if (opcode != 0x6205 || count != 2 || !node || PA_SIZE(node, 0) != 1 || PA_SIZE(node, 1) != 1 ||
        !PA_PTR(node, 0) || !PA_PTR(node, 1))
        return;
    waa_uuid_result_cb(ctx, opcode, count, node);
    if (!g_waa_oneshot || restore_fd < 0 || *PA_PTR(node, 0))
        return;
    int fd = restore_fd;
    restore_fd = -1;
    if (*PA_PTR(node, 1)) {
        qr_close(fd);
        return;
    }
    unsigned done = 0;
    while (done < sizeof(restore_token)) {
        int n = qr_write(fd, restore_token + done, sizeof(restore_token) - done);
        if (n <= 0)
            break;
        done += (unsigned)n;
    }
    ((int (*)(int))(qr_base + QR_FSYNC))(fd);
    qr_close(fd);
}
