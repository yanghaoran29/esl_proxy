#ifndef ESL_PROXY_ESL_SWIMLANE_HOST_H
#define ESL_PROXY_ESL_SWIMLANE_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void esl_swimlane_host_set_level(int level);
int esl_swimlane_host_init(int worker_count, int aicpu_thread_num, int device_id, const char *output_prefix);
void esl_swimlane_host_start(void);
void esl_swimlane_host_stop_and_export(void);
void esl_swimlane_host_finalize(void);
uint64_t esl_swimlane_host_data_base(void);
uint64_t esl_swimlane_host_rotation_table(void);
void esl_swimlane_host_set_core_types(const int32_t *core_types, int count);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ESL_SWIMLANE_HOST_H */
