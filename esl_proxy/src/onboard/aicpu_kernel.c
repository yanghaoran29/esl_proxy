/* aicpu_kernel.c — merged AICPU onboard sources (platform + glue + runtime + orch) */
#define _GNU_SOURCE

#include "onboard_config.h"
#include "tools.h"
#include "dlog_pub.h"
#include "aicpu_bridge.h"
#include "kernel_args.h"
#include "conf.h"
#include "dispatch.h"
#include "ring_buf.h"
#include "task.h"
#include "cutter.h"
#include "mem_pool.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* File-local config                                                          */
/* -------------------------------------------------------------------------- */

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
#define ONBOARD_POOL_BASE ((void *)0x40000000000ULL)
#define ONBOARD_POOL_SIZE (64ULL * 1024 * 1024 * 1024)
#define ONBOARD_WHEN2FREE_CAP 4096
#ifndef ORCH_CASE
#define ORCH_CASE paged_attention_unroll_manual_scope.h
#endif
#define INCLUDE(x) #x
#define INCLUDE_FILE(x) INCLUDE(x)
#define HANDSHAKE_SPIN_MAX 50000000ULL
#define DEINIT_ACK_SPIN_MAX 5000000ULL
#define ESL_FAKE_DUR_DIV_THRESHOLD 10000U
#define ESL_FAKE_DUR_MIN_TICKS 1
#define TL_FILTER_EXEC_IDX_UNSET (-1)

/* -------------------------------------------------------------------------- */
/* Forward declarations & cross-TU externs                                    */
/* -------------------------------------------------------------------------- */

void aicpu_orchestration_entry(uint64_t orch_args);
void esl_signal_orch_done(void);
void esl_singlethread_drive(void);
int esl_platform_init(EslRuntime *runtime, AicoreBridge *bridge);
void esl_platform_shutdown(AicoreBridge *bridge);
void init_predecessors(void);

extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern task_state *g_state_buf;
extern atomic_int g_completed_cnt;
extern atomic_bool g_orch_is_done;
extern atomic_bool g_is_done;
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

/* -------------------------------------------------------------------------- */
/* Static globals — platform regs                                             */
/* -------------------------------------------------------------------------- */

static uint64_t g_platform_regs;

/* -------------------------------------------------------------------------- */
/* Static globals — affinity gate (reserved for multi-AICPU launch)           */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/* Static globals — bridge / runtime / init                                   */
/* -------------------------------------------------------------------------- */

static EslRuntime *g_runtime;
static uint64_t g_core_reg_addrs[RUNTIME_MAX_WORKER];
static atomic_int g_thread_idx;
static atomic_bool g_init_done;
static atomic_bool g_init_failed;
static atomic_flag g_once = ATOMIC_FLAG_INIT;
static AicoreBridge g_bridge;

/* -------------------------------------------------------------------------- */
/* Static globals — stats / mem pool                                          */
/* -------------------------------------------------------------------------- */

static uint64_t g_device_start_cycle;
static when2free_entry_t g_onboard_when2free[ONBOARD_WHEN2FREE_CAP];
volatile uint64_t *g_esl_stats_base;

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
/* AICPU logging (tools.h implementation)                                     */
/* ========================================================================== */

bool g_is_log_enable_debug = false;
bool g_is_log_enable_info = false;
bool g_is_log_enable_warn = false;
bool g_is_log_enable_error = false;
int g_log_info_v = 5;

void init_log_switch(void)
{
    g_is_log_enable_debug = CheckLogLevel(AICPU, DLOG_DEBUG);
    g_is_log_enable_info = CheckLogLevel(AICPU, DLOG_INFO);
    g_is_log_enable_warn = CheckLogLevel(AICPU, DLOG_WARN);
    g_is_log_enable_error = CheckLogLevel(AICPU, DLOG_ERROR);
}

void set_log_level(int level)
{
    (void)level;
}

void set_log_info_v(int v)
{
    if (v < 0) {
        v = 0;
    }
    if (v > 9) {
        v = 9;
    }
    g_log_info_v = v;
}

int get_log_info_v(void)
{
    return g_log_info_v;
}

