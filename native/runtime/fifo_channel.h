/* Included after runtime and wire code in each target blob. Fresh FIFOs must be
 * created once by SAL after the boot selector removes stale names, mode600. Both ends kept RDWR: no
 * SIGPIPE on peer loss; heartbeat detects that loss instead of relying on EOF. */
#ifndef WAA_FIFO_CHANNEL_H
#define WAA_FIFO_CHANNEL_H
struct fifo_channel {
    struct wc wire;
    int in, out, started, failed, peer_seen;
    uint32_t last_rx, last_ping, now;
    struct wh *fsm;
};
static void fc_message(void *v, struct wh_message m) {
    struct fifo_channel *f = v;
    f->last_rx = f->now;
    f->peer_seen = 1;
    if (m.kind == WH_PEER_IPV4) {
#ifdef WAA_PEER_MESSAGE
        WAA_PEER_MESSAGE(m);
#endif
        return;
    }
    if (m.kind != WH_PING)
        wh_receive(f->fsm, m, f->now);
}
static int fc_send(void *f, struct wh_message m) {
    return wc_enqueue(&((struct fifo_channel *)f)->wire, m);
}
static int fc_open(struct fifo_channel *f, struct wh *s, int sal, uint32_t now) {
    *f = (struct fifo_channel){0};
    f->fsm = s;
    f->in = qr_open(sal ? "/tsd/var/wireless-aa/run/wa2sal.fifo"
                        : "/tsd/var/wireless-aa/run/sal2wa.fifo",
                    0x82);
    f->out = qr_open(sal ? "/tsd/var/wireless-aa/run/sal2wa.fifo"
                         : "/tsd/var/wireless-aa/run/wa2sal.fifo",
                     0x82);
    if (f->in < 0 || f->out < 0) {
        qr_close(f->in);
        qr_close(f->out);
        f->in = f->out = -1;
        f->failed = 1;
        return -1;
    }
    f->last_rx = now;
    f->last_ping = now - 1000u;
    f->started = 1;
    return 0;
}
/* Output-only pass: callers can publish newly queued control messages without
 * polling the same input FIFO again. Heartbeats and peer deadlines stay in pump. */
static void fc_flush(struct fifo_channel *f) {
    if (!f->started)
        return;
    for (unsigned i = 0; !f->failed && i < 4; i++) {
        unsigned n;
        const uint8_t *p = wc_output(&f->wire, &n);
        if (!p)
            break;
        int w = qr_write(f->out, p, n);
        if (w < 0) {
            if (!qr_transient(qr_errno()))
                f->failed = 1;
            break;
        }
        if (!w || wc_written(&f->wire, (unsigned)w)) {
            f->failed = 1;
            break;
        }
    }
    if (f->failed) {
        qr_close(f->in);
        qr_close(f->out);
        f->in = f->out = -1;
        f->started = 0;
        wh_link_lost(f->fsm, f->now);
    }
}

static void fc_pump(struct fifo_channel *f, uint32_t now) {
    if (!f->started || f->failed)
        return;
    f->now = now;
    for (unsigned i = 0; i < 4; i++) {
        uint8_t b[80];
        int n = qr_read(f->in, b, sizeof(b));
        if (n < 0) {
            if (!qr_transient(qr_errno()))
                f->failed = 1;
            break;
        }
        if (!n) {
            f->failed = 1;
            break;
        }
        if (wc_input(&f->wire, b, (unsigned)n, fc_message, f)) {
            f->failed = 1;
            break;
        }
    }
    /* Startup is not a lost established peer. Match WA's 90s startup budget. */
    /* A GAL Worker heartbeat is generated on idle dispatch, not a scheduler
     * guarantee. An accepted TCP generation owns its own I/O watchdog. Keep
     * the control stream open across busy-worker gaps so queued teardown and
     * READY can still be delivered. Bootstrap still has bounded peer loss. */
    if ((uint32_t)(now - f->last_rx) >= (f->peer_seen ? 5000u : 90000u) && !f->fsm->accepted) {
        f->failed = 1;
    }
    if (!f->failed && (uint32_t)(now - f->last_ping) >= 1000u) {
        /* Heartbeats must not exhaust the bounded control queue while a peer
         * is busy. One queued frame already guarantees forthcoming activity. */
        if (!f->wire.count && wc_enqueue(&f->wire, (struct wh_message){WH_PING, 0, now})) {
            f->failed = 1;
        }
        f->last_ping = now;
    }
    fc_flush(f);
}
#endif
