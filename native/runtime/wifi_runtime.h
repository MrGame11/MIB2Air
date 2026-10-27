#ifndef WAA_WIFI_RUNTIME_H
#define WAA_WIFI_RUNTIME_H
static void *wifi_symbols(void) {
    if (!qr_match(qr_base + QR_DLOPEN, qr_sig_dlopen) ||
        !qr_match(qr_base + QR_DLSYM, qr_sig_dlsym) ||
        !qr_match(qr_base + QR_DLCLOSE, qr_sig_dlclose))
        return 0;
    return ((void *(*)(const char *, int))(qr_base + QR_DLOPEN))(0, 2);
}
static void *wifi_symbol(void *h, const char *name) {
    return ((void *(*)(void *, const char *))(qr_base + QR_DLSYM))(h, name);
}
static void wifi_symbols_close(void *h) { ((int (*)(void *))(qr_base + QR_DLCLOSE))(h); }
#ifdef WAA_ROLE_WA
static int wifi_resolve_peer(uint32_t ip, uint8_t mac[6]) {
    static uint8_t routes[8192];
    void *h = wifi_symbols();
    if (!h)
        return -1;
    int (*query)(const int *, unsigned, void *, unsigned *, const void *, unsigned) =
        wifi_symbol(h, "sysctl");
    unsigned (*ifindex)(const char *) = wifi_symbol(h, "if_nametoindex");
    int result = -1;
    unsigned index = ifindex ? ifindex("wlan1") : 0;
    unsigned size = sizeof(routes);
    const int mib[6] = {4, 17, 0, 2, 2, 1024};
    if (query && index && !query(mib, 6, routes, &size, 0, 0) && size <= sizeof(routes))
        result = wifi_arp_peer(routes, size, ip, index, mac);
    wifi_symbols_close(h);
    return result;
}
#else
static uint32_t wifi_socket_peer(int fd) {
    void *h = wifi_symbols();
    if (!h)
        return 0;
    int (*peer)(int, void *, unsigned *) = wifi_symbol(h, "getpeername");
    uint8_t address[16] = {0};
    unsigned size = sizeof(address);
    uint32_t ip = 0;
    if (peer && !peer(fd, address, &size) && size == sizeof(address) && address[1] == 2)
        ip = wifi_u32(address + 4);
    wifi_symbols_close(h);
    return ip;
}
#endif
#endif
