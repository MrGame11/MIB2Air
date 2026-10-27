#ifndef WAA_AP_CONFIG_H
#define WAA_AP_CONFIG_H
#include "../waa-spp/aa_boot_sm.h"

/* Bounded parser for the native WlanApConfig schema. No DTD, attributes or
 * external entities; XML entities are decoded before enforcing Wi-Fi limits. */
static int ap_space(unsigned c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int ap_take(const char **p, const char *end, const char *s) {
    const char *q = *p;
    while (*s) {
        if (q == end || *q++ != *s++)
            return 0;
    }
    *p = q;
    return 1;
}
static void ap_skip(const char **p, const char *end) {
    while (*p < end && ap_space((unsigned char)**p))
        ++*p;
}
static int ap_utf8(const uint8_t *s, unsigned n) {
    unsigned i = 0;
    while (i < n) {
        unsigned c = s[i++], need, low;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f)
                return -1;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            low = 0x80;
            c &= 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            low = 0x800;
            c &= 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            low = 0x10000;
            c &= 7;
        } else
            return -1;
        if (need > n - i)
            return -1;
        while (need--) {
            if ((s[i] & 0xc0) != 0x80)
                return -1;
            c = (c << 6) | (s[i++] & 63);
        }
        if (c < low || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || c == 0xfffe || c == 0xffff)
            return -1;
    }
    return 0;
}
static int ap_text(const char **p, const char *end, uint8_t *out, unsigned *length) {
    unsigned n = 0;
    while (*p < end && **p != '<') {
        unsigned c = (unsigned char)*(*p)++;
        if (c == '&') {
            if (ap_take(p, end, "amp;"))
                c = '&';
            else if (ap_take(p, end, "lt;"))
                c = '<';
            else if (ap_take(p, end, "gt;"))
                c = '>';
            else if (ap_take(p, end, "quot;"))
                c = '"';
            else if (ap_take(p, end, "apos;"))
                c = '\'';
            else if (ap_take(p, end, "#")) {
                unsigned base = ap_take(p, end, "x") ? 16 : 10, digits = 0;
                c = 0;
                while (*p < end && **p != ';') {
                    unsigned ch = (unsigned char)*(*p)++, v;
                    if (ch >= '0' && ch <= '9')
                        v = ch - '0';
                    else if (ch >= 'a' && ch <= 'f')
                        v = ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F')
                        v = ch - 'A' + 10;
                    else
                        return -1;
                    if (v >= base || c > 0x10ffffu)
                        return -1;
                    c = c * base + v;
                    digits++;
                }
                if (!digits || !ap_take(p, end, ";") || c > 0x10ffffu || c < 0x20 || c == 0x7f ||
                    (c >= 0xd800 && c <= 0xdfff) || c == 0xfffe || c == 0xffff)
                    return -1;
                if (c >= 0x80) {
                    unsigned count = c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
                    if (n + count >= AA_CFG_MAX)
                        return -1;
                    unsigned first = count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0;
                    for (unsigned j = count - 1; j; j--) {
                        out[n + j] = 0x80 | (c & 63);
                        c >>= 6;
                    }
                    out[n] = first | c;
                    n += count;
                    continue;
                }
            } else
                return -1;
        }
        if (!c || n + 1 >= AA_CFG_MAX)
            return -1;
        out[n++] = (uint8_t)c;
    }
    if (ap_utf8(out, n))
        return -1;
    out[n] = 0;
    *length = n;
    return 0;
}
static int ap_equal(const uint8_t *v, unsigned n, const char *s) {
    unsigned i = 0;
    while (s[i]) {
        if (i == n || v[i] != (uint8_t)s[i])
            return 0;
        i++;
    }
    return i == n;
}
static int ap_xml(const char *xml, unsigned length, struct aa_wifi_cfg *cfg) {
    const char *p = xml, *end = xml + length;
    unsigned seen = 0;
    ap_take(&p, end, "\xef\xbb\xbf");
    ap_skip(&p, end);
    if (ap_take(&p, end, "<?xml ")) {
        while (p < end && !ap_take(&p, end, "?>"))
            p++;
        ap_skip(&p, end);
    }
    if (!ap_take(&p, end, "<WlanApConfig>"))
        return -1;
    for (;;) {
        ap_skip(&p, end);
        if (ap_take(&p, end, "</WlanApConfig>"))
            break;
        static const char *const open[] = {"<SSID>", "<Key>", "<Security>", "<HideSSID>",
                                           "<Channel>"};
        static const char *const close[] = {"</SSID>", "</Key>", "</Security>", "</HideSSID>",
                                            "</Channel>"};
        unsigned i, n = 0;
        uint8_t value[AA_CFG_MAX];
        for (i = 0; i < 5; i++)
            if (ap_take(&p, end, open[i]))
                break;
        if (i == 5 || (seen & (1u << i)) || ap_text(&p, end, value, &n) ||
            !ap_take(&p, end, close[i]))
            return -1;
        seen |= 1u << i;
        if (i < 2) {
            if ((i == 0 && (!n || n > 32)) || (i == 1 && n < 8))
                return -1;
            uint8_t *dest = i == 0 ? cfg->ssid : cfg->key;
            for (unsigned j = 0; j <= n; j++)
                dest[j] = value[j];
            if (i == 0)
                cfg->ssid_len = n;
            else
                cfg->key_len = n;
        } else if (i == 2) {
            if (!ap_equal(value, n, "3"))
                return -1;
        } else if (i == 3) {
            if (!ap_equal(value, n, "true") && !ap_equal(value, n, "false"))
                return -1;
        } else {
            unsigned channel = 0;
            if (!n || n > 3)
                return -1;
            for (unsigned j = 0; j < n; j++) {
                if (value[j] < '0' || value[j] > '9')
                    return -1;
                channel = channel * 10 + value[j] - '0';
            }
            if (channel > 196)
                return -1;
        }
    }
    ap_skip(&p, end);
    if (seen != 31 || p != end)
        return -1;
    cfg->security = 8;
    cfg->ap_type = 0;
    cfg->port = 5288;
    return 0;
}

