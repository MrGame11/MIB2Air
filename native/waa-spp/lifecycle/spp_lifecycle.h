/*
 * spp_lifecycle.h — stateless decoder of Bluetooth lifecycle HIF events
 * 0x0407 (connected) / 0x0409 (disconnected), plus a pure CONNECT-success filter.
 *
 * Freestanding C (stdint/stddef only): no malloc, no globals, no logging, no packed or
 * unaligned casts, no pointer into the caller's input is retained.
 *
 * Wire layout:
 *   0x0407  [status u8][bdaddr 6][function u32 BE][instance u8]              = 12 bytes
 *   0x0409  [status u8][bdaddr 6][reason u8][function u32 BE][instance u8]   = 13 bytes
 * 0x0408 is a different command (empty schema), NOT a disconnect; it is rejected.
 *
 * Length policy: the raw HIF payload must have EXACTLY the documented length. Shorter
 * (truncated) and longer (trailing bytes) payloads are both rejected with SPP_LC_ELEN.
 */
#ifndef SPP_LIFECYCLE_H
#define SPP_LIFECYCLE_H

#include <stddef.h>
#include <stdint.h>

#define SPP_LC_OPC_CONNECTED 0x0407u
#define SPP_LC_OPC_DISCONNECTED 0x0409u

#define SPP_LC_ADDR_LEN 6u
#define SPP_LC_CONNECTED_LEN 12u
#define SPP_LC_DISCONNECTED_LEN 13u

#define SPP_LC_FUNCTION_SPP 1u /* service/function mask of our SPP server */

/* Event kinds. SPP_LC_NONE is the defined "nothing decoded" state of a failed decode. */
enum { SPP_LC_NONE = 0, SPP_LC_CONNECTED = 1, SPP_LC_DISCONNECTED = 2 };

/* spp_lc_decode() result codes. */
enum {
    SPP_LC_OK = 0,
    SPP_LC_EARG = 1,    /* out == NULL, or payload == NULL                       */
    SPP_LC_EOPCODE = 2, /* opcode is neither 0x0407 nor 0x0409 (incl. 0x0408)    */
    SPP_LC_ELEN = 3     /* len != exact length for the opcode (short or long)    */
};

/* spp_lc_connect_filter() verdicts. Only SPP_LC_ACCEPT means "our SPP channel is up". */
enum {
    SPP_LC_ACCEPT = 0,
    SPP_LC_REJ_ARG = 1,      /* ev == NULL                                       */
    SPP_LC_REJ_NOADDR = 2,   /* no expected address supplied: always refused     */
    SPP_LC_REJ_KIND = 3,     /* not a decoded CONNECTED event                    */
    SPP_LC_REJ_STATUS = 4,   /* status not in {0x00, 0x80}                       */
    SPP_LC_REJ_FUNCTION = 5, /* function != 1                                    */
    SPP_LC_REJ_INSTANCE = 6, /* runtime instance not in {0, 1}                   */
    SPP_LC_REJ_ADDR = 7      /* wire address differs from the expected one       */
};

/*
 * Decoded event: a self-contained copy (by value) of the payload fields.
 *   addr      — the 6 wire bytes in wire order, NOT reversed into a printable MAC.
 *   function  — u32 assembled from big-endian wire bytes.
 *   reason    — valid only when has_reason == 1 (0x0409); 0 otherwise.
 *   instance  — RUNTIME instance byte reported by the module (not the UUID-registration slot).
 */
struct spp_lc_event {
    uint32_t function;
    uint8_t kind;
    uint8_t status;
    uint8_t has_reason;
    uint8_t reason;
    uint8_t instance;
    uint8_t addr[SPP_LC_ADDR_LEN];
};

/*
 * Decode one raw HIF payload. On ANY error, if out != NULL it is reset to the all-zero
 * state (kind = SPP_LC_NONE); it never holds partially decoded data. Statuses (incl.
 * non-success) and the disconnect reason are preserved as-is; no semantics are attached.
 */
int spp_lc_decode(uint32_t opcode, const uint8_t *payload, size_t len, struct spp_lc_event *out);

/*
 * Pure CONNECT-success predicate: kind CONNECTED, status 0x00 or 0x80, function == 1,
 * instance 0 or 1, addr equal to expected_addr (6 wire bytes). expected_addr == NULL is
 * always refused. Disconnect events are never "accepted" here. Checks run in the order
 * of the verdict enum; the first failing check is returned.
 */
int spp_lc_connect_filter(const struct spp_lc_event *ev, const uint8_t *expected_addr);

#endif /* SPP_LIFECYCLE_H */
