#ifndef ESL_PROXY_FAKE_AICORE_REGS_H
#define ESL_PROXY_FAKE_AICORE_REGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int esl_fake_aicore_regs_init(void);
void esl_fake_aicore_regs_shutdown(void);
uint32_t esl_fake_aicore_core_count(void);
uint64_t esl_fake_aicore_reg_addr(int core);

/** Write DATA_MAIN_BASE + ACK/FIN to simulated regs (instant complete). */
int esl_fake_aicore_dispatch(int core, uint32_t task_id);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_FAKE_AICORE_REGS_H */
