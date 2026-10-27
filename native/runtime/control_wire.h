#ifndef WAA_CONTROL_WIRE_H
#define WAA_CONTROL_WIRE_H
#include "handoff.h"
/* Fixed endian-independent records, no pointer/fd/password crosses processes.
 * Each side has one reader/writer on its owner thread. */
#define WC_FRAME 20
#define WC_QUEUE 8
struct wc {
    uint8_t rx[WC_FRAME], tx[WC_QUEUE * WC_FRAME];
    unsigned used, head, count, offset;
    int failed;
};
void wc_init(struct wc *);
int wc_enqueue(struct wc *, struct wh_message);
const uint8_t *wc_output(struct wc *, unsigned *);
int wc_written(struct wc *, unsigned);
int wc_input(struct wc *, const uint8_t *, unsigned, void (*)(void *, struct wh_message), void *);
#endif
