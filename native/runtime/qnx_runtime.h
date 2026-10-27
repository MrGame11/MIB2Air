#ifndef WAA_QNX_RUNTIME_H
#define WAA_QNX_RUNTIME_H
#include <stdint.h>
#include "profile_generated.h"
#include "runtime_generated.h"
#define QR_EINTR 4
#define QR_EAGAIN 11
static uintptr_t qr_base;
static int qr_match(uintptr_t addr, const unsigned char *sig) {
    const volatile unsigned char *p = (const void *)addr;
    for (unsigned i = 0; i < 16; i++)
        if (p[i] != sig[i])
            return 0;
    return 1;
}
static int qr_init(void) {
    ((int (*)(int, void *, unsigned))QR_READ_PLT)(-1, 0, 0);
    uintptr_t a = *(volatile uintptr_t *)QR_READ_GOT;
    if (!a || !qr_match(a, qr_sig_read))
        return -1;
    uintptr_t base = a - QR_READ;
    if (!qr_match(base + QR_CLOCK_GETTIME, qr_sig_clock_gettime) ||
        !qr_match(base + QR_PTHREAD_SELF, qr_sig_pthread_self) ||
        !qr_match(base + QR___GET_ERRNO_PTR, qr_sig___get_errno_ptr) ||
        !qr_match(base + QR_MKFIFO, qr_sig_mkfifo))
        return -1;
    qr_base = base;
    return 0;
}
static unsigned qr_thread(void) { return ((unsigned (*)(void))(qr_base + QR_PTHREAD_SELF))(); }
static int qr_errno(void) { return *((int *(*)(void))(qr_base + QR___GET_ERRNO_PTR))(); }
static int qr_now(uint32_t *ms) {
    struct {
        uint32_t sec, nsec;
    } ts;
    if (((int (*)(int, void *))(qr_base + QR_CLOCK_GETTIME))(2, &ts) || ts.nsec >= 1000000000u)
        return -1;
    *ms = ts.sec * 1000u + ts.nsec / 1000000u;
    return 0;
}
static int qr_mkfifo(const char *p) {
    return ((int (*)(const char *, unsigned))(qr_base + QR_MKFIFO))(p, 0600);
}
static int qr_transient(int e) { return e == QR_EAGAIN || e == QR_EINTR; }
static int qr_open(const char *p, int flags) {
    return ((int (*)(const char *, int))QR_OPEN_PLT)(p, flags);
}
static int qr_read(int fd, void *p, unsigned n) {
    return ((int (*)(int, void *, unsigned))QR_READ_PLT)(fd, p, n);
}
static int qr_write(int fd, const void *p, unsigned n) {
    return ((int (*)(int, const void *, unsigned))QR_WRITE_PLT)(fd, p, n);
}
static void qr_close(int fd) {
    if (fd >= 0)
        ((int (*)(int))QR_CLOSE_PLT)(fd);
}
#endif
