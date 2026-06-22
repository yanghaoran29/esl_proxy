#include "aicpu_loader.h"

#include "elf_fingerprint.h"

#include <acl/acl.h>
#include <runtime/rt.h>
#include <runtime/runtime/rts/rts_kernel.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

struct EslAicpuLoader {
    void *binary_handle = nullptr;
    rtFuncHandle init_handle = nullptr;
    rtFuncHandle exec_handle = nullptr;
    std::string json_path;
    uint64_t inner_fp = 0;
    int device_id = 0;
    std::string inner_basename;
};

namespace {

std::set<std::pair<uint64_t, int>> &bootstrapped()
{
    static std::set<std::pair<uint64_t, int>> k;
    return k;
}

std::mutex &bootstrap_mu()
{
    static std::mutex m;
    return m;
}

struct DevBuf {
    void *ptr = nullptr;
    ~DevBuf()
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

static std::string make_inner_basename(uint64_t fp, int device_id)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "simpler_inner_%016lx_%d.so", fp, device_id);
    return buf;
}

static std::string make_op_type(const char *base, uint64_t fp)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s_%016lx", base, fp);
    return buf;
}

static bool write_json(const std::string &path, const std::string &kernel_so, uint64_t fp)
{
    std::ofstream out(path);
    if (!out.is_open()) {
        return false;
    }
    auto entry = [&](const char *sym) {
        std::string op = make_op_type(sym, fp);
        out << "  \"" << op << "\": {\n";
        out << "    \"opInfo\": {\n";
        out << "      \"functionName\": \"" << sym << "\",\n";
        out << "      \"kernelSo\": \"" << kernel_so << "\",\n";
        out << "      \"opKernelLib\": \"AICPUKernel\",\n";
        out << "      \"computeCost\": \"100\",\n";
        out << "      \"engine\": \"DNN_VM_AICPU\",\n";
        out << "      \"flagAsync\": \"False\",\n";
        out << "      \"flagPartial\": \"False\",\n";
        out << "      \"userDefined\": \"False\"\n";
        out << "    }\n  }";
    };
    out << "{\n";
    entry(ESL_AICPU_INIT_NAME);
    out << ",\n";
    entry(ESL_AICPU_EXEC_NAME);
    out << "\n}\n";
    return true;
}

}  // namespace

extern "C" {

EslAicpuLoader *esl_aicpu_loader_create(void)
{
    return new EslAicpuLoader();
}

void esl_aicpu_loader_destroy(EslAicpuLoader *loader)
{
    if (loader == nullptr) {
        return;
    }
    if (loader->binary_handle != nullptr) {
        rtsBinaryUnload(loader->binary_handle);
    }
    if (!loader->json_path.empty()) {
        std::remove(loader->json_path.c_str());
    }
    delete loader;
}

int esl_aicpu_loader_bootstrap(
    EslAicpuLoader *loader, const void *dispatcher_so, size_t dispatcher_len, const void *inner_so, size_t inner_len,
    rtStream_t stream, int device_id)
{
    if (loader == nullptr || dispatcher_so == nullptr || dispatcher_len == 0 || inner_so == nullptr || inner_len == 0) {
        return -1;
    }

    loader->device_id = device_id;
    loader->inner_fp = esl_fingerprint_bytes(inner_so, inner_len);
    loader->inner_basename = make_inner_basename(loader->inner_fp, device_id);

    {
        std::lock_guard<std::mutex> lk(bootstrap_mu());
        if (bootstrapped().count({loader->inner_fp, device_id}) > 0) {
            std::fprintf(stderr, "[esl_proxy] AICPU inner SO already bootstrapped fp=%016lx dev=%d\n", loader->inner_fp,
                         device_id);
            return 0;
        }
    }

    DevBuf dev_dispatcher;
    DevBuf dev_inner;
    aclError rc = dev_dispatcher.alloc(dispatcher_len);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }
    rc = aclrtMemcpy(dev_dispatcher.ptr, dispatcher_len, dispatcher_so, dispatcher_len, ACL_MEMCPY_HOST_TO_DEVICE);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }
    rc = dev_inner.alloc(inner_len);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }
    rc = aclrtMemcpy(dev_inner.ptr, inner_len, inner_so, inner_len, ACL_MEMCPY_HOST_TO_DEVICE);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }

    constexpr size_t kDeviceArgsBytes = 160;
    char host_dev_args[kDeviceArgsBytes] = {};
    auto w64 = [&](size_t off, uint64_t v) {
        std::memcpy(host_dev_args + off, &v, sizeof(v));
    };
    w64(96, reinterpret_cast<uint64_t>(dev_dispatcher.ptr));
    w64(104, static_cast<uint64_t>(dispatcher_len));
    w64(112, static_cast<uint64_t>(device_id));
    w64(120, reinterpret_cast<uint64_t>(dev_inner.ptr));
    w64(128, static_cast<uint64_t>(inner_len));

    DevBuf dev_args;
    rc = dev_args.alloc(kDeviceArgsBytes);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }
    rc = aclrtMemcpy(dev_args.ptr, kDeviceArgsBytes, host_dev_args, kDeviceArgsBytes, ACL_MEMCPY_HOST_TO_DEVICE);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }

    struct Args {
        struct {
            uint64_t unused[5];
            uint64_t device_args_ptr;
            uint64_t pad[20];
        } k_args;
        char kernel_name[32];
        char so_name[32];
        char op_name[32];
    } args = {};
    args.k_args.device_args_ptr = reinterpret_cast<uint64_t>(dev_args.ptr);
    std::strncpy(args.kernel_name, "DynTileFwkKernelServerInit", sizeof(args.kernel_name) - 1);
    std::strncpy(args.so_name, "libaicpu_extend_kernels.so", sizeof(args.so_name) - 1);

    rtAicpuArgsEx_t rt_args = {};
    rt_args.args = &args;
    rt_args.argsSize = sizeof(args);
    rt_args.kernelNameAddrOffset = offsetof(Args, kernel_name);
    rt_args.soNameAddrOffset = offsetof(Args, so_name);

    rtError_t rrc = rtAicpuKernelLaunchExWithArgs(
        rtKernelType_t::KERNEL_TYPE_AICPU_KFC, "AST_DYN_AICPU", 1, &rt_args, nullptr, stream, 0);
    if (rrc != RT_ERROR_NONE) {
        std::fprintf(stderr, "rtAicpuKernelLaunchExWithArgs bootstrap failed: %d\n", (int)rrc);
        return (int)rrc;
    }
    rc = aclrtSynchronizeStream(stream);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }

    {
        std::lock_guard<std::mutex> lk(bootstrap_mu());
        bootstrapped().insert({loader->inner_fp, device_id});
    }
    std::fprintf(stderr, "[esl_proxy] bootstrapped inner SO %s\n", loader->inner_basename.c_str());
    return 0;
}

