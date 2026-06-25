/*
 * dispatch_payload.h — PTO2-aligned dispatch-time payload build (Host + onboard).
 */
#ifndef ESL_PROXY_DISPATCH_PAYLOAD_H
#define ESL_PROXY_DISPATCH_PAYLOAD_H

#include <stdint.h>

#include "onboard/onboard_config.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Materialize payload metadata at task submit (owner_task_id, etc.). */
void task_payload_materialize(uint16_t task_id);

/* Build per-core GM payload before reg kick. block_idx is the SPMD sub-block index. */
void esl_build_dispatch_payload(EslFakeDispatchPayload *out, const struct task_desc *desc,
                                const struct task_payload *pay, uint32_t block_idx);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_DISPATCH_PAYLOAD_H */
