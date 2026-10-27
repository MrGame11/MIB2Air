#ifndef SPP_LIFECYCLE_QUEUE_H
#define SPP_LIFECYCLE_QUEUE_H
#include "spp_lifecycle.h"

#define SPP_LCQ_CAPACITY 8u
/* Caller-owned, initialized by reset before use. No synchronization is provided. */
struct spp_lcq {
    struct spp_lc_event slots[SPP_LCQ_CAPACITY];
    unsigned int head;
    unsigned int count;
    unsigned int poisoned;
};
enum {
    SPP_LCQ_OK = 0,
    SPP_LCQ_EMPTY = 1,
    SPP_LCQ_POISONED = 2,
    SPP_LCQ_EARG = 3,
    SPP_LCQ_EEVENT = 4
};
/* Reset discards all events. After overflow, caller must establish an external
 * lifecycle barrier before resetting; reset does NOT prove an SPP drain. */
int spp_lcq_reset(struct spp_lcq *q);
/* Copy only; never invokes callbacks. Structural validation only, no filtering.
 * Full queue + valid event poisons it permanently until explicit reset. */
int spp_lcq_push(struct spp_lcq *q, const struct spp_lc_event *event);
/* Pop from external serialized dispatch, never recursively from the producer.
 * On failure, non-NULL out is zeroed. out must not overlap q. */
int spp_lcq_pop(struct spp_lcq *q, struct spp_lc_event *out);
#endif
