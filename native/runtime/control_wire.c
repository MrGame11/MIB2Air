#include "control_wire.h"
static void put(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}
static uint32_t get(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int valid(struct wh_message m) {
    if (m.kind == WH_PEER_IPV4)
        return m.generation != 0 && m.value != 0;
    if (m.kind == WH_PING)
        return !m.generation;
    if (m.kind < WH_READY || m.kind > WH_FAILED)
        return 0;
    if (m.kind == WH_READY)
        return m.value && m.value <= 65535;
    return m.generation != 0 && m.value == 0;
}
void wc_init(struct wc *c) { *c = (struct wc){0}; }
int wc_enqueue(struct wc *c, struct wh_message m) {
    if (c->failed)
        return -1;
    if (!valid(m) || c->count == WC_QUEUE) {
        c->failed = 1;
        return -1;
    }
    uint8_t *p = c->tx + ((c->head + c->count) % WC_QUEUE) * WC_FRAME;
    put(p, 0x57414131);
    put(p + 4, WC_FRAME);
    put(p + 8, m.kind);
    put(p + 12, m.generation);
    put(p + 16, m.value);
    c->count++;
    return 0;
}
const uint8_t *wc_output(struct wc *c, unsigned *len) {
    *len = 0;
    if (c->failed || !c->count)
        return 0;
    *len = WC_FRAME - c->offset;
    return c->tx + c->head * WC_FRAME + c->offset;
}
int wc_written(struct wc *c, unsigned n) {
    if (c->failed || !c->count || !n || n > WC_FRAME - c->offset) {
        c->failed = 1;
        return -1;
    }
    c->offset += n;
    if (c->offset == WC_FRAME) {
        c->offset = 0;
        c->head = (c->head + 1) % WC_QUEUE;
        c->count--;
    }
    return 0;
}
int wc_input(struct wc *c, const uint8_t *p, unsigned n, void (*cb)(void *, struct wh_message),
             void *ctx) {
    if (c->failed)
        return -1;
    for (unsigned i = 0; i < n; i++) {
        c->rx[c->used++] = p[i];
        if (c->used == WC_FRAME) {
            struct wh_message m = {get(c->rx + 8), get(c->rx + 12), get(c->rx + 16)};
            c->used = 0;
            if (get(c->rx) != 0x57414131 || get(c->rx + 4) != WC_FRAME || !valid(m)) {
                c->failed = 1;
                return -1;
            }
            cb(ctx, m);
            if (c->failed)
                return -1;
        }
    }
    return 0;
}