static void dev_vlog_emit(int level, int info_v, const char *func, const char *fmt, va_list args)
{
    char buffer[2048];

    vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (level == DLOG_DEBUG) {
        dlog_debug(AICPU, "%lu %s\n\"%s\"", GET_TID(), func, buffer);
    } else if (level == DLOG_WARN) {
        dlog_warn(AICPU, "%lu %s\n\"%s\"", GET_TID(), func, buffer);
    } else if (level == DLOG_ERROR) {
        dlog_error(AICPU, "%lu %s\n\"%s\"", GET_TID(), func, buffer);
    } else {
        dlog_info(AICPU, "%lu %s [V%d]\n\"%s\"", GET_TID(), func, info_v, buffer);
    }
}

void dev_vlog_debug(const char *func, const char *fmt, va_list args)
{
    dev_vlog_emit(DLOG_DEBUG, 0, func, fmt, args);
}

void dev_vlog_warn(const char *func, const char *fmt, va_list args)
{
    dev_vlog_emit(DLOG_WARN, 0, func, fmt, args);
}

void dev_vlog_error(const char *func, const char *fmt, va_list args)
{
    dev_vlog_emit(DLOG_ERROR, 0, func, fmt, args);
}

void dev_vlog_info_v(int v, const char *func, const char *fmt, va_list args)
{
    dev_vlog_emit(DLOG_INFO, v, func, fmt, args);
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

void esl_dispatch_payload_prepare(int core, uint16_t task_id, uint32_t raw_duration)
{
    EslFakeTaskArgs *p;
    uint64_t base;
    int slot;

    if (g_runtime == NULL || core < 0 || core >= RUNTIME_MAX_WORKER) {
        return;
    }
    base = g_runtime->workers[core].task;
    if (base == 0) {
        return;
    }
    slot = (int)(task_id & 1u);
    p = (EslFakeTaskArgs *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslFakeTaskArgs));
    p->duration = scale_duration_ticks(raw_duration);
    p->mask = (int64_t)task_id;
    cache_flush_range(p, sizeof(EslFakeTaskArgs));
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

/* ========================================================================== */
/* Runtime execute path                                                       */
/* ========================================================================== */

static int init_once(EslRuntime *runtime)
{
    if (atomic_flag_test_and_set_explicit(&g_once, memory_order_acquire)) {
        while (!atomic_load_explicit(&g_init_done, memory_order_acquire) &&
               !atomic_load_explicit(&g_init_failed, memory_order_acquire)) {
        }
        return atomic_load_explicit(&g_init_failed, memory_order_acquire) ? -1 : 0;
    }
    if (esl_platform_init(runtime, &g_bridge) != 0) {
        atomic_store_explicit(&g_init_failed, true, memory_order_release);
        return -1;
    }
    if (get_platform_regs() != 0) {
        if (esl_handshake_all_cores(runtime) != 0) {
            atomic_store_explicit(&g_init_failed, true, memory_order_release);
            return -1;
        }
    }
    atomic_store_explicit(&g_init_done, true, memory_order_release);
    return 0;
}

int32_t esl_aicpu_execute(EslRuntime *runtime)
{
    int idx;

    if (runtime == NULL) {
        return -1;
    }
    if (init_once(runtime) != 0) {
        return -1;
    }
    esl_onboard_invalidate_runtime(runtime);
    idx = atomic_fetch_add_explicit(&g_thread_idx, 1, memory_order_acq_rel);
    if (idx != 0) {
        return 0;
    }
    aicpu_orchestration_entry(0);
    esl_signal_orch_done();
    esl_singlethread_drive();
    esl_platform_shutdown(&g_bridge);
    return 0;
}

void esl_write_stats(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt, uint64_t commit,
                     uint64_t ready_cube, uint64_t ready_vec, uint64_t min_uncomplete)
{
    if (g_esl_stats_base != NULL) {
        g_esl_stats_base[0] = task_cnt;
        g_esl_stats_base[1] = subtask_cnt;
        g_esl_stats_base[2] = completed_cnt;
        g_esl_stats_base[4] = commit;
        g_esl_stats_base[5] = ready_cube;
        g_esl_stats_base[6] = ready_vec;
        g_esl_stats_base[7] = min_uncomplete;
        cache_flush_range((const void *)g_esl_stats_base, 8 * sizeof(uint64_t));
    }
}

/* ========================================================================== */
/* CANN kernel entry points                                                   */
/* ========================================================================== */

__attribute__((visibility("default"))) int simpler_aicpu_init(void *arg)
{
    KernelArgs *k_args;

    init_log_switch();
    if (arg == NULL) {
        LOG_ERROR("%s", "Invalid kernel arguments: null pointer");
        return -1;
    }
    k_args = (KernelArgs *)arg;
    set_log_level((int)k_args->log_level);
    set_log_info_v((int)k_args->log_info_v);
    g_device_start_cycle = esl_onboard_sys_cnt();
    if (k_args->device_wall_data_base != 0) {
        *(uint64_t *)(uintptr_t)k_args->device_wall_data_base = 0;
    }
    LOG_INFO_V0("%s", "esl_proxy AICPU Init");
    return 0;
}

__attribute__((visibility("default"))) int simpler_aicpu_exec(void *arg)
{
    KernelArgs *k_args;
    EslRuntime *runtime;
    int rc;
    uint64_t my_end;

    if (arg == NULL) {
        LOG_ERROR("%s", "Invalid kernel arguments: null pointer");
        return -1;
    }
    k_args = (KernelArgs *)arg;
    runtime = (EslRuntime *)(uintptr_t)k_args->runtime_args;
    if (runtime == NULL) {
        LOG_ERROR("%s", "Invalid runtime_args: null pointer");
        return -1;
    }
    set_log_level((int)k_args->log_level);
    set_log_info_v((int)k_args->log_info_v);
    set_platform_regs(k_args->regs);
    g_esl_stats_base = (volatile uint64_t *)(uintptr_t)k_args->device_wall_data_base;
    rc = esl_aicpu_execute(runtime);
    if (rc != 0) {
        LOG_ERROR("esl_aicpu_execute failed rc=%d", rc);
        return rc;
    }
    my_end = esl_onboard_sys_cnt();
    if (g_esl_stats_base != NULL && my_end > g_device_start_cycle) {
        g_esl_stats_base[3] = (uint64_t)(cycles_to_us(my_end - g_device_start_cycle) * 1000.0);
        cache_flush_range((const void *)g_esl_stats_base, 4 * sizeof(uint64_t));
    }
    return rc;
}

/* ========================================================================== */
/* AICore bridge (dispatch.c)                                                 */
/* ========================================================================== */

static uint64_t core_reg_addr(int core)
{
    uint64_t reg_addr = esl_handshake_reg_addr(core);

    if (reg_addr != 0) {
        return reg_addr;
    }
    const uint64_t table = get_platform_regs();

    if (table == 0) {
        return 0;
    }
    return ((uint64_t *)table)[core];
}

int aicore_bridge_init(AicoreBridge *bridge, EslRuntime *runtime, uint64_t fake_kernel_addr)
{
    if (bridge == NULL || runtime == NULL) {
        return -1;
    }
    bridge->runtime = runtime;
    bridge->fake_kernel_addr = fake_kernel_addr;
    bridge->initialized = 1;
    return 0;
}

void aicore_bridge_shutdown(AicoreBridge *bridge)
{
    if (bridge != NULL && bridge->initialized) {
        if (bridge->runtime != NULL) {
            esl_shutdown_all_cores(bridge->runtime);
        }
        bridge->initialized = 0;
    }
}

int aicore_bridge_poll_completions(AicoreBridge *bridge, int dispatch_tid)
{
    (void)dispatch_tid;
    if (bridge == NULL || !bridge->initialized) {
        return 0;
    }
    const int n = bridge->runtime->worker_count;
    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            for (int core = 0; core < n && core < AIC_CNT; core++) {
                uint16_t task_id = g_executors[exe_type][core].tasks[slot];

                if (task_id == EXEC_SLOT_EMPTY) {
                    continue;
                }
                if (g_ctrl_t[0].msg_bitmap[exe_type][slot] & ((uint64_t)1 << core)) {
                    continue;
                }
                const int phys = esl_phys_worker(core, exe_type);
                if (phys >= n) {
                    continue;
                }
                if (esl_hw_poll_fin(core_reg_addr(phys), task_id)) {
                    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= ((uint64_t)1 << core);
                    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
                    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
                }
            }
        }
    }
    return 0;
}

