#include "profile_generated.h"
/* WirelessAdapter UUID setup and factory reset. Profile-selected ARM hooks.
 * Registration runs before the RX thread. In factory-reset mode the UUID request runs once
 * after APP_INIT; in UUID-setup mode the including target decides when to send it.
 * Restore resets the service to factory SPP UUID 0x1101, not a saved custom UUID. */

#include <stdint.h>
#include <stddef.h>

#define WAA_MODE_UUIDSET 1
#define WAA_MODE_RESTORE 2
#ifndef WAA_MODE
#error "Select UUID setup (1) or factory reset (2)"
#endif
#if WAA_MODE != WAA_MODE_UUIDSET && WAA_MODE != WAA_MODE_RESTORE
#error "Unsupported UUID operation"
#endif

#include "../runtime/hif.h"

/* WAA SPP service UUID128 in big-endian string order. */
static const uint8_t WAA_UUID128[16] = {
    0x4d, 0xe1, 0x7a, 0x00, 0x52, 0xcb, 0x11, 0xe6, 0xbd, 0xf4, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66,
};
static const uint8_t SPP_1101[2] = {0x11, 0x01};

enum { UUID_16 = 1, UUID_32 = 2, UUID_128 = 3 };
static uint32_t uuid_type_len(uint32_t t) {
    return t == UUID_16 ? 2u : t == UUID_32 ? 4u : t == UUID_128 ? 16u : 0u;
}

/* One-shot guard in .data (writable => this mode's blob is RWX). Only the RX one-shot writes
 * it, exactly once; there is no runtime "arm" word to patch. */
#if WAA_MODE == WAA_MODE_RESTORE
__attribute__((used, section(".data"))) static volatile uint32_t g_waa_oneshot = 0;
#endif

/* UUID-set confirmation: [instance][status]. Zero is success; 2/3/7 mean
 * bad length/type or busy. Missing confirmation is handled by the stock timeout. */
void waa_uuid_result_cb(void *ctx, uint32_t opcode, uint32_t param_count, const void *node) {
    void *hl = HL_OF(ctx);
    const uint8_t *p0 = pa_byte(node, param_count, 0);
    const uint8_t *p1 = pa_byte(node, param_count, 1);
    uint32_t instance = p0 ? p0[0] : 0xffu;
    uint32_t status = p1 ? p1[0] : 0xffu;
    const char *why = status == 0   ? "OK"
                      : status == 2 ? "FAIL(bad_length)"
                      : status == 3 ? "FAIL(bad_type)"
                      : status == 7 ? "FAIL(busy/connected)"
                                    : "FAIL";

    Log(hl, WAA_LOG_LEVEL, "WAA_EXT uuid_result op=0x%04x count=%u instance=%u status=%u (%s)\n",
        opcode, param_count, instance, status, why);
}

/* Queue UUID-set as [instance][length u16 BE][first type/UUID][service type/UUID].
 * The block length must be 6..34 bytes. Only the service UUID selects the SDP service;
 * preserve the first UUID separately because its role is not established. */
int waa_ext_send_uuid_set(void *hl, uint32_t instance, uint32_t first_type,
                          const uint8_t *first_uuid, uint32_t service_type,
                          const uint8_t *service_uuid) {
    /* The UUID BLOCK only: [first_type][first_uuid][service_type][service_uuid]. instance and the
     * block length are SEPARATE schema fields (see below) — NOT baked into this buffer. */
    uint8_t block[36];
    uint8_t inst;
    uint16_t blob_len; /* native u16; type-18 BE-copies it -> big-endian on wire */
    uint32_t l1 = uuid_type_len(first_type);
    uint32_t l2 = uuid_type_len(service_type);
    uint32_t n = 0, i;

    if (first_type < 1 || first_type > 3)
        return 2;
    if (service_type < 1 || service_type > 3)
        return 2;
    /* Block length includes both type bytes and UUID bodies. */
    if ((2u + l1 + l2) < 6 || (2u + l1 + l2) > 0x22)
        return 3;

    block[n++] = (uint8_t)first_type;
    for (i = 0; i < l1; i++)
        block[n++] = first_uuid[i];
    block[n++] = (uint8_t)service_type;
    for (i = 0; i < l2; i++)
        block[n++] = service_uuid[i];
    blob_len = (uint16_t)n; /* == 2 + l1 + l2 */
    inst = (uint8_t)instance;

    /* Schema fields: one-byte instance, two-byte big-endian length, raw UUID block.
     * The data field requires a preceding length field for stock TX decoding. */
    struct hif_param sp[3] = {{&inst, 1, 0}, {&blob_len, 2, 0}, {block, (uint16_t)n, 0}};
    BuildAndQueueMessage(hl, 0x6205u, 3u, sp, 0);
    Log(hl, WAA_LOG_LEVEL,
        "WAA_EXT: queued 0x6205 uuid_set inst=%u blob=%u (await result/timeout)\n", instance,
        (unsigned)n);
    return 0;
}

#if WAA_MODE == WAA_MODE_RESTORE
/* Reset both UUID fields to factory SPP 0x1101. There is no verified readback
 * of previous custom UUID values, so this cannot restore them. */
int waa_ext_restore_uuid(void *hl, uint32_t instance) {
    Log(hl, WAA_LOG_LEVEL,
        "WAA_EXT: factory-reset uuid -> 0x1101 inst=%u (NOT prior-value restore)\n", instance);
    return waa_ext_send_uuid_set(hl, instance, UUID_16, SPP_1101, UUID_16, SPP_1101);
}

