/* Host-side L2 swimlane collector wrapper (C ABI for host_onboard.c). */
#include "esl_swimlane_host.h"

#include "host/l2_swimlane_collector.h"

#include "common/core_type.h"

#include <acl/acl.h>
#include <ascend_hal.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

void *g_hal_handle = nullptr;

using HalHostRegisterFn = int (*)(void *dev_ptr, size_t size, unsigned int flags, int device_id, void **host_ptr);
using HalHostUnregisterFn = int (*)(void *host_ptr, int device_id);

int load_hal_if_needed(void)
{
    if (g_hal_handle != nullptr) {
        return 0;
    }
    g_hal_handle = dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
    return g_hal_handle != nullptr ? 0 : -1;
}

HalHostRegisterFn get_hal_host_register(void)
{
    if (g_hal_handle == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<HalHostRegisterFn>(dlsym(g_hal_handle, "halHostRegister"));
}

HalHostUnregisterFn get_hal_host_unregister(void)
{
    if (g_hal_handle == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<HalHostUnregisterFn>(dlsym(g_hal_handle, "halHostUnregister"));
}

L2SwimlaneCollector g_collector;
L2SwimlaneLevel g_level = L2SwimlaneLevel::AICPU_TIMING;
int g_device_id = 0;
bool g_initialized = false;
std::string g_output_prefix = ".";

void *acl_alloc(size_t size)
{
    void *ptr = nullptr;
    if (aclrtMalloc(&ptr, size, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS) {
        return nullptr;
    }
    return ptr;
}

int acl_free(void *ptr)
{
    return ptr != nullptr ? (int)aclrtFree(ptr) : 0;
}

int hal_register(void *dev_ptr, size_t size, int device_id, void **host_ptr)
{
    if (load_hal_if_needed() != 0) {
        return -1;
    }
    HalHostRegisterFn fn = get_hal_host_register();
    if (fn == nullptr) {
        return -1;
    }
    return fn(dev_ptr, size, DEV_SVM_MAP_HOST, device_id, host_ptr);
}

int hal_unregister(void *host_ptr, int device_id)
{
    if (load_hal_if_needed() != 0) {
        return -1;
    }
    HalHostUnregisterFn fn = get_hal_host_unregister();
    if (fn == nullptr) {
        return -1;
    }
    return fn(host_ptr, device_id);
}

static void export_perfetto_trace_json(void)
{
    const char *esl_root = getenv("ESL_PROXY_ROOT");
    if (esl_root == nullptr || esl_root[0] == '\0') {
        esl_root = ".";
    }
    const std::string raw = g_output_prefix + "/l2_swimlane_records.json";
    const std::string out = g_output_prefix + "/l2_swimlane_trace.json";
    const std::string script = std::string(esl_root) + "/tools/swimlane_to_perfetto.py";
    const std::string cmd =
        "python3 '" + script + "' '" + raw + "' -o '" + out + "' >/dev/null 2>&1";

    if (std::system(cmd.c_str()) == 0) {
        fprintf(stderr, "[esl_proxy] Perfetto trace: %s\n", out.c_str());
        fprintf(stderr, "[esl_proxy] Import at https://ui.perfetto.dev/\n");
    } else {
        fprintf(stderr,
            "[esl_proxy] Perfetto conversion failed; raw swimlane data at %s "
            "(run: python3 %s %s)\n",
            raw.c_str(), script.c_str(), raw.c_str());
    }
}

}  // namespace

extern "C" void esl_swimlane_host_set_level(int level)
{
    if (level < 0) {
        g_level = L2SwimlaneLevel::DISABLED;
        return;
    }
    if (level > static_cast<int>(L2SwimlaneLevel::ORCH_PHASES)) {
        level = static_cast<int>(L2SwimlaneLevel::ORCH_PHASES);
    }
    g_level = static_cast<L2SwimlaneLevel>(level);
}

extern "C" int esl_swimlane_host_init(int worker_count, int aicpu_thread_num, int device_id, const char *output_prefix)
{
    if (g_level == L2SwimlaneLevel::DISABLED || worker_count <= 0) {
        return 0;
    }
    g_device_id = device_id;
    if (output_prefix != nullptr && output_prefix[0] != '\0') {
        g_output_prefix = output_prefix;
    } else {
        g_output_prefix = ".";
    }

    auto alloc_cb = [](size_t size) -> void * { return acl_alloc(size); };
    auto free_cb = [](void *ptr) -> int { return acl_free(ptr); };

    int rc = g_collector.initialize(
        worker_count, aicpu_thread_num, device_id, g_level, alloc_cb, hal_register, free_cb, g_output_prefix);
    if (rc != 0) {
        fprintf(stderr, "[esl_proxy] swimlane init failed rc=%d\n", rc);
        return rc;
    }
    g_initialized = true;
    return 0;
}

extern "C" void esl_swimlane_host_start(void)
{
    if (!g_initialized) {
        return;
    }
    // CANN device context is per-thread; collector mgmt/poll threads call
    // aclrtMalloc when recycling buffers — mirror simpler DeviceRunner::create_thread.
    const int dev_id = g_device_id;
    g_collector.start([dev_id](std::function<void()> fn) -> std::thread {
        return std::thread([dev_id, fn = std::move(fn)]() {
            if (aclrtSetDevice(dev_id) != ACL_SUCCESS) {
                fprintf(stderr, "[esl_proxy] swimlane thread aclrtSetDevice(%d) failed\n", dev_id);
                return;
            }
            fn();
        });
    });
}

extern "C" void esl_swimlane_host_stop_and_export(void)
{
    if (!g_initialized) {
        return;
    }
    g_collector.stop();
    g_collector.read_phase_header_metadata();
    g_collector.reconcile_counters();
    if (g_collector.export_swimlane_json() == 0) {
        export_perfetto_trace_json();
    }
}

extern "C" void esl_swimlane_host_finalize(void)
{
    if (!g_initialized) {
        return;
    }
    g_collector.finalize(hal_unregister, [](void *ptr) -> int { return acl_free(ptr); });
    g_initialized = false;
}

extern "C" uint64_t esl_swimlane_host_data_base(void)
{
    if (!g_initialized) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(g_collector.get_l2_swimlane_setup_device_ptr());
}

extern "C" uint64_t esl_swimlane_host_rotation_table(void)
{
    if (!g_initialized) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(g_collector.get_aicore_ring_addr_table_device_ptr());
}

extern "C" void esl_swimlane_host_set_core_types(const int32_t *core_types, int count)
{
    if (!g_initialized || core_types == nullptr || count <= 0) {
        return;
    }
    std::vector<CoreType> types(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        types[static_cast<size_t>(i)] = static_cast<CoreType>(core_types[i]);
    }
    g_collector.set_core_types(types.data(), count);
}