/* NetMgr persists its native role: 1=AP, 2=client, 3=combined.
 * This is the user's enable policy, not proof of radio/DHCP readiness.
 * wlan1 can retain UP/RUNNING and its address after AP shutdown. */
static int ap_mode_enabled(const char *text, unsigned length) {
    const char *p = text, *end = text + length;
    unsigned seen = 0, role = 0, active = 0;
    ap_skip(&p, end);
    if (!ap_take(&p, end, "[WlanLastmode]"))
        return 0;
    while (p < end) {
        if (*p != '\r' && *p != '\n')
            return 0;
        ap_skip(&p, end);
        if (p == end)
            break;
        if (ap_take(&p, end, "WlanRole=")) {
            if ((seen & 1) || p == end || *p < '0' || *p > '3')
                return 0;
            role = (unsigned)(*p++ - '0');
            seen |= 1;
        } else if (ap_take(&p, end, "Active=")) {
            if (seen & 2)
                return 0;
            if (ap_take(&p, end, "true"))
                active = 1;
            else if (!ap_take(&p, end, "false"))
                return 0;
            seen |= 2;
        } else
            return 0;
    }
    return seen == 3 && active && (role == 1 || role == 3);
}

/* QNX 6.x ARM BSD ifaddrs/sockaddr ABI, not Linux sockaddr. getifaddrs and
 * freeifaddrs are resolved in the already loaded libsocket through dlsym.
 * https://www.qnx.com/support/knowledgebase.html?id=50130000000mosy */
struct ap_ifaddrs {
    struct ap_ifaddrs *next;
    const char *name;
    unsigned flags;
    const uint8_t *addr, *netmask, *dst;
    void *data;
};
static int ap_interface(const struct ap_ifaddrs *list, struct aa_wifi_cfg *cfg) {
    unsigned ips = 0, macs = 0, nodes = 0;
    uint8_t selected_mac[6] = {0};
    for (const struct ap_ifaddrs *a = list; a; a = a->next) {
        if (++nodes > 256)
            return -1;
        if (!a->name || !ap_equal((const uint8_t *)a->name, 5, "wlan1") || a->name[5] || !a->addr)
            continue;
        const uint8_t *s = a->addr;
        if ((a->flags & 0x41) != 0x41)
            return -1;   /* IFF_UP | IFF_RUNNING */
        if (s[1] == 2) { /* AF_INET */
            const uint8_t *m = a->netmask;
            if (++ips != 1 || s[0] < 8 || !m || m[0] < 7 || m[4] != 255 || m[5] != 255 ||
                m[6] != 255 || (m[0] > 7 && m[7]) || !s[4] || s[4] == 127 || s[4] >= 224 || !s[7] ||
                s[7] == 255)
                return -1;
            unsigned n = 0;
            for (unsigned i = 4; i < 8; i++) {
                unsigned v = s[i];
                if (v >= 100)
                    cfg->ip[n++] = '0' + v / 100;
                if (v >= 10)
                    cfg->ip[n++] = '0' + (v / 10) % 10;
                cfg->ip[n++] = '0' + v % 10;
                if (i != 7)
                    cfg->ip[n++] = '.';
            }
            cfg->ip[n] = 0;
            cfg->ip_len = n;
        } else if (s[1] == 18) { /* AF_LINK; sockaddr_dl data starts at byte 8 */
            if (s[0] < 8 || s[6] != 6 || 8u + s[5] + 6u > s[0])
                return -1;
            const uint8_t *mac = s + 8 + s[5];
            /* QNX may repeat AF_LINK for the same interface. Accept only
             * identical addresses; conflicting BSSIDs remain ambiguous. */
            if (macs) {
                for (unsigned i = 0; i < 6; i++)
                    if (selected_mac[i] != mac[i])
                        return -1;
                continue;
            }
            macs = 1;
            for (unsigned i = 0; i < 6; i++)
                selected_mac[i] = mac[i];
            unsigned any = 0;
            if (mac[0] & 1)
                return -1;
            for (unsigned i = 0; i < 6; i++) {
                static const char hex[] = "0123456789abcdef";
                any |= mac[i];
                cfg->bssid[3 * i] = hex[mac[i] >> 4];
                cfg->bssid[3 * i + 1] = hex[mac[i] & 15];
                if (i != 5)
                    cfg->bssid[3 * i + 2] = ':';
            }
            if (!any)
                return -1;
            cfg->bssid[17] = 0;
            cfg->bssid_len = 17;
        }
    }
    return ips == 1 && macs == 1 ? 0 : -1;
}
#endif