/* Wait for BT application initialization (0x0401), when SDP services exist
 * and the instance is not yet connected, before sending the UUID request. */
#define WAA_TRIGGER_OPCODE 0x0401u

/*
 * One-shot body, run AFTER the stock ParseMessage of this frame. hl = HifLib*, buf = the RX PDU
 * passed to ParseMessage. We read the inbound opcode exactly as ParseMessage does (an optional
 * 3-byte ASIP length prefix is present iff hl+WA_ASIP_PREFIX != 0) and fire ONLY on the trigger
 * event, exactly once. No retransmission: on FAIL or timeout the guard stays set and nothing
 * re-sends.
 */
void waa_rx_oneshot_c(void *hl, const uint8_t *buf, uint32_t len, uint32_t flag) {
    uint32_t prefix, opcode;
    if (g_waa_oneshot || flag || !buf)
        return; /* nested or bufferless frames never carry the top-level trigger */
    prefix = (*(const volatile uint8_t *)((const char *)hl + WA_ASIP_PREFIX)) ? 3u : 0u;
    if (len < prefix + 4u)
        return;
    opcode = (uint32_t)buf[prefix] | ((uint32_t)buf[prefix + 1] << 8);
    if (opcode != WAA_TRIGGER_OPCODE)
        return; /* wait for BT_APPL_APP_INIT_IND, ignore other frames */
#ifdef WAA_RESTORE_BEGIN
    if (WAA_RESTORE_BEGIN())
        return;
#endif
    g_waa_oneshot = 1; /* set BEFORE sending: fire exactly once, never retried */
    Log(hl, WAA_LOG_LEVEL, "WAA_EXT: trigger 0x0401 (APP_INIT) seen -> one-shot UUID op\n");
    waa_ext_restore_uuid(hl, 0);
}

/* RX detour ABI: r0=&out, r1=HifLib*, r2=buffer, r3=length, [sp]=flag.
 * Forward all five arguments unchanged; pass buffer, length and flag to the one-shot callback. */
__attribute__((naked)) void waa_rx_oneshot_entry(void) {
    __asm__ volatile(
        "push   {r0-r3, r4, lr}        \n" /* 6 words -> 8-byte aligned; r0-r3 remain live     */
        "mov    r4, r1                 \n" /* r4 = HifLib* (preserved across ParseMessage)      */
        "ldr    r12, [sp, #24]         \n" /* r12 = caller's 5th arg (orig [sp], now [sp+24])   */
        "sub    sp, sp, #8             \n" /* reserve the 5th-arg slot, keep 8-byte alignment   */
        "str    r12, [sp, #0]          \n"
        "ldr    r12, =" WAA_STRING(
            WA_PARSE) "       \n"
                      "blx    r12                    \n" /* stock ParseMessage(r0,r1,r2,r3,
                                                            [sp]=flag)        */
                      "add    sp, sp, #8             \n"
                      "mov    r0, r4                 \n" /* r0 = HifLib* */
                      "ldr    r1, [sp, #8]           \n" /* r1 = saved r2 = RX buffer */
                      "ldr    r2, [sp, #12]          \n" /* r2 = saved r3 = RX length */
                      "ldr    r3, [sp, #24]          \n" /* r3 = caller's 5th arg = flag */
                      "bl     waa_rx_oneshot_c       \n"
                      "pop    {r0-r3, r4, lr}        \n"
                      "bx     lr                     \n");
}

#endif

/*
 * Registration (ctor hook). Runs after stock registration, before the RX thread. RegisterCmd
 * precedes RegisterHandler (which asserts the opcode present) and UpdateCmd (which asserts it
 * present and names it). No frame is sent here.
 */
void waa_ext_register(void *hifhandler) {
    void *hl = HL_OF(hifhandler);

    RegisterCmd(hl, 0x6205u, 1 /*CFM*/, 6, "(1 instance, 1 status)");
#ifndef WAA_UUID_CALLBACK
#define WAA_UUID_CALLBACK waa_uuid_result_cb
#endif
    RegisterHandler(hl, 0x6205u, (void *)WAA_UUID_CALLBACK);
    /* A variable data field requires a preceding length field for stock TX decoding. */
    RegisterCmd(hl, 0x6205u, 0 /*REQ send*/, 6, "(1 instance, 18, 14 data)");
    /* Arm the request confirmation timer; the format describes all three fields. */
    UpdateCmd(hl, 0x6205u, 0 /*REQ*/, "WAA_SPP_UUID_SET_REQ", "instance=%d, len=%d, data=0x%s",
              1000);
    UpdateCmd(hl, 0x6205u, 1 /*CFM*/, "WAA_SPP_UUID_SET_CFM", "instance=%d, status=%d", 0);
    Log(hl, WAA_LOG_LEVEL,
        "WAA_EXT: registered 0x6205(req/cfm, timeout 1000ms); mode=%d one-shot armed\n", WAA_MODE);
}

#if WAA_MODE == WAA_MODE_RESTORE
/* Constructor detour: r0 is HifHandler; run stock registration first. */
__attribute__((naked)) void waa_ext_entry(void) {
    __asm__ volatile(
        "push   {r0-r3, r12, lr}       \n"
        "ldr    r12, =" WAA_STRING(WA_REGISTER_CALLBACKS) "       \n"
                                                          "blx    r12                    \n"
                                                          "ldr    r0, [sp]               \n"
                                                          "bl     waa_ext_register       \n"
                                                          "pop    {r0-r3, r12, lr}       \n"
                                                          "bx     lr                     \n");
}

#endif
