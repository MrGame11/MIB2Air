#include "spp.h"
#include "profile_generated.h"
/* SPP byte transport. All public calls and HIF callbacks must use one dispatch thread.
 * The non-reentrant drain owns TX consumption; callbacks only update flags and RX.
 * Reentrant callbacks enqueue work, never pump or mutate TX indices. Check flow/error
 * before each chunk and limit each pass to SPP_PUMP_BATCH. Public mutators reject reentry.
 * Reopen requires mark_drained after confirmed teardown; late closed-session events drop.
 * Wire: TX 0x6200 [instance][length u16 BE][data], up to 667 bytes; error-only CFM.
 * RX 0x6300 [marker][length u16 BE][data], up to 200 bytes; flow 0x6302 [instance][value]. */
#include <stdint.h>
#include <stddef.h>

#include "../runtime/hif.h"

static const char SCHEMA_TX[] = "(1 instance, 18, 14 data)";
static const char SCHEMA_TXCFM[] = "(1 instance, 1 status)";
static const char SCHEMA_RX[] = "(1 marker, 18, 14 data)";
static const char SCHEMA_FLOW[] = "(1 instance, 1 value)";
#define SPP_OPC_TX 0x6200u
#define SPP_OPC_RX 0x6300u
#define SPP_OPC_FLOW 0x6302u

#define SPP_NINST 2
#define SPP_MAX_CHUNK 667
#define SPP_TXRING 4096
#define SPP_RXBUF 4096
#define SPP_PUMP_BATCH 4
#define SPP_NONE 0xffu

enum {
    SPP_OK = 0,
    SPP_ENOTOPEN = 1,
    SPP_EFULL = 2,
    SPP_EARG = 3,
    SPP_EBUSY = 4,
    SPP_EERROR = 5,
    SPP_EAGAIN = 6
};

enum { LC_CLOSED = 0, LC_OPEN = 1, LC_DRAINING = 2 };

struct spp_session {
    uint8_t opened;               /* all session state belongs to the owner thread */
    volatile uint8_t error;       /* set by callbacks; cleared by lifecycle reset */
    volatile uint8_t flow_paused; /* re-read by the pump before each chunk */
    uint8_t tx_pending;
    uint16_t rx_len;
    uint16_t tx_head, tx_tail, tx_count;
    uint8_t rx[SPP_RXBUF];
    uint8_t tx[SPP_TXRING];
};

static struct spp_session g_spp[SPP_NINST];
static void *g_hl;
static uint8_t g_active = SPP_NONE;
static uint8_t g_lifecycle = LC_CLOSED;
static volatile int g_in_drain;
static uint32_t g_pub_flags; /* published snapshot */

static struct spp_session *sess(uint32_t inst) {
    if (inst >= SPP_NINST)
        return 0;
    return &g_spp[inst];
}
static void spp_zero(struct spp_session *s) {
    uint32_t i;
    uint8_t *p = (uint8_t *)s;
    for (i = 0; i < sizeof(*s); i++)
        p[i] = 0;
}

/* ---- the single drain loop: the SOLE mutator of the TX queue (pump) ---- */
static void spp_drain(void *hl) {
    struct spp_session *s;
    int pumped;
    if (g_in_drain)
        return; /* reentrant -> do not pump; running drain continues */
    g_in_drain = 1;
    pumped = 0;
    s = (g_active != SPP_NONE) ? sess(g_active) : 0;
    if (s && s->opened) {
        /* At most SPP_PUMP_BATCH chunks per drain pass; re-read flow/error each chunk. */
        while (pumped < SPP_PUMP_BATCH && !s->flow_paused && !s->error && s->tx_count) {
            static uint8_t chunk[SPP_MAX_CHUNK];
            uint8_t u8inst = g_active;
            uint16_t len16;
            uint32_t n = s->tx_count, i, h = s->tx_head;
            if (n > SPP_MAX_CHUNK)
                n = SPP_MAX_CHUNK;
            for (i = 0; i < n; i++) {
                chunk[i] = s->tx[h];
                h = (h + 1) % SPP_TXRING;
            }
            len16 = (uint16_t)n;
            struct hif_param sp[3] = {{&u8inst, 1, 0}, {&len16, 2, 0}, {chunk, (uint16_t)n, 0}};
            BuildAndQueueMessage(hl, SPP_OPC_TX, 3u, sp, 0); /* may reenter a callback */
            s->tx_head = (uint16_t)h;
            s->tx_count = (uint16_t)(s->tx_count - n);
            pumped++;
        }
        if (s->error && s->tx_count) { /* error cleanup done HERE, not in the callback */
            s->tx_count = 0;
            s->tx_head = s->tx_tail;
        }
        s->tx_pending = (uint8_t)(!s->error && s->tx_count > 0);
    }
    /* publish snapshot */
    s = (g_active != SPP_NONE) ? sess(g_active) : 0;
    g_pub_flags = 0;
    if (s) {
        if (s->error)
            g_pub_flags |= SPP_F_ERROR;
        if (s->flow_paused)
            g_pub_flags |= SPP_F_FLOW;
        if (s->tx_pending)
            g_pub_flags |= SPP_F_TXPEND;
    }
    g_in_drain = 0;
}

