#ifndef WAA_HIF_H
#define WAA_HIF_H
#include <stdint.h>
#include <stddef.h>
#include "profile_generated.h"
/* ---- Verified stock ABIs (HifLib.cpp / HifHandler), ET_EXEC absolute ARM ---- */

/* HifLib::RegisterCmd(HifLib* hl, u16 opcode, int kind, int group, const char* schema).
 * kind 0/3 -> m_mapSendCmd, kind 1/2 -> m_mapRecvCmd. Schema numeric tokens are PARAM TYPE
 * CODES (leading number of each field). RegisterCmd ALONE does not set a confirm timeout. */
typedef void (*register_cmd_t)(void *hl, uint32_t opcode, int kind, int group, const char *schema);
#define RegisterCmd ((register_cmd_t)WA_REGISTER_CMD)

/* UpdateCmd requires a registered command with an empty name slot.
 * Set a nonzero request timeout so the stock confirmation timer is armed. */
typedef void (*update_cmd_t)(void *hl, uint32_t opcode, int kind, const char *name, const char *fmt,
                             int confirm_timeout_ms);
#define UpdateCmd ((update_cmd_t)WA_UPDATE_CMD)

/* HifLib::RegisterHandler(HifLib* hl, u16 opcode, ResultFunc func). Asserts opcode ALREADY in
 * m_mapRecvCmd (RegisterCmd first) and the slot is empty. */
typedef void (*register_handler_t)(void *hl, uint32_t opcode, void *func);
#define RegisterHandler ((register_handler_t)WA_REGISTER_HANDLER)

/* BuildAndQueueMessage requires a registered send command. It copies parameters
 * into an owned queued PDU; numeric fields use schema byte order, data fields copy raw bytes. */
typedef void (*build_and_queue_t)(void *hl, uint32_t code, uint32_t param_count, const void *params,
                                  char log_flag);
#define BuildAndQueueMessage ((build_and_queue_t)WA_QUEUE_MESSAGE)

/* Stock variadic logger; object and function address come from the profile. */
typedef void (*log_t)(void *obj, int level, const char *fmt, ...);
#define Log ((log_t)WA_LOG)
#define WAA_LOG_LEVEL 1

/* HifHandler layout: +0x14 = HifLib*. Recv callback ctx == HifHandler. */
#define HL_OF(handler) (*(void **)((char *)(handler) + WA_HIFLIB))

/* send_param entry consumed by BuildAndQueueMessage's raw branch: {void* ptr; u16 size;}. */
struct hif_param {
    const void *ptr;
    uint16_t size;
    uint16_t pad;
};
_Static_assert(sizeof(struct hif_param) == WA_PARAM_STRIDE &&
                   offsetof(struct hif_param, size) == WA_PARAM_SIZE,
               "HIF parameter ABI differs from profile");

/* Receive parameter pointers use the profile-selected entry stride. */
#define PA_PTR(node, i) (*(uint8_t *const *)((const char *)(node) + (i) * WA_PARAM_STRIDE))

#define PA_SIZE(node, i)                                                                           \
    (*(const uint16_t *)((const char *)(node) + (i) * WA_PARAM_STRIDE + WA_PARAM_SIZE))

/* Parameter i as a readable byte, or NULL when the node, entry or bytes are missing. */
static inline const uint8_t *pa_byte(const void *node, uint32_t count, uint32_t i) {
    return node && i < count && PA_SIZE(node, i) ? PA_PTR(node, i) : 0;
}

#endif
