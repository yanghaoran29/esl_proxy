/*
 * fake_aicore_host.c — Host pthread fake AICore (72 workers by default).
 */
#define _POSIX_C_SOURCE 199309L

#include "fake_aicore_host.h"

#include "conf.h"
#include "fake_kernel.h"
#include "log.h"
#include "spin.h"
#include "worker_map.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define HOST_FAKE_JOB_QUEUE_DEPTH 64
#define HOST_FAKE_FIN_QUEUE_CAP 16384

typedef struct {
    uint16_t task_id;
    uint8_t exe_type;
    uint8_t core;
    uint8_t slot;
    uint64_t mask;
    uint32_t duration_ns;
    uint32_t jitter_mask;
} HostFakeJob;

typedef struct {
    HostFakeJob jobs[HOST_FAKE_JOB_QUEUE_DEPTH];
    atomic_uint head;
    atomic_uint tail;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} HostFakeWorkerQueue;

static HostFakeFin g_fin_queue[HOST_FAKE_FIN_QUEUE_CAP];
static atomic_uint g_fin_head;
static atomic_uint g_fin_tail;
static pthread_mutex_t g_fin_push_mu = PTHREAD_MUTEX_INITIALIZER;

static HostFakeWorkerQueue g_fake_queues[ESL_PROXY_HOST_WORKER_COUNT];
static pthread_t g_fake_threads[ESL_PROXY_HOST_WORKER_COUNT];
static atomic_bool g_fake_shutdown;
static int g_fake_worker_count;

static int host_fake_fin_push(const HostFakeFin *fin)
{
    uint32_t tail;
    uint32_t next;

    pthread_mutex_lock(&g_fin_push_mu);
    tail = atomic_load_explicit(&g_fin_tail, memory_order_relaxed);
    next = (tail + 1U) % (uint32_t)HOST_FAKE_FIN_QUEUE_CAP;
    if (next == atomic_load_explicit(&g_fin_head, memory_order_acquire)) {
        pthread_mutex_unlock(&g_fin_push_mu);
        return -1;
    }
    g_fin_queue[tail] = *fin;
    atomic_store_explicit(&g_fin_tail, next, memory_order_release);
    pthread_mutex_unlock(&g_fin_push_mu);
    return 0;
}

int host_fake_fin_pop(HostFakeFin *out)
{
    uint32_t head;
    uint32_t tail;
    uint32_t next;

    head = atomic_load_explicit(&g_fin_head, memory_order_relaxed);
    tail = atomic_load_explicit(&g_fin_tail, memory_order_acquire);
    if (head == tail) {
        return -1;
    }
    *out = g_fin_queue[head];
    next = (head + 1U) % (uint32_t)HOST_FAKE_FIN_QUEUE_CAP;
    atomic_store_explicit(&g_fin_head, next, memory_order_release);
    return 0;
}

static void host_fake_signal_completion(const HostFakeJob *job)
{
    HostFakeFin fin;

    fin.task_id = job->task_id;
    fin.exe_type = job->exe_type;
    fin.core = job->core;
    fin.slot = job->slot;
    fin.mask = job->mask;
    if (host_fake_fin_push(&fin) != 0) {
        /* Should not happen with FIN queue >> in-flight subtasks. */
        for (;;) {
            spin_wait();
        }
    }
}

static int host_fake_queue_push(HostFakeWorkerQueue *q, const HostFakeJob *job)
{
    uint32_t tail;
    uint32_t next;

    pthread_mutex_lock(&q->mu);
    tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    next = (tail + 1U) % HOST_FAKE_JOB_QUEUE_DEPTH;
    if (next == atomic_load_explicit(&q->head, memory_order_acquire)) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    q->jobs[tail] = *job;
    atomic_store_explicit(&q->tail, next, memory_order_release);
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

static int host_fake_queue_pop(HostFakeWorkerQueue *q, HostFakeJob *out)
{
    uint32_t head;
    uint32_t next;

    pthread_mutex_lock(&q->mu);
    while (atomic_load_explicit(&q->head, memory_order_acquire) ==
           atomic_load_explicit(&q->tail, memory_order_acquire)) {
        if (atomic_load_explicit(&g_fake_shutdown, memory_order_acquire)) {
            pthread_mutex_unlock(&q->mu);
            return -1;
        }
        pthread_cond_wait(&q->cv, &q->mu);
    }
    head = atomic_load_explicit(&q->head, memory_order_relaxed);
    *out = q->jobs[head];
    next = (head + 1U) % HOST_FAKE_JOB_QUEUE_DEPTH;
    atomic_store_explicit(&q->head, next, memory_order_release);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

static void *host_fake_aicore_worker(void *arg)
{
    const int worker_id = (int)(intptr_t)arg;
    HostFakeWorkerQueue *q = &g_fake_queues[worker_id];
    HostFakeJob job;

    while (!atomic_load_explicit(&g_fake_shutdown, memory_order_acquire)) {
        if (host_fake_queue_pop(q, &job) != 0) {
            break;
        }
        esl_fake_kernel_busy_wait_ns(job.duration_ns, (uint64_t)job.jitter_mask, get_time_ns);
        host_fake_signal_completion(&job);
    }
    return NULL;
}

void host_fake_aicore_start(int worker_count)
{
    int i;

    if (worker_count <= 0 || worker_count > ESL_PROXY_HOST_WORKER_COUNT) {
        worker_count = ESL_PROXY_HOST_WORKER_COUNT;
    }
    g_fake_worker_count = worker_count;
    atomic_store_explicit(&g_fake_shutdown, false, memory_order_release);
    atomic_store_explicit(&g_fin_head, 0U, memory_order_relaxed);
    atomic_store_explicit(&g_fin_tail, 0U, memory_order_relaxed);
    for (i = 0; i < g_fake_worker_count; ++i) {
        HostFakeWorkerQueue *q = &g_fake_queues[i];
        atomic_store_explicit(&q->head, 0U, memory_order_relaxed);
        atomic_store_explicit(&q->tail, 0U, memory_order_relaxed);
        pthread_mutex_init(&q->mu, NULL);
        pthread_cond_init(&q->cv, NULL);
        if (pthread_create(&g_fake_threads[i], NULL, host_fake_aicore_worker, (void *)(intptr_t)i) != 0) {
            host_fake_aicore_stop();
            return;
        }
    }
}

void host_fake_aicore_stop(void)
{
    int i;

    atomic_store_explicit(&g_fake_shutdown, true, memory_order_release);
    for (i = 0; i < g_fake_worker_count; ++i) {
        pthread_cond_broadcast(&g_fake_queues[i].cv);
        (void)pthread_join(g_fake_threads[i], NULL);
        pthread_mutex_destroy(&g_fake_queues[i].mu);
        pthread_cond_destroy(&g_fake_queues[i].cv);
    }
    g_fake_worker_count = 0;
}

int host_fake_aicore_submit(int phys_worker, uint16_t task_id, int exe_type, int core, int slot,
                            uint64_t mask, uint32_t duration_ns, uint32_t jitter_mask)
{
    HostFakeJob job;

    if (phys_worker < 0 || phys_worker >= g_fake_worker_count) {
        return -1;
    }
    job.task_id = task_id;
    job.exe_type = (uint8_t)exe_type;
    job.core = (uint8_t)core;
    job.slot = (uint8_t)slot;
    job.mask = mask;
    job.duration_ns = duration_ns;
    job.jitter_mask = jitter_mask;
    return host_fake_queue_push(&g_fake_queues[phys_worker], &job);
}
