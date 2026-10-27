#ifndef WAA_PEER_SELECTION_H
#define WAA_PEER_SELECTION_H
#include <stdint.h>
struct waa_peer_selection {
    uint8_t address[6];
    unsigned selected;
};
static int waa_peer_equal(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static unsigned waa_peer_nonzero(const uint8_t *a) {
    unsigned n = 0;
    for (unsigned i = 0; i < 6; i++)
        n |= a[i];
    return n;
}
/* Select the first successful inbound SPP peer.
 * Keep that identity until cleanup, so another paired device cannot cancel it. */
static int waa_peer_select(struct waa_peer_selection *p, const uint8_t address[6], unsigned connect,
                           unsigned function, unsigned status, unsigned instance) {
    if (!waa_peer_nonzero(address))
        return 0;
    if (p->selected)
        return waa_peer_equal(p->address, address);
    if (!connect || function != 1 || instance > 1 || (status != 0 && status != 0x80))
        return 0;
    for (unsigned i = 0; i < 6; i++)
        p->address[i] = address[i];
    p->selected = 1;
    return 1;
}
#endif