static void spp_fail_set(struct spp_session *s) /* callback-side: SET only */
{
    s->error = 1;
}

/* ---- lifecycle ---- */
int waa_spp_open(void *hl, uint32_t inst) {
    struct spp_session *s = sess(inst);
    if (!s)
        return SPP_EARG;
    if (g_in_drain)
        return SPP_EBUSY; /* top-level only */
    if (g_lifecycle == LC_OPEN && g_active != inst)
        return SPP_EBUSY; /* refuse 2nd session */
    if (g_lifecycle == LC_DRAINING)
        return SPP_EAGAIN; /* reconnect barrier */
    spp_zero(s);
    s->opened = 1;
    g_active = (uint8_t)inst;
    g_lifecycle = LC_OPEN;
    g_hl = hl;
    if (hl)
        Log(hl, WAA_LOG_LEVEL, "WAA_SPP: open inst=%u\n", inst);
    spp_drain(hl);
    return SPP_OK;
}
int waa_spp_close(void *hl, uint32_t inst) {
    struct spp_session *s = sess(inst);
    if (!s)
        return SPP_EARG;
    if (g_in_drain)
        return SPP_EBUSY;
    spp_zero(s);
    if (g_active == inst) {
        g_active = SPP_NONE;
        g_lifecycle = LC_DRAINING;
    }
    if (hl)
        Log(hl, WAA_LOG_LEVEL, "WAA_SPP: close inst=%u -> DRAINING (await mark_drained)\n", inst);
    spp_drain(hl);
    return SPP_OK;
}
/* External confirmation that the transport flushed old events; permits reopen. */
int waa_spp_mark_drained(void *hl) {
    if (g_in_drain)
        return SPP_EBUSY;
    if (g_lifecycle == LC_DRAINING)
        g_lifecycle = LC_CLOSED;
    if (hl)
        Log(hl, WAA_LOG_LEVEL, "WAA_SPP: drained -> CLOSED (reopen permitted)\n");
    return SPP_OK;
}

/* ---- TX (top-level producer; sole appender) ---- */
int waa_spp_send(void *hl, uint32_t inst, const uint8_t *data, uint32_t len) {
    struct spp_session *s = sess(inst);
    uint32_t i, t;
    if (!s || !data)
        return SPP_EARG;
    if (g_in_drain)
        return SPP_EBUSY; /* send must be top-level */
    if (g_lifecycle != LC_OPEN || g_active != inst || !s->opened)
        return SPP_ENOTOPEN;
    if (s->error)
        return SPP_EERROR;
    if (len == 0)
        return SPP_OK;
    if (len > SPP_TXRING || s->tx_count + len > SPP_TXRING) {
        spp_fail_set(s);
        spp_drain(hl);
        return SPP_EFULL;
    }
    t = s->tx_tail;
    for (i = 0; i < len; i++) {
        s->tx[t] = data[i];
        t = (t + 1) % SPP_TXRING;
    }
    s->tx_tail = (uint16_t)t;
    s->tx_count = (uint16_t)(s->tx_count + len);
    spp_drain(hl);
    return SPP_OK;
}
void waa_spp_tick(void *hl, uint32_t inst) {
    (void)inst;
    spp_drain(hl);
}

/* ---- RX reassembly: rx_cb is the sole writer of rx[] ---- */
void waa_spp_rx_cb(void *ctx, uint32_t opcode, uint32_t param_count, const void *node) {
    struct spp_session *s;
    const uint8_t *data;
    uint32_t dlen, i;
    (void)ctx;
    (void)opcode;
    if (g_lifecycle != LC_OPEN || g_active == SPP_NONE)
        return; /* drop when not open */
    s = sess(g_active);
    if (!s || !s->opened || s->error || param_count < 3 || !node)
        return;
    data = PA_PTR(node, 2);
    dlen = PA_SIZE(node, 2);
    if (!dlen || !data)
        return;
    if (s->rx_len + dlen > SPP_RXBUF) {
        spp_fail_set(s);
        return;
    }
    for (i = 0; i < dlen; i++)
        s->rx[s->rx_len + i] = data[i];
    s->rx_len = (uint16_t)(s->rx_len + dlen);
}

