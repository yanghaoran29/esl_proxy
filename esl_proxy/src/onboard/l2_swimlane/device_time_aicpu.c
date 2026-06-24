#include "tools.h"
#include "aicpu/device_time.h"

uint64_t get_sys_cnt_aicpu(void)
{
    return esl_onboard_sys_cnt();
}
