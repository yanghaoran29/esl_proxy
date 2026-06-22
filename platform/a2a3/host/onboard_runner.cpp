/*
 * esl_proxy standalone onboard runner — launches libaicpu_kernel.so via CANN only.
 * No simpler Python / libhost_runtime dependency at runtime.
 */
#include "aicpu_loader.h"
#include "esl_kernel_args.h"
#include "esl_runtime.h"
#include "onboard_config.h"

#include <acl/acl.h>
#include <runtime/rt.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef ESL_PROXY_CANN_AICPU_LAUNCH_THREADS
#define ESL_PROXY_CANN_AICPU_LAUNCH_THREADS 6
#endif

namespace {

#define ACL_CHECK(call, msg)                                   \
    do {                                                       \
        aclError _rc = (call);                                 \
        if (_rc != ACL_SUCCESS) {                              \
            std::fprintf(stderr, "%s failed: %d (%s)\n", #call, (int)_rc, msg); \
            return 1;                                          \
        }                                                      \
    } while (0)

#define RT_CHECK(call, msg)                                    \
    do {                                                       \
        rtError_t _rc = (call);                                \
        if (_rc != RT_ERROR_NONE) {                            \
            std::fprintf(stderr, "%s failed: %d (%s)\n", #call, (int)_rc, msg);  \
            return 1;                                          \
        }                                                      \
    } while (0)

bool read_file(const char *path, std::vector<char> *out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::fprintf(stderr, "open %s: %s\n", path, std::strerror(errno));
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz <= 0) {
        std::fprintf(stderr, "empty file: %s\n", path);
        return false;
    }
    out->resize(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(out->data(), static_cast<std::streamsize>(out->size()));
    return f.gcount() == static_cast<std::streamsize>(out->size());
}

struct DevMem {
    void *ptr = nullptr;
    ~DevMem()
    {
        if (ptr != nullptr) {
            aclrtFree(ptr);
        }
    }
    aclError alloc(size_t n)
    {
        return aclrtMalloc(&ptr, n, ACL_MEM_MALLOC_HUGE_FIRST);
    }
};

int resolve_device_id(int cli_dev)
{
    const char *env = std::getenv("TASK_DEVICE");
    if (env != nullptr && env[0] != '\0') {
        return std::atoi(env);
    }
    return cli_dev;
}

void usage(const char *prog)
{
    std::fprintf(stderr,
                 "usage: %s [-d device_id] [--dispatcher PATH] [--aicpu PATH]\n"
                 "  defaults:\n"
                 "    dispatcher = build/onboard/aicpu/libsimpler_aicpu_dispatcher.so\n"
                 "    aicpu      = build/onboard/aicpu/libaicpu_kernel.so\n",
                 prog);
}

}  // namespace

int main(int argc, char **argv)
{
    int device_id = 0;
    std::string dispatcher_path;
    std::string aicpu_path;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device_id = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--dispatcher") == 0 && i + 1 < argc) {
            dispatcher_path = argv[++i];
        } else if (std::strcmp(argv[i], "--aicpu") == 0 && i + 1 < argc) {
            aicpu_path = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    device_id = resolve_device_id(device_id);

    if (dispatcher_path.empty()) {
        const char *env = std::getenv("ESL_PROXY_DISPATCHER_SO");
        dispatcher_path = env ? env : "build/onboard/aicpu/libsimpler_aicpu_dispatcher.so";
    }
    if (aicpu_path.empty()) {
        const char *env = std::getenv("ESL_PROXY_AICPU_SO");
        aicpu_path = env ? env : "build/onboard/aicpu/libaicpu_kernel.so";
    }

    std::vector<char> dispatcher_bytes;
    std::vector<char> aicpu_bytes;
    if (!read_file(dispatcher_path.c_str(), &dispatcher_bytes)) {
        return 1;
    }
    if (!read_file(aicpu_path.c_str(), &aicpu_bytes)) {
        return 1;
    }

    std::fprintf(stderr, "[esl_proxy] onboard runner device=%d fake_cores=%d aicpu_threads=%d launch_threads=%d\n",
                 device_id, ESL_PROXY_FAKE_AICORE_COUNT, ESL_PROXY_AICPU_THREAD_NUM,
                 ESL_PROXY_CANN_AICPU_LAUNCH_THREADS);
    std::fprintf(stderr, "[esl_proxy] dispatcher=%s aicpu=%s\n", dispatcher_path.c_str(), aicpu_path.c_str());

    ACL_CHECK(aclInit(nullptr), "aclInit");
    ACL_CHECK(aclrtSetDevice(device_id), "aclrtSetDevice");

    rtStream_t stream = nullptr;
    ACL_CHECK(aclrtCreateStream(&stream), "aclrtCreateStream");

    EslAicpuLoader *loader = esl_aicpu_loader_create();
    int rc = esl_aicpu_loader_bootstrap(
        loader, dispatcher_bytes.data(), dispatcher_bytes.size(), aicpu_bytes.data(), aicpu_bytes.size(), stream,
        device_id);
    if (rc != 0) {
        std::fprintf(stderr, "bootstrap failed: %d\n", rc);
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream);
        aclrtResetDevice(device_id);
        aclFinalize();
        return 1;
    }
    rc = esl_aicpu_loader_init(loader);
    if (rc != 0) {
        std::fprintf(stderr, "loader init failed: %d\n", rc);
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream);
        aclrtResetDevice(device_id);
        aclFinalize();
        return 1;
    }

