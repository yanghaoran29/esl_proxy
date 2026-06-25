/*
 * platform_sim.c — host CPU sim backend for platform.h
 */
#include "platform.h"

#include <stddef.h>

#include "fake_aicore_host.h"
#include "worker_map.h"

void platform_init_from_env(void)
{
    host_fake_aicore_configure_from_env();
}

int platform_fake_kernel_enabled(void)
{
    return host_fake_aicore_kernel_enabled();
}

void platform_workers_start(int worker_count)
{
    host_fake_aicore_start(worker_count);
}

void platform_workers_stop(void)
{
    host_fake_aicore_stop();
}

int platform_pick_phys_worker(int core, int exe_type)
{
    return esl_pick_phys_worker(core, exe_type);
}

int platform_dispatch_block(uint16_t task_id, int exe_type, int core, int slot, uint64_t mask,
                            uint32_t duration_ns, uint32_t jitter_mask, uint16_t *phys_out)
{
    if (host_fake_aicore_kernel_enabled()) {
        const int phys = esl_pick_phys_worker(core, exe_type);
        if (phys_out != NULL) {
            *phys_out = (uint16_t)phys;
        }
        if (phys < 0) {
            return -1;
        }
        return host_fake_aicore_submit(phys, task_id, exe_type, core, slot, mask, duration_ns,
                                       jitter_mask);
    }
    return host_fake_aicore_finish_sync(task_id, exe_type, core, slot, mask);
}

int platform_pop_completion(PlatformCompletion *out)
{
    HostFakeFin fin;

    if (out == NULL) {
        return -1;
    }
    if (host_fake_fin_pop(&fin) != 0) {
        return -1;
    }
    out->task_id = fin.task_id;
    out->exe_type = fin.exe_type;
    out->core = fin.core;
    out->slot = fin.slot;
    out->mask = fin.mask;
    return 0;
}
