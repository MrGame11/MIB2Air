#ifndef WAA_WIFI_PEER_H
#define WAA_WIFI_PEER_H
#include <stdint.h>
/* Target routing ABI: CTL_NET/PF_ROUTE/AF_INET/NET_RT_FLAGS with RTF_LLINFO
 * uses {4,17,0,2,2,1024}, a 76-byte rt_msghdr and 4-byte sockaddr alignment.
 * Unknown or incomplete records remain passive. */
static unsigned wifi_u16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t wifi_u32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int wifi_equal(const uint8_t *a, const uint8_t *b, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static int wifi_arp_peer(const uint8_t *b, unsigned n, uint32_t ip, unsigned index,
                         uint8_t mac[6]) {
    unsigned found = 0;
    uint8_t result[6];
    while (n) {
        if (n < 76)
            return -1;
        unsigned size = wifi_u16(b);
        if (size < 76 || size > n)
            return -1;
        if ((wifi_u32(b + 8) & 1024u) && (wifi_u32(b + 12) & 3u) == 3u) {
            const uint8_t *dst = b + 76;
            if (size < 92 || dst[0] < 8 || dst[0] > size - 76)
                return -1;
            unsigned off = 76 + ((dst[0] + 3u) & ~3u);
            if (off + 8 > size)
                return -1;
            const uint8_t *link = b + off;
            unsigned len = link[0], name = link[5];
            if (len < 8 || len > size - off || 8u + name + link[6] > len)
                return -1;
            if (dst[1] == 2 && wifi_u32(dst + 4) == ip && link[1] == 18 &&
                wifi_u16(link + 2) == index && link[6] == 6) {
                const uint8_t *address = link + 8 + name;
                static const uint8_t zero[6] = {0};
                if ((address[0] & 1) || wifi_equal(address, zero, 6) || found)
                    return -1;
                for (unsigned i = 0; i < 6; i++)
                    result[i] = address[i];
                found = 1;
            }
        }
        b += size;
        n -= size;
    }
    if (!found)
        return -1;
    for (unsigned i = 0; i < 6; i++)
        mac[i] = result[i];
    return 0;
}
static int wifi_station_down(const uint8_t *b, unsigned n, unsigned prefix, const uint8_t mac[6]) {
    unsigned o = prefix ? 3 : 0;
    if (!b || prefix > 1 || n != o + 19)
        return 0;
    if (prefix && (b[0] != 19 || b[1] != 0))
        return 0;
    /* Disconnected status 1, station MAC at payload +3. The peer is pinned
     * to wlan1 by the ARP lookup; HIF interface IDs are a separate namespace. */
    return b[o] == 2 && b[o + 1] == 0xa6 && b[o + 2] == 15 && b[o + 3] == 0 && b[o + 6] == 1 &&
           wifi_equal(b + o + 7, mac, 6);
}
#endif