int esl_aicpu_loader_init(EslAicpuLoader *loader)
{
    if (loader == nullptr || loader->inner_fp == 0) {
        return -1;
    }

    char json_name[128];
    std::snprintf(json_name, sizeof(json_name), "/tmp/esl_proxy_inner_%016lx_%d.json", loader->inner_fp, getpid());
    loader->json_path = json_name;
    if (!write_json(loader->json_path, loader->inner_basename, loader->inner_fp)) {
        return -1;
    }

    rtLoadBinaryOption_t option = {};
    option.optionId = RT_LOAD_BINARY_OPT_CPU_KERNEL_MODE;
    option.value.cpuKernelMode = 0;
    rtLoadBinaryConfig_t load_config = {};
    load_config.options = &option;
    load_config.numOpt = 1;

    rtError_t rc = rtsBinaryLoadFromFile(loader->json_path.c_str(), &load_config, &loader->binary_handle);
    if (rc != RT_ERROR_NONE) {
        std::fprintf(stderr, "rtsBinaryLoadFromFile failed: %d\n", (int)rc);
        return (int)rc;
    }

    std::string init_op = make_op_type(ESL_AICPU_INIT_NAME, loader->inner_fp);
    std::string exec_op = make_op_type(ESL_AICPU_EXEC_NAME, loader->inner_fp);
    rc = rtsFuncGetByName(loader->binary_handle, init_op.c_str(), &loader->init_handle);
    if (rc != RT_ERROR_NONE) {
        return (int)rc;
    }
    rc = rtsFuncGetByName(loader->binary_handle, exec_op.c_str(), &loader->exec_handle);
    if (rc != RT_ERROR_NONE) {
        return (int)rc;
    }
    return 0;
}

int esl_aicpu_loader_launch(
    EslAicpuLoader *loader, rtStream_t stream, EslKernelArgs *k_args, int aicpu_num, const char *symbol_name)
{
    if (loader == nullptr || k_args == nullptr || symbol_name == nullptr) {
        return -1;
    }

    rtFuncHandle handle = nullptr;
    if (std::strcmp(symbol_name, ESL_AICPU_INIT_NAME) == 0) {
        handle = loader->init_handle;
    } else if (std::strcmp(symbol_name, ESL_AICPU_EXEC_NAME) == 0) {
        handle = loader->exec_handle;
    } else {
        return -1;
    }

    rtCpuKernelArgs_t cpu_args = {};
    cpu_args.baseArgs.args = k_args;
    cpu_args.baseArgs.argsSize = sizeof(EslKernelArgs);
    rtLaunchKernelAttr_t attr = {};
    rtKernelLaunchCfg_t cfg = {&attr, 0U};

    rtError_t rc = rtsLaunchCpuKernel(handle, static_cast<uint32_t>(aicpu_num), stream, &cfg, &cpu_args);
    if (rc != RT_ERROR_NONE) {
        std::fprintf(stderr, "rtsLaunchCpuKernel(%s) failed: %d\n", symbol_name, (int)rc);
        return (int)rc;
    }
    return 0;
}

}  // extern "C"
