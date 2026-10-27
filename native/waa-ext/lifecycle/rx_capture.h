#ifndef WAA_RX_CAPTURE_H
#define WAA_RX_CAPTURE_H
#include <stdint.h>
#include "../../waa-spp/lifecycle/spp_lifecycle.h"
/* Decode only a complete known lifecycle physical frame. No stock parse-success flag exists.
 * prefix_mode: 0 = HIF header, 1 = 3-byte transport prefix. No input pointers retained. */
int waa_lifecycle_frame(const uint8_t *buf, uint32_t len, unsigned prefix_mode,
                        struct spp_lc_event *out);
#endif
