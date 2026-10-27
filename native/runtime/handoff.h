#ifndef WAA_HANDOFF_H
#define WAA_HANDOFF_H
#include <stdint.h>
/* Generation-scoped sessions; rearm only after confirmed cleanup. All entries on the role's
 * serialized owner. Effects return 0 on success; no blocking effects allowed. */
enum wh_kind {
    WH_READY = 1,
    WH_START,
    WH_ARMED,
    WH_ACCEPTED,
    WH_CANCEL,
    WH_TORNDOWN,
    WH_FAILED,
    WH_PING,
    WH_PEER_IPV4
};
enum wh_state {
    WH_WAIT,
    WH_READY_STATE,
    WH_ARMING,
    WH_BOOT,
    WH_TCP_WAIT,
    WH_SESSION,
    WH_STOPPING,
    WH_DONE,
    WH_ERROR
};
struct wh_message {
    uint32_t kind, generation, value;
};
struct wh_ops {
    int (*send)(void *, struct wh_message);
    int (*bootstrap)(void *, uint16_t);
    int (*drain)(void *);      /* 0 completely drained, 1 more pending, -1 failure */
    int (*adopt)(void *, int); /* 0 transferred; -1 caller retains fd; -2 consumed but failed */
    void (*close_fd)(void *, int);
    void (*stop)(void *); /* asynchronous cleanup request, not confirmation */
    void *ctx;
};
struct wh {
    struct wh_ops ops;
    uint32_t state, generation, deadline, timeout, port;
    uint8_t sal, phone, used, link, accepted, stop_sent, dirty, cleaned;
};
void wh_init(struct wh *, int sal, const struct wh_ops *, uint32_t timeout);
void wh_ready(struct wh *, uint16_t port, uint32_t now);
void wh_phone(struct wh *, uint32_t now);
void wh_receive(struct wh *, struct wh_message, uint32_t now);
void wh_tick(struct wh *, uint32_t now);
void wh_boot_done(struct wh *, uint32_t now);
void wh_accept(struct wh *, int fd, uint32_t now);
void wh_cancel(struct wh *, uint32_t now);
void wh_link_lost(struct wh *, uint32_t now);
void wh_cleaned(struct wh *);
int wh_rearm(struct wh *);
#endif