    EslRuntime host_runtime = {};
    host_runtime.worker_count = ESL_PROXY_FAKE_AICORE_COUNT;
    host_runtime.aicpu_thread_num = ESL_PROXY_AICPU_THREAD_NUM;

    DevMem dev_runtime;
    DevMem dev_wall;
    ACL_CHECK(dev_runtime.alloc(sizeof(EslRuntime)), "runtime GM");
    ACL_CHECK(dev_wall.alloc(sizeof(uint64_t)), "device wall GM");
    ACL_CHECK(
        aclrtMemcpy(dev_runtime.ptr, sizeof(EslRuntime), &host_runtime, sizeof(EslRuntime), ACL_MEMCPY_HOST_TO_DEVICE),
        "H2D runtime");
    ACL_CHECK(aclrtMemset(dev_wall.ptr, sizeof(uint64_t), 0, sizeof(uint64_t)), "zero wall");

    EslKernelArgs k_args = {};
    k_args.runtime_args = reinterpret_cast<EslRuntime *>(dev_runtime.ptr);
    k_args.regs = 0;
    k_args.log_level = 1;
    k_args.log_info_v = 5;
    k_args.device_wall_data_base = reinterpret_cast<uint64_t>(dev_wall.ptr);
    k_args.device_id = static_cast<uint32_t>(device_id);

    rc = esl_aicpu_loader_launch(loader, stream, &k_args, 1, ESL_AICPU_INIT_NAME);
    if (rc != 0) {
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream);
        aclrtResetDevice(device_id);
        aclFinalize();
        return 1;
    }
    ACL_CHECK(aclrtSynchronizeStream(stream), "sync after init");

    rc = esl_aicpu_loader_launch(loader, stream, &k_args, ESL_PROXY_CANN_AICPU_LAUNCH_THREADS, ESL_AICPU_EXEC_NAME);
    if (rc != 0) {
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream);
        aclrtResetDevice(device_id);
        aclFinalize();
        return 1;
    }
    ACL_CHECK(aclrtSynchronizeStream(stream), "sync after exec");

    uint64_t wall_ns = 0;
    ACL_CHECK(
        aclrtMemcpy(&wall_ns, sizeof(wall_ns), dev_wall.ptr, sizeof(wall_ns), ACL_MEMCPY_DEVICE_TO_HOST), "D2H wall");

    std::printf("esl_proxy onboard smoke: OK device_wall_ns=%llu\n", (unsigned long long)wall_ns);

    esl_aicpu_loader_destroy(loader);
    aclrtDestroyStream(stream);
    aclrtResetDevice(device_id);
    aclFinalize();
    return 0;
}
