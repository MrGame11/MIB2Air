#include "handoff.h"
static int terminal(const struct wh *s) { return s->state == WH_DONE || s->state == WH_ERROR; }
static int expired(const struct wh *s, uint32_t now) { return (int32_t)(now - s->deadline) >= 0; }
static int emit(struct wh *s, uint32_t kind, uint32_t value) {
    struct wh_message m = {kind, s->generation, value};
    if (!s->link || s->ops.send(s->ops.ctx, m)) {
        s->link = 0;
        return -1;
    }
    return 0;
}
static void request_stop(struct wh *s) {
    if (!s->stop_sent) {
        s->stop_sent = 1;
        s->ops.stop(s->ops.ctx);
    }
}
static void reset_generation(struct wh *s) {
    s->state = WH_WAIT;
    s->used = s->accepted = s->stop_sent = s->cleaned = s->phone = 0;
}
static void stop(struct wh *s, uint32_t now) {
    if (terminal(s) || s->state == WH_STOPPING)
        return;
    s->state = WH_STOPPING;
    s->deadline = now + s->timeout;
    request_stop(s);
}
static void begin(struct wh *s, uint32_t now) {
    if (s->sal || s->state != WH_READY_STATE || !s->phone || s->used || !s->link ||
        s->generation == 0xffffffffu)
        return;
    s->used = 1;
    s->generation++;
    s->state = WH_ARMING;
    s->deadline = now + s->timeout;
    if (emit(s, WH_START, 0))
        stop(s, now);
}
void wh_init(struct wh *s, int sal, const struct wh_ops *ops, uint32_t timeout) {
    *s = (struct wh){0};
    s->ops = *ops;
    s->sal = sal != 0;
    s->link = 1;
    s->timeout = timeout;
    if (!timeout || timeout > 0x3fffffffu)
        s->state = WH_ERROR;
}
void wh_ready(struct wh *s, uint16_t port, uint32_t now) {
    if (!s->sal || s->state != WH_WAIT || !port || !s->link)
        return;
    s->port = port;
    s->state = WH_READY_STATE;
    if (emit(s, WH_READY, port))
        stop(s, now);
}
void wh_phone(struct wh *s, uint32_t now) {
    if (s->sal || terminal(s))
        return;
    s->phone = 1;
    begin(s, now);
}
static void deadlines(struct wh *s, uint32_t now) {
    if (terminal(s))
        return;
    if ((s->state == WH_ARMING || s->state == WH_BOOT || s->state == WH_TCP_WAIT ||
         s->state == WH_STOPPING) &&
        expired(s, now)) {
        if (s->state == WH_STOPPING) {
            s->dirty = 1;
            s->state = WH_ERROR;
            return;
        }
        wh_cancel(s, now);
        return;
    }
}
void wh_tick(struct wh *s, uint32_t now) {
    deadlines(s, now);
    if (s->sal && s->state == WH_ARMING) {
        int r = s->ops.drain(s->ops.ctx);
        if (r < 0) {
            stop(s, now);
            return;
        }
        if (!r) {
            s->state = WH_TCP_WAIT;
            s->deadline = now + s->timeout;
            if (emit(s, WH_ARMED, 0))
                stop(s, now);
        }
    }
}
void wh_receive(struct wh *s, struct wh_message m, uint32_t now) {
    deadlines(s, now);
    if (!s->link)
        return;
    if (m.kind == WH_READY && !s->sal && s->cleaned && !s->dirty && m.generation == s->generation &&
        m.value && m.value <= 65535)
        reset_generation(s);
    if (terminal(s))
        return;
    if (m.kind == WH_READY && !s->sal && s->state == WH_WAIT && m.generation == s->generation &&
        m.value && m.value <= 65535) {
        s->port = m.value;
        s->state = WH_READY_STATE;
        begin(s, now);
        return;
    }
    if (m.kind == WH_START && s->sal && s->state == WH_READY_STATE && !s->used &&
        s->generation != 0xffffffffu && m.generation == s->generation + 1) {
        s->used = 1;
        s->generation = m.generation;
        s->state = WH_ARMING;
        s->deadline = now + s->timeout;
        return;
    }
    if (!s->used || !s->generation || m.generation != s->generation)
        return;
    if (m.kind == WH_CANCEL && s->sal) {
        stop(s, now);
        return;
    }
    if (m.kind == WH_TORNDOWN && !s->sal) {
        s->cleaned = 1;
        if (s->state == WH_SESSION)
            s->state = WH_DONE;
        else
            s->state = WH_ERROR;
        request_stop(s);
        return;
    }
    if (m.kind == WH_FAILED && !s->sal) {
        stop(s, now);
        return;
    }
    if (s->state == WH_STOPPING)
        return;
    if (m.kind == WH_ARMED && !s->sal && s->state == WH_ARMING) {
        s->state = WH_BOOT;
        s->deadline = now + s->timeout;
        if (s->ops.bootstrap(s->ops.ctx, (uint16_t)s->port))
            wh_cancel(s, now);
        return;
    }
    if (m.kind == WH_ACCEPTED && !s->sal && (s->state == WH_BOOT || s->state == WH_TCP_WAIT)) {
        s->accepted = 1;
        if (s->state == WH_TCP_WAIT)
            s->state = WH_SESSION;
    }
}
void wh_boot_done(struct wh *s, uint32_t now) {
    deadlines(s, now);
    if (!s->sal && s->state == WH_BOOT) {
        s->state = s->accepted ? WH_SESSION : WH_TCP_WAIT;
        s->deadline = now + s->timeout;
    }
}
void wh_accept(struct wh *s, int fd, uint32_t now) {
    deadlines(s, now);
    if (!s->sal || s->state != WH_TCP_WAIT || !s->link) {
        s->ops.close_fd(s->ops.ctx, fd);
        return;
    }
    int adopted = s->ops.adopt(s->ops.ctx, fd);
    if (adopted) {
        if (adopted != -2)
            s->ops.close_fd(s->ops.ctx, fd);
        stop(s, now);
        return;
    }
    s->state = WH_SESSION;
    s->accepted = 1;
    if (emit(s, WH_ACCEPTED, 0))
        stop(s, now);
}
void wh_cancel(struct wh *s, uint32_t now) {
    if (terminal(s) || s->state == WH_STOPPING)
        return;
    if (!s->sal && s->generation)
        emit(s, WH_CANCEL, 0);
    stop(s, now);
}
void wh_link_lost(struct wh *s, uint32_t now) {
    s->link = 0;
    stop(s, now);
}
void wh_cleaned(struct wh *s) {
    if (!s->sal || (s->state != WH_STOPPING && s->state != WH_SESSION))
        return;
    int success = s->state == WH_SESSION;
    if (s->generation && s->link && emit(s, WH_TORNDOWN, 0))
        success = 0;
    s->cleaned = 1;
    s->state = success ? WH_DONE : WH_ERROR;
}

int wh_rearm(struct wh *s) {
    if (!s->sal || !terminal(s) || !s->cleaned || s->dirty || !s->link ||
        s->generation == 0xffffffffu)
        return -1;
    reset_generation(s);
    return 0;
}
