#include "lifecycle_queue.h"

int spp_lcq_reset(struct spp_lcq *q) {
    if (!q)
        return SPP_LCQ_EARG;
    *q = (struct spp_lcq){0};
    return SPP_LCQ_OK;
}
int spp_lcq_push(struct spp_lcq *q, const struct spp_lc_event *event) {
    unsigned int tail;
    if (!q || !event)
        return SPP_LCQ_EARG;
    if (q->poisoned)
        return SPP_LCQ_POISONED;
    if (!((event->kind == SPP_LC_CONNECTED && event->has_reason == 0 && event->reason == 0) ||
          (event->kind == SPP_LC_DISCONNECTED && event->has_reason == 1)))
        return SPP_LCQ_EEVENT;
    if (q->count == SPP_LCQ_CAPACITY) {
        q->poisoned = 1;
        return SPP_LCQ_POISONED;
    }
    tail = (q->head + q->count) % SPP_LCQ_CAPACITY;
    q->slots[tail] = *event;
    ++q->count;
    return SPP_LCQ_OK;
}
int spp_lcq_pop(struct spp_lcq *q, struct spp_lc_event *out) {
    if (!out)
        return SPP_LCQ_EARG;
    *out = (struct spp_lc_event){0};
    if (!q)
        return SPP_LCQ_EARG;
    if (q->poisoned)
        return SPP_LCQ_POISONED;
    if (q->count == 0)
        return SPP_LCQ_EMPTY;
    *out = q->slots[q->head];
    q->slots[q->head] = (struct spp_lc_event){0};
    q->head = (q->head + 1u) % SPP_LCQ_CAPACITY;
    --q->count;
    return SPP_LCQ_OK;
}
