/* aicpu_platform.c — onboard AICPU platform primitives.
 *
 * Pure NPU bring-up + AICore handshake: register access, cache ops, HW
 * dispatch primitives. Onboard-only — this file must
 * not include any non-onboard (functional) header.
 */
#define _GNU_SOURCE

#include "onboard_config.h"
#include "tools.h"
#include "onboard_log.h"
#include "aicpu_bridge.h"
#include "kernel_args.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __aarch64__
#define OUT_OF_ORDER_STORE_BARRIER() __asm__ __volatile__("dmb ishst" ::: "memory")
#else
#define OUT_OF_ORDER_STORE_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

#define AICPU_CORES_PER_CHIP 8
#define MAX_CLUSTERS 2
#define CPUS_PER_CLUSTER 4
#define MAX_GATE_THREADS 16
#define ESL_FAKE_DUR_MAX_TICKS 200
#define HANDSHAKE_SPIN_MAX 50000000ULL
#define DEINIT_ACK_SPIN_MAX 5000000ULL
#define ESL_FAKE_DUR_DIV_THRESHOLD 10000U
#define ESL_FAKE_DUR_MIN_TICKS 1
#define TL_FILTER_EXEC_IDX_UNSET (-1)

static uint64_t g_platform_regs;
static EslRuntime *g_runtime;
static uint64_t g_core_reg_addrs[RUNTIME_MAX_WORKER];

static atomic_uint_fast64_t s_cpumask;
static atomic_int_fast32_t s_reported;
static atomic_int_fast32_t s_gate_init;
static atomic_int_fast32_t s_gate_ready;
static atomic_int_fast32_t s_cleanup;
static int32_t s_thread_cpu[MAX_GATE_THREADS];
static bool s_thread_survive[MAX_GATE_THREADS];
static _Thread_local int32_t tl_filter_exec_idx = TL_FILTER_EXEC_IDX_UNSET;
static atomic_int_fast32_t s_filter_claim;
static atomic_int_fast32_t s_filter_published;
static atomic_int_fast32_t s_filter_classify_init;
static atomic_int_fast32_t s_filter_classify_ready;
static atomic_int_fast32_t s_filter_cleanup;
static int32_t s_filter_thread_cpu[MAX_GATE_THREADS];
static int32_t s_filter_thread_exec_idx[MAX_GATE_THREADS];

/* ========================================================================== */
/* Platform registers & cache (aicpu_bridge.h)                                */
/* ========================================================================== */

void set_platform_regs(uint64_t regs)
{
    g_platform_regs = regs;
}

uint64_t get_platform_regs(void)
{
    return g_platform_regs;
}

volatile uint32_t *get_reg_ptr(uint64_t reg_base_addr, RegId reg)
{
    return (volatile uint32_t *)(uintptr_t)(reg_base_addr + reg_offset(reg));
}

uint64_t read_reg(uint64_t reg_base_addr, RegId reg)
{
    return (uint64_t)*get_reg_ptr(reg_base_addr, reg);
}

void write_reg(uint64_t reg_base_addr, RegId reg, uint64_t value)
{
    *get_reg_ptr(reg_base_addr, reg) = (uint32_t)value;
}

void platform_init_aicore_regs(uint64_t reg_addr)
{
    write_reg(reg_addr, REG_ID_FAST_PATH_ENABLE, REG_SPR_FAST_PATH_OPEN);
    write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICPU_IDLE_TASK_ID);
}