int aicore_bridge_dispatch_task(AicoreBridge *bridge, int dispatch_tid, uint16_t task_id, int core, int slot,
                                int exe_type)
{
    (void)dispatch_tid;
    g_executors[exe_type][core].tasks[slot] = task_id;
    g_executors[exe_type][core].duration[slot] = g_basic_buf[task_id & RING_MASK].duration;
    g_executors[exe_type][core].idx = (uint8_t)slot;
    const int phys = esl_phys_worker(core, exe_type);
    if (bridge != NULL && bridge->runtime != NULL && phys >= bridge->runtime->worker_count) {
        g_ctrl_t[0].msg_bitmap[exe_type][slot] |= ((uint64_t)1 << core);
        return 0;
    }
    const uint32_t raw = g_basic_buf[task_id & RING_MASK].duration;
    esl_dispatch_payload_prepare(phys, task_id, raw);
    const uint64_t reg_addr = core_reg_addr(phys);
    if (reg_addr == 0) {
        return -1;
    }
    esl_hw_dispatch_reg(reg_addr, task_id);
    return 0;
}

/* ========================================================================== */
/* Shared-memory cache sync (aicpu_bridge.h)                                  */
/* ========================================================================== */

void esl_onboard_invalidate_runtime(void *runtime)
{
    if (runtime != NULL) {
        cache_invalidate_range(runtime, sizeof(EslRuntime));
    }
}

