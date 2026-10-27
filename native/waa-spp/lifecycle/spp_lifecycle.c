/* Stateless lifecycle decoder. Fields use explicit byte access and big-endian
 * integers; no input struct overlay. See spp_lifecycle.h for wire layouts. */
#include "spp_lifecycle.h"

static void lc_reset(struct spp_lc_event *ev) { *ev = (struct spp_lc_event){0}; }

static uint32_t lc_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int spp_lc_decode(uint32_t opcode, const uint8_t *payload, size_t len, struct spp_lc_event *out) {
    size_t want, off;
    unsigned i;

    if (out == NULL)
        return SPP_LC_EARG;
    lc_reset(out);
    if (payload == NULL)
        return SPP_LC_EARG;

    if (opcode == SPP_LC_OPC_CONNECTED)
        want = SPP_LC_CONNECTED_LEN;
    else if (opcode == SPP_LC_OPC_DISCONNECTED)
        want = SPP_LC_DISCONNECTED_LEN;
    else
        return SPP_LC_EOPCODE;
    if (len != want)
        return SPP_LC_ELEN;

    out->status = payload[0];
    for (i = 0; i < SPP_LC_ADDR_LEN; i++)
        out->addr[i] = payload[1 + i];
    off = 1 + SPP_LC_ADDR_LEN;
    if (opcode == SPP_LC_OPC_DISCONNECTED) {
        out->has_reason = 1;
        out->reason = payload[off];
        off++;
    }
    out->function = lc_be32(payload + off);
    out->instance = payload[off + 4];
    out->kind = (opcode == SPP_LC_OPC_CONNECTED) ? SPP_LC_CONNECTED : SPP_LC_DISCONNECTED;
    return SPP_LC_OK;
}

int spp_lc_connect_filter(const struct spp_lc_event *ev, const uint8_t *expected_addr) {
    unsigned i;

    if (ev == NULL)
        return SPP_LC_REJ_ARG;
    if (expected_addr == NULL)
        return SPP_LC_REJ_NOADDR;
    if (ev->kind != SPP_LC_CONNECTED)
        return SPP_LC_REJ_KIND;
    if (ev->status != 0x00u && ev->status != 0x80u)
        return SPP_LC_REJ_STATUS;
    if (ev->function != SPP_LC_FUNCTION_SPP)
        return SPP_LC_REJ_FUNCTION;
    if (ev->instance > 1u)
        return SPP_LC_REJ_INSTANCE;
    for (i = 0; i < SPP_LC_ADDR_LEN; i++)
        if (ev->addr[i] != expected_addr[i])
            return SPP_LC_REJ_ADDR;
    return SPP_LC_ACCEPT;
}