/* ---- FLOW: set flow_paused, then drain ---- */
void waa_spp_flow_cb(void *ctx, uint32_t opcode, uint32_t param_count, const void *node) {
    void *hl = g_hl;
    const uint8_t *p0 = pa_byte(node, param_count, 0), *p1 = pa_byte(node, param_count, 1);
    uint32_t inst, value;
    struct spp_session *s;
    (void)ctx;
    (void)opcode;
    if (!p0 || !p1 || g_lifecycle != LC_OPEN)
        return;
    inst = p0[0];
    value = p1[0];
    if (inst != g_active || !(s = sess(inst)) || !s->opened || s->error)
        return;
    s->flow_paused = (uint8_t)(value != 0); /* atomic single byte; pump re-reads each chunk */
    spp_drain(hl);                          /* reentrant -> no pump; else pump on resume */
}

/* ---- TX error CFM (0x6200 status 2/3/7): SET error; drain does the TX-drop cleanup ---- */
void waa_spp_err_cb(void *ctx, uint32_t opcode, uint32_t param_count, const void *node) {
    void *hl = g_hl;
    const uint8_t *p0 = pa_byte(node, param_count, 0), *p1 = pa_byte(node, param_count, 1);
    uint32_t inst, status;
    struct spp_session *s;
    (void)ctx;
    (void)opcode;
    if (!p0 || !p1 || g_lifecycle != LC_OPEN)
        return;
    inst = p0[0];
    status = p1[0];
    if (status == 0 || inst != g_active || !(s = sess(inst)) || !s->opened)
        return;
    spp_fail_set(s); /* no TX touch here; cleanup in spp_drain */
    if (hl)
        Log(hl, WAA_LOG_LEVEL, "WAA_SPP: TX error status=%u -> session END (no retry)\n", status);
    spp_drain(hl);
}

/* ---- read + status (bootstrap-facing; error observable) ---- */
uint32_t waa_spp_read(uint32_t inst, uint8_t *dst, uint32_t max) {
    struct spp_session *s = sess(inst);
    uint32_t n, i;
    if (!s || !dst || max == 0)
        return 0;
    if (g_in_drain)
        return 0; /* top-level only */
    n = 0;
    if (g_lifecycle == LC_OPEN && s->opened && !s->error && s->rx_len) {
        n = s->rx_len;
        if (n > max)
            n = max;
        for (i = 0; i < n; i++)
            dst[i] = s->rx[i];
        for (i = n; i < s->rx_len; i++)
            s->rx[i - n] = s->rx[i];
        s->rx_len = (uint16_t)(s->rx_len - n);
    }
    return n; /* 0 => no data OR error: check waa_spp_status */
}
uint32_t waa_spp_status(uint32_t inst) {
    struct spp_session *s = sess(inst);
    uint32_t f = g_pub_flags;
    if (s && s->error)
        f |= SPP_F_ERROR; /* live error visibility */
    if (s)
        f = (f & 0xffffu) | ((uint32_t)s->rx_len << 16);
    return f;
}
int waa_spp_ready(uint32_t inst) {
    struct spp_session *s = sess(inst);
    return g_lifecycle == LC_OPEN && g_active == inst && s && s->opened && !s->error;
}
/* ---- registration ---- */
#ifndef WAA_SPP_RX_CALLBACK
#define WAA_SPP_RX_CALLBACK waa_spp_rx_cb
#define WAA_SPP_FLOW_CALLBACK waa_spp_flow_cb
#define WAA_SPP_ERROR_CALLBACK waa_spp_err_cb
#endif
void waa_spp_register(void *hifhandler) {
    void *hl = HL_OF(hifhandler);
    g_hl = hl;
    g_active = SPP_NONE;
    g_lifecycle = LC_CLOSED;
    RegisterCmd(hl, SPP_OPC_TX, 3, 6, SCHEMA_TX);
    /* PRIVACY: the stock BuildAndQueueMessage emits this UpdateCmd trace format on every send. The
     * WifiInfoResponse payload carries the Wi-Fi passphrase, so the format must NOT reference the
     * data (no "data=0x%s"): otherwise the key would be dumped to the HIF log. Only instance/len
     * are traced. The HifLib's own send-logger line logs size, not payload. (ARM phone fixture
     * T_privacy asserts no TX-path log format references the payload.) */
    UpdateCmd(hl, SPP_OPC_TX, 3, "WAA_SPP_TX", "instance=%d, len=%d", 0);
    RegisterCmd(hl, SPP_OPC_TX, 1, 6, SCHEMA_TXCFM);
    RegisterHandler(hl, SPP_OPC_TX, (void *)WAA_SPP_ERROR_CALLBACK);
    RegisterCmd(hl, SPP_OPC_RX, 2, 6, SCHEMA_RX);
    RegisterHandler(hl, SPP_OPC_RX, (void *)WAA_SPP_RX_CALLBACK);
    RegisterCmd(hl, SPP_OPC_FLOW, 2, 6, SCHEMA_FLOW);
    RegisterHandler(hl, SPP_OPC_FLOW, (void *)WAA_SPP_FLOW_CALLBACK);
    Log(hl, WAA_LOG_LEVEL, "WAA_SPP: registered TX 0x6200(kind3)+CFM, RX 0x6300, FLOW 0x6302\n");
}