void cache_invalidate_range(const void *addr, size_t size)
{
    const size_t k_cache_line_size = 64;
    uintptr_t start;
    uintptr_t end;
    uintptr_t p;

    if (size == 0) {
        return;
    }
    start = (uintptr_t)addr & ~(k_cache_line_size - 1);
    end = ((uintptr_t)addr + size + k_cache_line_size - 1) & ~(k_cache_line_size - 1);
    for (p = start; p < end; p += k_cache_line_size) {
        __asm__ __volatile__("dc civac, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

void cache_flush_range(const void *addr, size_t size)
{
    const size_t k_cache_line_size = 64;
    uintptr_t start;
    uintptr_t end;
    uintptr_t p;

    if (size == 0) {
        return;
    }
    start = (uintptr_t)addr & ~(k_cache_line_size - 1);
    end = ((uintptr_t)addr + size + k_cache_line_size - 1) & ~(k_cache_line_size - 1);
    for (p = start; p < end; p += k_cache_line_size) {
        __asm__ __volatile__("dc cvac, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

/* ========================================================================== */
/* AICPU affinity gate (aicpu_bridge.h; not wired in esl_proxy smoke yet)   */
/* ========================================================================== */

bool platform_aicpu_affinity_gate(int32_t logical_count, int32_t total_launched)
{
    int32_t idx;
    int32_t cpu;
    int32_t normalized_cpu;
    int_fast32_t expected;
    int32_t cleanup_idx;
    bool survive;

    if (logical_count >= total_launched) {
        return true;
    }
    idx = atomic_fetch_add_explicit(&s_reported, 1, memory_order_acq_rel);
#if defined(__aarch64__) || defined(__x86_64__)
    cpu = sched_getcpu();
#else
    cpu = -1;
#endif
    normalized_cpu = -1;
    if (cpu >= 0) {
        if (cpu < 63) {
            atomic_fetch_or_explicit(&s_cpumask, 1ULL << cpu, memory_order_release);
        }
        normalized_cpu = cpu % AICPU_CORES_PER_CHIP;
    }
    if (idx < MAX_GATE_THREADS) {
        s_thread_cpu[idx] = normalized_cpu;
    }
    while (popcount64(atomic_load_explicit(&s_cpumask, memory_order_acquire)) < total_launched &&
           atomic_load_explicit(&s_reported, memory_order_acquire) < total_launched) {
    }
    expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &s_gate_init, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
        int32_t i;
        int32_t tid;
        int32_t major_id;
        int32_t minor_id;
        int32_t major_cnt;
        int32_t minor_cnt;
        struct ClusterInfo {
            int32_t count;
            int32_t tids[MAX_GATE_THREADS];
        } clusters[MAX_CLUSTERS] = {{0}};

        for (i = 0; i < total_launched; ++i) {
            s_thread_survive[i] = false;
        }
        for (tid = 0; tid < total_launched; ++tid) {
            int32_t c = s_thread_cpu[tid];
            int32_t cluster_id;

            if (c < 0) {
                continue;
            }
            cluster_id = c / CPUS_PER_CLUSTER;
            if (cluster_id < 0 || cluster_id >= MAX_CLUSTERS) {
                continue;
            }
            if (clusters[cluster_id].count < MAX_GATE_THREADS) {
                clusters[cluster_id].tids[clusters[cluster_id].count++] = tid;
            }
        }
        major_id = (clusters[0].count >= clusters[1].count) ? 0 : 1;
        minor_id = 1 - major_id;
        major_cnt = clusters[major_id].count;
        minor_cnt = clusters[minor_id].count;
        LOG_INFO_V0("AICPU affinity gate: major=%d(cnt=%d) minor=%d(cnt=%d) logical=%d", major_id, major_cnt,
                    minor_id, minor_cnt, logical_count);
        if (major_cnt == logical_count && minor_cnt == (total_launched - logical_count)) {
            for (i = 0; i < clusters[major_id].count; ++i) {
                s_thread_survive[clusters[major_id].tids[i]] = true;
            }
        } else {
            LOG_WARN(
                "AICPU affinity gate: unexpected topology (major=%d minor=%d), "
                "falling back to index-based cutoff",
                major_cnt, minor_cnt);
            for (i = 0; i < logical_count && i < total_launched; ++i) {
                s_thread_survive[i] = true;
            }
        }
        atomic_store_explicit(&s_gate_ready, 1, memory_order_release);
    }
    while (atomic_load_explicit(&s_gate_ready, memory_order_acquire) == 0) {
    }
    survive = (idx < total_launched) ? s_thread_survive[idx] : false;
    cleanup_idx = atomic_fetch_add_explicit(&s_cleanup, 1, memory_order_acq_rel);
    if (cleanup_idx + 1 == total_launched) {
        atomic_store_explicit(&s_cpumask, 0, memory_order_release);
        atomic_store_explicit(&s_reported, 0, memory_order_release);
        atomic_store_explicit(&s_gate_init, 0, memory_order_release);
        atomic_store_explicit(&s_gate_ready, 0, memory_order_release);
        atomic_store_explicit(&s_cleanup, 0, memory_order_release);
    }
    if (!survive) {
        LOG_INFO_V0("AICPU affinity gate: thread idx=%d cpu=%d DROPPED", idx, normalized_cpu);
    } else {
        LOG_INFO_V0("AICPU affinity gate: thread idx=%d cpu=%d ACTIVE", idx, normalized_cpu);
    }
    return survive;
}

bool platform_aicpu_affinity_gate_filter(const int32_t *allowed_cpus, int32_t allowed_count, int32_t total_launched)
{
    int32_t idx;
    int32_t cpu;
    int_fast32_t expected;
    bool survive;

    tl_filter_exec_idx = TL_FILTER_EXEC_IDX_UNSET;
    if (allowed_cpus == NULL || allowed_count <= 0 || allowed_count > MAX_GATE_THREADS || total_launched <= 0 ||
        total_launched > MAX_GATE_THREADS) {
        LOG_ERROR(
            "AICPU filter gate: invalid config allowed_count=%d total_launched=%d (max=%d) — dropping all threads",
            allowed_count, total_launched, MAX_GATE_THREADS);
        return false;
    }
    idx = atomic_fetch_add_explicit(&s_filter_claim, 1, memory_order_acq_rel);
#if defined(__aarch64__) || defined(__x86_64__)
    cpu = sched_getcpu();
#else
    cpu = -1;
#endif
    if (idx < MAX_GATE_THREADS) {
        s_filter_thread_cpu[idx] = cpu;
    }
    atomic_fetch_add_explicit(&s_filter_published, 1, memory_order_release);
    while (atomic_load_explicit(&s_filter_published, memory_order_acquire) < total_launched) {
    }
    expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &s_filter_classify_init, &expected, 1, memory_order_acq_rel, memory_order_acquire)) {
        int32_t i;
        int32_t tid;
        bool slot_filled[MAX_GATE_THREADS] = {false};

        for (i = 0; i < total_launched && i < MAX_GATE_THREADS; ++i) {
            s_filter_thread_exec_idx[i] = TL_FILTER_EXEC_IDX_UNSET;
        }
        for (tid = 0; tid < total_launched && tid < MAX_GATE_THREADS; ++tid) {
            int32_t my_cpu = s_filter_thread_cpu[tid];
            int32_t a;

            if (my_cpu < 0) {
                continue;
            }
            for (a = 0; a < allowed_count && a < MAX_GATE_THREADS; ++a) {
                if (allowed_cpus[a] == my_cpu && !slot_filled[a]) {
                    s_filter_thread_exec_idx[tid] = a;
                    slot_filled[a] = true;
                    break;
                }
            }
        }
        LOG_INFO_V0("AICPU filter gate: allowed_count=%d total_launched=%d", allowed_count, total_launched);
        for (i = 0; i < allowed_count; ++i) {
            const char *role = (i == allowed_count - 1) ? "orch" : "sched";

            LOG_INFO_V0("AICPU filter gate:   allowed[%d] = cpu_id %d  role=%s", i, allowed_cpus[i], role);
        }
        atomic_store_explicit(&s_filter_classify_ready, 1, memory_order_release);
    }
    while (atomic_load_explicit(&s_filter_classify_ready, memory_order_acquire) == 0) {
    }
    if (idx < total_launched && idx < MAX_GATE_THREADS) {
        tl_filter_exec_idx = s_filter_thread_exec_idx[idx];
        survive = (tl_filter_exec_idx >= 0);
    } else {
        tl_filter_exec_idx = TL_FILTER_EXEC_IDX_UNSET;
        survive = false;
    }
    if (atomic_fetch_add_explicit(&s_filter_cleanup, 1, memory_order_acq_rel) + 1 == total_launched) {
        atomic_store_explicit(&s_filter_claim, 0, memory_order_release);
        atomic_store_explicit(&s_filter_published, 0, memory_order_release);
        atomic_store_explicit(&s_filter_classify_init, 0, memory_order_release);
        atomic_store_explicit(&s_filter_classify_ready, 0, memory_order_release);
        atomic_store_explicit(&s_filter_cleanup, 0, memory_order_release);
    }
    if (survive) {
        const char *role = (tl_filter_exec_idx == allowed_count - 1) ? "orch" : "sched";

        LOG_INFO_V0("AICPU filter gate: thread idx=%d cpu=%d exec_idx=%d ACTIVE(%s)", idx, cpu, tl_filter_exec_idx,
                    role);
    } else {
        LOG_INFO_V0("AICPU filter gate: thread idx=%d cpu=%d DROPPED", idx, cpu);
    }
    return survive;
}

int32_t platform_aicpu_affinity_thread_idx(void)
{
    return tl_filter_exec_idx;
}

/* ========================================================================== */
/* HW dispatch & AICore handshake                                             */
/* ========================================================================== */

int esl_hw_poll_fin(uint64_t reg_addr, uint16_t task_id)
{
    uint64_t cond;

    if (reg_addr == 0) {
        return 0;
    }
    cond = read_reg(reg_addr, REG_ID_COND);
    return (EXTRACT_TASK_STATE(cond) == TASK_FIN_STATE && EXTRACT_TASK_ID(cond) == (int)task_id) ? 1 : 0;
}

void esl_hw_dispatch_reg(uint64_t reg_addr, uint16_t task_id)
{
    if (reg_addr != 0) {
        write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, task_id);
    }
}

void esl_dispatch_payload_init(EslRuntime *runtime)
{
    g_runtime = runtime;
}

static int64_t scale_duration_ticks(uint32_t raw)
{
    int64_t d = (raw > ESL_FAKE_DUR_DIV_THRESHOLD) ? (int64_t)(raw / 100U)
                                                   : (raw == 0U ? ESL_FAKE_DUR_MIN_TICKS : (int64_t)raw);

    if (d > ESL_FAKE_DUR_MAX_TICKS) {
        d = ESL_FAKE_DUR_MAX_TICKS;
    }
    return d;
}

void esl_dispatch_payload_prepare(int core, uint16_t task_id, const EslOnboardDispatchInput *input)
{
    EslFakeDispatchPayload *p;
    uint64_t base;
    int slot;
    uint16_t tc;
    uint16_t sc;
    int i;

    if (g_runtime == NULL || core < 0 || core >= RUNTIME_MAX_WORKER || input == NULL) {
        return;
    }
    base = g_runtime->workers[core].task;
    if (base == 0) {
        return;
    }
    slot = (int)(task_id & 1u);
    p = (EslFakeDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslFakeDispatchPayload));

    memset(p, 0, sizeof(*p));
    p->task = input->task;
    p->duration_ticks = scale_duration_ticks(input->task.duration);
    p->task_id_mask = (int64_t)task_id;

    tc = input->task.tensor_cnt;
    sc = input->task.scalar_cnt;
    if (tc > ESL_ONBOARD_MAX_TENSOR_ARGS) {
        tc = ESL_ONBOARD_MAX_TENSOR_ARGS;
    }
    if (sc > ESL_ONBOARD_MAX_SCALAR_ARGS) {
        sc = ESL_ONBOARD_MAX_SCALAR_ARGS;
    }

    for (i = 0; i < (int)tc; ++i) {
        p->tensors[i].buffer_addr = input->tensor_addrs[i];
        p->args[i] = (uint64_t)(uintptr_t)&p->tensors[i];
    }
    for (i = 0; i < (int)sc; ++i) {
        p->args[tc + (uint16_t)i] = (uint64_t)input->scalars[i];
    }

    cache_flush_range(p, sizeof(EslFakeDispatchPayload));
}

static int wait_handshake_field(volatile uint32_t *field, uint32_t expect)
{
    uint64_t spins;

    for (spins = 0; spins < HANDSHAKE_SPIN_MAX; ++spins) {
        cache_invalidate_range((const void *)field, sizeof(*field));
        if (*field == expect) {
            return 1;
        }
    }
    return 0;
}

int esl_handshake_all_cores(EslRuntime *runtime)
{
    uint64_t *regs;
    uint64_t regs_table;
    int n;
    int n_handshake;
    int i;

    if (runtime == NULL) {
        return -1;
    }
    n = runtime->worker_count;
    if (n <= 0 || n > RUNTIME_MAX_WORKER) {
        LOG_ERROR("Invalid worker_count %d", n);
        return -1;
    }
    regs_table = get_platform_regs();
    if (regs_table == 0) {
        LOG_ERROR("Platform regs table not set");
        return -1;
    }
    regs = (uint64_t *)(uintptr_t)regs_table;
    n_handshake = n;
    LOG_INFO_V0("esl_proxy handshake for %d workers", n_handshake);
    for (i = 0; i < n_handshake; i++) {
        EslHandshake *wk = &runtime->workers[i];

        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicpu_ready = 1;
        cache_flush_range((const void *)wk, sizeof(EslHandshake));
    }
    for (i = 0; i < n_handshake; i++) {
        EslHandshake *wk = &runtime->workers[i];
        uint32_t phys;
        uint64_t reg_addr;

        if (!wait_handshake_field(&wk->aicore_regs_ready, 1)) {
            LOG_ERROR("Core %d aicore_regs_ready timeout", i);
            return -1;
        }
        cache_invalidate_range((const void *)&wk->physical_core_id, sizeof(wk->physical_core_id));
        phys = wk->physical_core_id;
        if (phys >= RUNTIME_MAX_WORKER) {
            LOG_ERROR("Core %d invalid physical_core_id=%u", i, phys);
            continue;
        }
        reg_addr = regs[phys];
        g_core_reg_addrs[i] = reg_addr;
        platform_init_aicore_regs(reg_addr);
        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicpu_regs_ready = 1;
        cache_flush_range((const void *)&wk->aicpu_regs_ready, sizeof(wk->aicpu_regs_ready));
        if (!wait_handshake_field((volatile uint32_t *)&wk->aicore_done, (uint32_t)(i + 1))) {
            LOG_ERROR("Core %d aicore_done timeout", i);
            return -1;
        }
    }
    return 0;
}

uint64_t esl_handshake_reg_addr(int core_idx)
{
    if (core_idx < 0 || core_idx >= RUNTIME_MAX_WORKER) {
        return 0;
    }
    return g_core_reg_addrs[core_idx];
}

void esl_shutdown_all_cores(EslRuntime *runtime)
{
    uint64_t *regs;
    uint64_t regs_table;
    int n;
    int n_shutdown;
    int i;

    if (runtime == NULL) {
        return;
    }
    n = runtime->worker_count;
    if (n <= 0 || n > RUNTIME_MAX_WORKER) {
        return;
    }
    n_shutdown = n;
    LOG_INFO_V0("esl_proxy shutting down %d AICore workers", n_shutdown);
    regs_table = get_platform_regs();
    regs = regs_table ? (uint64_t *)(uintptr_t)regs_table : NULL;
    for (i = 0; i < n_shutdown; i++) {
        uint64_t reg_addr = g_core_reg_addrs[i];

        if (reg_addr == 0 && regs != NULL) {
            reg_addr = regs[i];
        }
        if (reg_addr != 0) {
            write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICORE_EXIT_SIGNAL);
        }
    }
    for (i = 0; i < n_shutdown; i++) {
        uint64_t reg_addr = g_core_reg_addrs[i];
        uint64_t spins;

        if (reg_addr == 0 && regs != NULL) {
            reg_addr = regs[i];
        }
        if (reg_addr == 0) {
            continue;
        }
        spins = 0;
        while (read_reg(reg_addr, REG_ID_COND) != AICORE_EXITED_VALUE) {
            if (++spins > DEINIT_ACK_SPIN_MAX) {
                LOG_ERROR("Core %d deinit ack timeout (best-effort, continuing)", i);
                break;
            }
        }
    }
}
