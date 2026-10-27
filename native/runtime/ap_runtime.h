#ifndef WAA_AP_RUNTIME_H
#define WAA_AP_RUNTIME_H
#include "ap_config.h"
#define AP_MODE_PATH "/tsd/var/networking/wlanlastmode.conf"
#define AP_XML_PATH "/tsd/var/networking/wlanapconfig.xml"
static char ap_document[2048], ap_confirm[2048];
static void ap_erase(void *data, unsigned length) {
    volatile unsigned char *p = data;
    while (length--)
        *p++ = 0;
}
static int ap_read_file(const char *path, char *out, unsigned capacity) {
    int fd = ((int (*)(const char *, int))QR_OPEN_PLT)(path, 0);
    if (fd < 0)
        return -1;
    unsigned used = 0, interruptions = 0;
    int result = -1;
    while (used < capacity) {
        int n = qr_read(fd, out + used, capacity - used);
        if (n < 0) {
            if (qr_errno() == 4 && ++interruptions < 4)
                continue;
            break;
        }
        if (!n) {
            result = (int)used;
            break;
        }
        if ((unsigned)n > capacity - used)
            break;
        used += (unsigned)n;
    }
    qr_close(fd);
    return result;
}
static int ap_enabled(void) {
    char mode[128];
    int n = ap_read_file(AP_MODE_PATH, mode, sizeof(mode));
    return n > 0 && ap_mode_enabled(mode, (unsigned)n);
}
static int ap_refresh(struct aa_wifi_cfg *out) {
    struct aa_wifi_cfg cfg = {0};
    char mode[128], confirm[128];
    struct ap_ifaddrs *list = 0;
    int result = -1;
    void *handle = 0;
    void (*release)(struct ap_ifaddrs *) = 0;
    if (!qr_match(qr_base + QR_DLOPEN, qr_sig_dlopen) ||
        !qr_match(qr_base + QR_DLSYM, qr_sig_dlsym) ||
        !qr_match(qr_base + QR_DLCLOSE, qr_sig_dlclose))
        goto done;
    int mode_length = ap_read_file(AP_MODE_PATH, mode, sizeof(mode));
    if (mode_length <= 0 || !ap_mode_enabled(mode, (unsigned)mode_length))
        goto done;
    int n = ap_read_file(AP_XML_PATH, ap_document, sizeof(ap_document));
    if (n <= 0 || ap_xml(ap_document, (unsigned)n, &cfg))
        goto done;
    /* RTLD_NOW=2 in the supported QNX runtime. NULL accesses dependencies
     * already loaded by WA, including libsocket.so.3; no additional library. */
    handle = ((void *(*)(const char *, int))(qr_base + QR_DLOPEN))(0, 2);
    if (!handle)
        goto done;
    void *(*symbol)(void *, const char *) = (void *)(qr_base + QR_DLSYM);
    int (*get)(struct ap_ifaddrs **) = symbol(handle, "getifaddrs");
    release = symbol(handle, "freeifaddrs");
    if (!get || !release || get(&list) || ap_interface(list, &cfg))
        goto done;
    /* Reject an XML rewrite while collecting interface state. */
    int m = ap_read_file(AP_XML_PATH, ap_confirm, sizeof(ap_confirm));
    if (m != n)
        goto done;
    for (int i = 0; i < n; i++)
        if (ap_document[i] != ap_confirm[i])
            goto done;
    int confirmed = ap_read_file(AP_MODE_PATH, confirm, sizeof(confirm));
    if (confirmed != mode_length)
        goto done;
    for (int i = 0; i < mode_length; i++)
        if (mode[i] != confirm[i])
            goto done;
    *out = cfg;
    result = 0;
done:
    if (list && release)
        release(list);
    if (handle)
        ((int (*)(void *))(qr_base + QR_DLCLOSE))(handle);
    ap_erase(&cfg, sizeof(cfg));
    ap_erase(ap_document, sizeof(ap_document));
    ap_erase(ap_confirm, sizeof(ap_confirm));
    if (result)
        ap_erase(out, sizeof(*out));
    return result;
}
#endif
