#include "rx_capture.h"
int waa_lifecycle_frame(const uint8_t *buf, uint32_t len, unsigned prefix_mode,
                        struct spp_lc_event *out) {
    uint32_t offset, opcode, payload;
    if (!out)
        return SPP_LC_EARG;
    /* Reset using the decoder's defined error path. */
    (void)spp_lc_decode(0, 0, 0, out);
    if (!buf || prefix_mode > 1)
        return SPP_LC_EARG;
    offset = prefix_mode ? 3u : 0u;
    if (len < offset + 4u)
        return SPP_LC_ELEN;
    opcode = (uint32_t)buf[offset] | ((uint32_t)buf[offset + 1] << 8);
    payload = (uint32_t)buf[offset + 2] | ((uint32_t)buf[offset + 3] << 8);
    /* Conservative exact physical length: no undocumented transport padding accepted. */
    if (len != offset + 4u + payload)
        return SPP_LC_ELEN;
    if (prefix_mode && ((uint32_t)buf[0] | ((uint32_t)buf[1] << 8)) != payload + 4u)
        return SPP_LC_ELEN;
    return spp_lc_decode(opcode, buf + offset + 4u, payload, out);
}