void esl_onboard_flush_shared_after_orch(void)
{
    cache_flush_range(&g_task_id, sizeof(g_task_id));
    cache_flush_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_flush_range(g_basic_buf, sizeof(g_basic_buf[0]) * 8);
    cache_flush_range(g_predecessors, sizeof(g_predecessors[0]) * 8);
    cache_flush_range(g_successor_buf, sizeof(g_successor_buf[0]) * 8);
}

void esl_onboard_invalidate_shared_before_worker(void)
{
    cache_invalidate_range(&g_task_id, sizeof(g_task_id));
    cache_invalidate_range(&g_orch_is_done, sizeof(g_orch_is_done));
    cache_invalidate_range(&g_completed_cnt, sizeof(g_completed_cnt));
    cache_invalidate_range(&g_is_done, sizeof(g_is_done));
    cache_invalidate_range(g_basic_buf, sizeof(g_basic_buf[0]) * 8);
    cache_invalidate_range(g_predecessors, sizeof(g_predecessors[0]) * 8);
    cache_invalidate_range(g_successor_buf, sizeof(g_successor_buf[0]) * 8);
    cache_invalidate_range(g_ctrl_t, sizeof(g_ctrl_t));
}

void esl_onboard_flush_after_cutter(void)
{
    cache_flush_range(g_ctrl_t, sizeof(g_ctrl_t));
    cache_flush_range(&g_completed_cnt, sizeof(g_completed_cnt));
}

void esl_onboard_flush_after_dispatch(void)
{
    cache_flush_range(g_ctrl_t, sizeof(g_ctrl_t));
    cache_flush_range(&g_completed_cnt, sizeof(g_completed_cnt));
}

/* ========================================================================== */
/* Platform init / shutdown                                                   */
/* ========================================================================== */

int esl_platform_init(EslRuntime *runtime, AicoreBridge *bridge)
{
    ring_buf_init();
    init_state_buf();
    init_predecessors();
    init_ctrl_t();
    mem_pool_init(&g_mem_pool, ONBOARD_POOL_BASE, ONBOARD_POOL_SIZE);
    mem_pool_init_fifo(&g_mem_pool, g_onboard_when2free, ONBOARD_WHEN2FREE_CAP);
    for (int t = 0; t < EXE_TYPE_CNT; t++) {
        for (int c = 0; c < AIC_CNT; c++) {
            for (int s = 0; s < AIC_OSTD; s++) {
                g_executors[t][c].tasks[s] = EXEC_SLOT_EMPTY;
            }
        }
    }
    esl_dispatch_payload_init(runtime);
    if (runtime != NULL) {
        runtime->worker_count = ESL_PROXY_FAKE_AICORE_COUNT;
    }
    uint64_t fake_addr = 0;
    if (runtime != NULL) {
        fake_addr = runtime->func_id_to_addr_[0];
    }
    if (aicore_bridge_init(bridge, runtime, fake_addr) != 0) {
        return -1;
    }
    dispatch_set_aicore_bridge(bridge);
    return 0;
}

void esl_platform_shutdown(AicoreBridge *bridge)
{
    aicore_bridge_shutdown(bridge);
}

/* ========================================================================== */
/* Orchestration case (ORCH_CASE env / build flag)                            */
/* ========================================================================== */

#include INCLUDE_FILE(ORCH_CASE)
