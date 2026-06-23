/* host_runner.c — merged Host onboard runner (loader + launcher + regs + fingerprint + main) */
#define _GNU_SOURCE
#include "kernel_args.h"
#include "esl_runtime.h"
#include "onboard_config.h"
#include "tools.h"
#include <stdint.h>
#include <string.h>
#include <acl/acl_rt.h>
#include <runtime/rt.h>
#include <runtime/runtime/rts/rts_kernel.h>
#include <ascend_hal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include "dlog_pub.h"

#ifndef ESL_PROXY_KARGS_LOG_LEVEL
#define ESL_PROXY_KARGS_LOG_LEVEL 1 /* DLOG_INFO */
#endif

#define ESL_AICPU_INIT_NAME "simpler_aicpu_init"
#define ESL_AICPU_EXEC_NAME "simpler_aicpu_exec"
#define kHalMemCtlEacces 13
#define kHalMemCtlMaxRetries 3
#define ESL_BOOTSTRAP_CACHE_MAX 32
#ifndef ESL_PROXY_CANN_AICPU_LAUNCH_THREADS
#define ESL_PROXY_CANN_AICPU_LAUNCH_THREADS 6
#endif
#define ACL_CHECK(call, msg) do { aclError _rc = (call); if (_rc != ACL_SUCCESS) { fprintf(stderr, "%s failed: %d (%s)\n", #call, (int)_rc, msg); return 1; } } while (0)
#define AICORE_CORE_STRIDE (8ULL * 1024ULL * 1024ULL)
#define AICORE_SUB_CORE_STRIDE 0x100000ULL
#define AICORE_REG_VALID_FALLBACK 0xFFFFFFFFULL
#define ESL_AICPU_JSON_PATH_FMT "/tmp/esl_proxy_inner_%016lx_%d.json"
#define ESL_INNER_SO_BASENAME_FMT "simpler_inner_%016lx_%d.so"
#define ESL_INNER_OP_TYPE_FMT "%s_%016lx"


/* ===== host_regs.c ===== */

typedef int (*HalGetDeviceInfoByBuffFn)(uint64_t deviceId, int32_t moduleType, int32_t infoType, void *buf,
                                        int32_t *size);
typedef int (*HalMemCtlFn)(int type, void *paramValue, size_t paramValueSize, void *outValue, size_t *outSizeRet);

static int get_pg_mask(uint64_t *valid, uint64_t device_id)
{
    uint64_t aicore_bitmap[PLATFORM_AICORE_MAP_BUFF_LEN] = {0};
    int32_t size_n = (int32_t)(sizeof(uint64_t) * PLATFORM_AICORE_MAP_BUFF_LEN);
    HalGetDeviceInfoByBuffFn halFuncDevInfo;

    halFuncDevInfo = (HalGetDeviceInfoByBuffFn)dlsym(RTLD_DEFAULT, "halGetDeviceInfoByBuff");
    if (halFuncDevInfo == NULL) {
        return 0;
    }

    if (halFuncDevInfo(device_id, MODULE_TYPE_AICORE, INFO_TYPE_OCCUPY, aicore_bitmap, &size_n) != 0) {
        return 0;
    }
    *valid = aicore_bitmap[0];
    return 1;
}

static int get_aicore_reg_info(int64_t **aic, size_t *aic_len, size_t *aic_cap, int64_t **aiv, size_t *aiv_len,
                               size_t *aiv_cap, int addr_type, int64_t device_id)
{
    uint64_t valid = 0;
    HalMemCtlFn halFunc;
    struct AddrMapInPara in_map_para;
    struct AddrMapOutPara out_map_para;
    int ret;
    int attempt;
    uint32_t i;
    uint32_t j;

    if (!get_pg_mask(&valid, (uint64_t)device_id)) {
        valid = 0xFFFFFFFFULL;
    }

    halFunc = (HalMemCtlFn)dlsym(RTLD_DEFAULT, "halMemCtl");
    if (halFunc == NULL) {
        fprintf(stderr, "halMemCtl not found\n");
        return -1;
    }

    memset(&in_map_para, 0, sizeof(in_map_para));
    memset(&out_map_para, 0, sizeof(out_map_para));
    in_map_para.devid = device_id;
    in_map_para.addr_type = addr_type;

    ret = -1;
    for (attempt = 0; attempt <= kHalMemCtlMaxRetries; ++attempt) {
        ret = halFunc(0, &in_map_para, sizeof(in_map_para), &out_map_para, NULL);
        if (ret != kHalMemCtlEacces) {
            break;
        }
    }
    if (ret != 0) {
        fprintf(stderr, "halMemCtl failed: %d\n", ret);
        return ret;
    }

    for (i = 0; i < DAV_2201_PLATFORM_MAX_PHYSICAL_CORES; i++) {
        for (j = 0; j < PLATFORM_SUB_CORES_PER_AICORE; j++) {
            uint64_t vaddr = 0;

            if ((valid & (1ULL << i)) != 0) {
                const uint64_t core_stride = 8ULL * 1024ULL * 1024ULL;
                const uint64_t sub_core_stride = 0x100000ULL;

                vaddr = out_map_para.ptr + (i * core_stride + j * sub_core_stride);
            }
            if (j == 0) {
                if (grow_array(aic, aic_cap, aic_len, (int64_t)vaddr) == NULL) {
                    return -1;
                }
            } else {
                if (grow_array(aiv, aiv_cap, aiv_len, (int64_t)vaddr) == NULL) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

int esl_host_init_aicore_regs(uint64_t device_id, uint64_t *out_dev_regs)
{
    int64_t *aic = NULL;
    size_t aic_len = 0;
    size_t aic_cap = 0;
    int64_t *aiv = NULL;
    size_t aiv_len = 0;
    size_t aiv_cap = 0;
    int64_t *host_regs = NULL;
    size_t host_regs_len = 0;
    size_t host_regs_cap = 0;
    size_t i;
    size_t regs_size;
    void *dev_ptr;
    aclError ac;
    int rc;

    if (out_dev_regs == NULL) {
        return -1;
    }
    *out_dev_regs = 0;

    rc = get_aicore_reg_info(&aic, &aic_len, &aic_cap, &aiv, &aiv_len, &aiv_cap, ADDR_MAP_TYPE_REG_AIC_CTRL,
                             (int64_t)device_id);
    if (rc != 0) {
        free(aic);
        free(aiv);
        return rc;
    }

    for (i = 0; i < aic_len; ++i) {
        if (grow_array(&host_regs, &host_regs_cap, &host_regs_len, aic[i]) == NULL) {
            free(aic);
            free(aiv);
            free(host_regs);
            return -1;
        }
    }
    for (i = 0; i < aiv_len; ++i) {
        if (grow_array(&host_regs, &host_regs_cap, &host_regs_len, aiv[i]) == NULL) {
            free(aic);
            free(aiv);
            free(host_regs);
            return -1;
        }
    }

    free(aic);
    free(aiv);

    if (host_regs_len == 0) {
        free(host_regs);
        return -1;
    }

    regs_size = host_regs_len * sizeof(int64_t);
    dev_ptr = NULL;
    ac = aclrtMalloc(&dev_ptr, regs_size, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ac != ACL_SUCCESS) {
        free(host_regs);
        return (int)ac;
    }
    ac = aclrtMemcpy(dev_ptr, regs_size, host_regs, regs_size, ACL_MEMCPY_HOST_TO_DEVICE);
    free(host_regs);
    if (ac != ACL_SUCCESS) {
        aclrtFree(dev_ptr);
        return (int)ac;
    }

    *out_dev_regs = (uint64_t)(uintptr_t)dev_ptr;
    fprintf(stderr, "[esl_proxy] HW AICore regs: %zu addresses H2D -> 0x%lx\n", host_regs_len,
            (unsigned long)*out_dev_regs);
    return 0;
}

/* ===== aicpu_loader.c ===== */

struct EslAicpuLoader {
    void *binary_handle;
    rtFuncHandle init_handle;
    rtFuncHandle exec_handle;
    char json_path[256];
    uint64_t inner_fp;
    int device_id;
    char inner_basename[64];
};

typedef struct EslAicpuLoader EslAicpuLoader;

typedef struct {
    uint64_t fp;
    int device_id;
} BootstrapKey;

static BootstrapKey g_bootstrapped[ESL_BOOTSTRAP_CACHE_MAX];
static size_t g_bootstrapped_count;
static pthread_mutex_t g_bootstrap_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    void *ptr;
} DevBuf;

static aclError devbuf_alloc(DevBuf *buf, size_t n)
{
    return aclrtMalloc(&buf->ptr, n, ACL_MEM_MALLOC_HUGE_FIRST);
}

static void devbuf_free(DevBuf *buf)
{
    if (buf->ptr != NULL) {
        aclrtFree(buf->ptr);
        buf->ptr = NULL;
    }
}

static int bootstrap_seen(uint64_t fp, int device_id)
{
    size_t i;

    for (i = 0; i < g_bootstrapped_count; ++i) {
        if (g_bootstrapped[i].fp == fp && g_bootstrapped[i].device_id == device_id) {
            return 1;
        }
    }
    return 0;
}

static int bootstrap_insert(uint64_t fp, int device_id)
{
    if (g_bootstrapped_count >= ESL_BOOTSTRAP_CACHE_MAX) {
        return 0;
    }
    g_bootstrapped[g_bootstrapped_count].fp = fp;
    g_bootstrapped[g_bootstrapped_count].device_id = device_id;
    g_bootstrapped_count++;
    return 1;
}

static void make_inner_basename(uint64_t fp, int device_id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "simpler_inner_%016lx_%d.so", (unsigned long)fp, device_id);
}

static void make_op_type(const char *base, uint64_t fp, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s_%016lx", base, (unsigned long)fp);
}

static int write_json(const char *path, const char *kernel_so, uint64_t fp)
{
    FILE *out;
    char init_op[128];
    char exec_op[128];

    out = fopen(path, "w");
    if (out == NULL) {
        return 0;
    }

    make_op_type(ESL_AICPU_INIT_NAME, fp, init_op, sizeof(init_op));
    make_op_type(ESL_AICPU_EXEC_NAME, fp, exec_op, sizeof(exec_op));

    fprintf(out, "{\n");
    fprintf(out, "  \"%s\": {\n", init_op);
    fprintf(out, "    \"opInfo\": {\n");
    fprintf(out, "      \"functionName\": \"%s\",\n", ESL_AICPU_INIT_NAME);
    fprintf(out, "      \"kernelSo\": \"%s\",\n", kernel_so);
    fprintf(out, "      \"opKernelLib\": \"AICPUKernel\",\n");
    fprintf(out, "      \"computeCost\": \"100\",\n");
    fprintf(out, "      \"engine\": \"DNN_VM_AICPU\",\n");
    fprintf(out, "      \"flagAsync\": \"False\",\n");
    fprintf(out, "      \"flagPartial\": \"False\",\n");
    fprintf(out, "      \"userDefined\": \"False\"\n");
    fprintf(out, "    }\n  },\n");
    fprintf(out, "  \"%s\": {\n", exec_op);
    fprintf(out, "    \"opInfo\": {\n");
    fprintf(out, "      \"functionName\": \"%s\",\n", ESL_AICPU_EXEC_NAME);
    fprintf(out, "      \"kernelSo\": \"%s\",\n", kernel_so);
    fprintf(out, "      \"opKernelLib\": \"AICPUKernel\",\n");
    fprintf(out, "      \"computeCost\": \"100\",\n");
    fprintf(out, "      \"engine\": \"DNN_VM_AICPU\",\n");
    fprintf(out, "      \"flagAsync\": \"False\",\n");
    fprintf(out, "      \"flagPartial\": \"False\",\n");
    fprintf(out, "      \"userDefined\": \"False\"\n");
    fprintf(out, "    }\n  }\n");
    fprintf(out, "}\n");
    fclose(out);
    return 1;
}

EslAicpuLoader *esl_aicpu_loader_create(void)
{
    return (EslAicpuLoader *)calloc(1, sizeof(EslAicpuLoader));
}

void esl_aicpu_loader_destroy(EslAicpuLoader *loader)
{
    if (loader == NULL) {
        return;
    }
    if (loader->binary_handle != NULL) {
        rtsBinaryUnload(loader->binary_handle);
    }
    if (loader->json_path[0] != '\0') {
        remove(loader->json_path);
    }
    free(loader);
}

int esl_aicpu_loader_bootstrap(EslAicpuLoader *loader, const void *dispatcher_so, size_t dispatcher_len,
                               const void *inner_so, size_t inner_len, rtStream_t stream, int device_id)
{
    DevBuf dev_dispatcher = {0};
    DevBuf dev_inner = {0};
    DevBuf dev_args = {0};
    aclError rc;
    char host_dev_args[160];
    struct Args {
        struct {
            uint64_t unused[5];
            uint64_t device_args_ptr;
            uint64_t pad[20];
        } k_args;
        char kernel_name[32];
        char so_name[32];
        char op_name[32];
    } args;
    rtAicpuArgsEx_t rt_args;
    rtError_t rrc;

    if (loader == NULL || dispatcher_so == NULL || dispatcher_len == 0 || inner_so == NULL || inner_len == 0) {
        return -1;
    }

    loader->device_id = device_id;
    loader->inner_fp = esl_fingerprint_bytes(inner_so, inner_len);
    make_inner_basename(loader->inner_fp, device_id, loader->inner_basename, sizeof(loader->inner_basename));

    pthread_mutex_lock(&g_bootstrap_mu);
    if (bootstrap_seen(loader->inner_fp, device_id)) {
        pthread_mutex_unlock(&g_bootstrap_mu);
        fprintf(stderr, "[esl_proxy] AICPU inner SO already bootstrapped fp=%016lx dev=%d\n",
                (unsigned long)loader->inner_fp, device_id);
        return 0;
    }
    pthread_mutex_unlock(&g_bootstrap_mu);

    rc = devbuf_alloc(&dev_dispatcher, dispatcher_len);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }
    rc = aclrtMemcpy(dev_dispatcher.ptr, dispatcher_len, dispatcher_so, dispatcher_len, ACL_MEMCPY_HOST_TO_DEVICE);
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }
    rc = devbuf_alloc(&dev_inner, inner_len);
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }
    rc = aclrtMemcpy(dev_inner.ptr, inner_len, inner_so, inner_len, ACL_MEMCPY_HOST_TO_DEVICE);
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_inner);
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }

    memset(host_dev_args, 0, sizeof(host_dev_args));
    {
        uint64_t v;

        v = (uint64_t)(uintptr_t)dev_dispatcher.ptr;
        memcpy(host_dev_args + 96, &v, sizeof(v));
        v = (uint64_t)dispatcher_len;
        memcpy(host_dev_args + 104, &v, sizeof(v));
        v = (uint64_t)(unsigned)device_id;
        memcpy(host_dev_args + 112, &v, sizeof(v));
        v = (uint64_t)(uintptr_t)dev_inner.ptr;
        memcpy(host_dev_args + 120, &v, sizeof(v));
        v = (uint64_t)inner_len;
        memcpy(host_dev_args + 128, &v, sizeof(v));
    }

    rc = devbuf_alloc(&dev_args, sizeof(host_dev_args));
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_inner);
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }
    rc = aclrtMemcpy(dev_args.ptr, sizeof(host_dev_args), host_dev_args, sizeof(host_dev_args),
                     ACL_MEMCPY_HOST_TO_DEVICE);
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_args);
        devbuf_free(&dev_inner);
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }

    memset(&args, 0, sizeof(args));
    args.k_args.device_args_ptr = (uint64_t)(uintptr_t)dev_args.ptr;
    strncpy(args.kernel_name, "DynTileFwkKernelServerInit", sizeof(args.kernel_name) - 1);
    strncpy(args.so_name, "libaicpu_extend_kernels.so", sizeof(args.so_name) - 1);

    memset(&rt_args, 0, sizeof(rt_args));
    rt_args.args = &args;
    rt_args.argsSize = sizeof(args);
    rt_args.kernelNameAddrOffset = offsetof(struct Args, kernel_name);
    rt_args.soNameAddrOffset = offsetof(struct Args, so_name);

    rrc = rtAicpuKernelLaunchExWithArgs(KERNEL_TYPE_AICPU_KFC, "AST_DYN_AICPU", 1, &rt_args, NULL, stream, 0);
    if (rrc != RT_ERROR_NONE) {
        fprintf(stderr, "rtAicpuKernelLaunchExWithArgs bootstrap failed: %d\n", (int)rrc);
        devbuf_free(&dev_args);
        devbuf_free(&dev_inner);
        devbuf_free(&dev_dispatcher);
        return (int)rrc;
    }
    rc = aclrtSynchronizeStream(stream);
    devbuf_free(&dev_args);
    devbuf_free(&dev_inner);
    devbuf_free(&dev_dispatcher);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }

    pthread_mutex_lock(&g_bootstrap_mu);
    bootstrap_insert(loader->inner_fp, device_id);
    pthread_mutex_unlock(&g_bootstrap_mu);
    fprintf(stderr, "[esl_proxy] bootstrapped inner SO %s\n", loader->inner_basename);
    return 0;
}

int esl_aicpu_loader_init(EslAicpuLoader *loader)
{
    char init_op[128];
    char exec_op[128];
    rtLoadBinaryOption_t option;
    rtLoadBinaryConfig_t load_config;
    rtError_t rc;

    if (loader == NULL || loader->inner_fp == 0) {
        return -1;
    }

    snprintf(loader->json_path, sizeof(loader->json_path), "/tmp/esl_proxy_inner_%016lx_%d.json",
             (unsigned long)loader->inner_fp, getpid());
    if (!write_json(loader->json_path, loader->inner_basename, loader->inner_fp)) {
        return -1;
    }

    memset(&option, 0, sizeof(option));
    option.optionId = RT_LOAD_BINARY_OPT_CPU_KERNEL_MODE;
    option.value.cpuKernelMode = 0;
    memset(&load_config, 0, sizeof(load_config));
    load_config.options = &option;
    load_config.numOpt = 1;

    rc = rtsBinaryLoadFromFile(loader->json_path, &load_config, &loader->binary_handle);
    if (rc != RT_ERROR_NONE) {
        fprintf(stderr, "rtsBinaryLoadFromFile failed: %d\n", (int)rc);
        return (int)rc;
    }

    make_op_type(ESL_AICPU_INIT_NAME, loader->inner_fp, init_op, sizeof(init_op));
    make_op_type(ESL_AICPU_EXEC_NAME, loader->inner_fp, exec_op, sizeof(exec_op));
    rc = rtsFuncGetByName(loader->binary_handle, init_op, &loader->init_handle);
    if (rc != RT_ERROR_NONE) {
        return (int)rc;
    }
    rc = rtsFuncGetByName(loader->binary_handle, exec_op, &loader->exec_handle);
    if (rc != RT_ERROR_NONE) {
        return (int)rc;
    }
    return 0;
}

int esl_aicpu_loader_launch(EslAicpuLoader *loader, rtStream_t stream, EslKernelArgs *k_args, int aicpu_num,
                            const char *symbol_name)
{
    rtFuncHandle handle;
    rtCpuKernelArgs_t cpu_args;
    rtLaunchKernelAttr_t attr;
    rtKernelLaunchCfg_t cfg;
    rtError_t rc;

    if (loader == NULL || k_args == NULL || symbol_name == NULL) {
        return -1;
    }

    if (strcmp(symbol_name, ESL_AICPU_INIT_NAME) == 0) {
        handle = loader->init_handle;
    } else if (strcmp(symbol_name, ESL_AICPU_EXEC_NAME) == 0) {
        handle = loader->exec_handle;
    } else {
        return -1;
    }

    memset(&cpu_args, 0, sizeof(cpu_args));
    cpu_args.baseArgs.args = k_args;
    cpu_args.baseArgs.argsSize = sizeof(EslKernelArgs);
    memset(&attr, 0, sizeof(attr));
    cfg.attrs = &attr;
    cfg.numAttrs = 0U;

    rc = rtsLaunchCpuKernel(handle, (uint32_t)aicpu_num, stream, &cfg, &cpu_args);
    if (rc != RT_ERROR_NONE) {
        fprintf(stderr, "rtsLaunchCpuKernel(%s) failed: %d\n", symbol_name, (int)rc);
        return (int)rc;
    }
    return 0;
}

/* ===== aicore_launcher.c ===== */

struct EslAicoreLauncher {
    char *binary;
    size_t binary_len;
    void *bin_handle;
};

typedef struct EslAicoreLauncher EslAicoreLauncher;

EslAicoreLauncher *esl_aicore_launcher_create(void)
{
    return (EslAicoreLauncher *)calloc(1, sizeof(EslAicoreLauncher));
}

void esl_aicore_launcher_destroy(EslAicoreLauncher *launcher)
{
    if (launcher == NULL) {
        return;
    }
    free(launcher->binary);
    free(launcher);
}

int esl_aicore_launcher_load(EslAicoreLauncher *launcher, const void *elf_data, size_t elf_len)
{
    char *copy;

    if (launcher == NULL || elf_data == NULL || elf_len == 0) {
        return -1;
    }
    copy = (char *)malloc(elf_len);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, elf_data, elf_len);
    free(launcher->binary);
    launcher->binary = copy;
    launcher->binary_len = elf_len;
    launcher->bin_handle = NULL;
    return 0;
}

int esl_aicore_launcher_launch(EslAicoreLauncher *launcher, rtStream_t stream, void *k_args_dev, int block_dim)
{
    rtDevBinary_t binary;
    rtArgsEx_t rt_args;
    rtTaskCfgInfo_t cfg;
    rtError_t rc;

    struct Args {
        EslKernelArgs *k_args;
    } args;

    if (launcher == NULL || k_args_dev == NULL || block_dim < 1) {
        return -1;
    }

    if (launcher->bin_handle == NULL) {
        memset(&binary, 0, sizeof(binary));
        binary.magic = RT_DEV_BINARY_MAGIC_ELF;
        binary.version = 0;
        binary.data = launcher->binary;
        binary.length = launcher->binary_len;
        rc = rtRegisterAllKernel(&binary, &launcher->bin_handle);
        if (rc != RT_ERROR_NONE) {
            fprintf(stderr, "rtRegisterAllKernel failed: %d\n", (int)rc);
            return (int)rc;
        }
    }

    args.k_args = (EslKernelArgs *)k_args_dev;
    memset(&rt_args, 0, sizeof(rt_args));
    rt_args.args = &args;
    rt_args.argsSize = sizeof(args);

    memset(&cfg, 0, sizeof(cfg));
    cfg.schemMode = RT_SCHEM_MODE_BATCH;

    rc = rtKernelLaunchWithHandleV2(launcher->bin_handle, 0, (uint32_t)block_dim, &rt_args, NULL, stream, &cfg);
    if (rc != RT_ERROR_NONE) {
        fprintf(stderr, "rtKernelLaunchWithHandleV2 failed: %d\n", (int)rc);
        return (int)rc;
    }
    return 0;
}

/* ===== host_runner.c (main) ===== */

typedef struct {
    void *ptr;
} DevMem;

static void devmem_free(DevMem *mem)
{
    if (mem->ptr != NULL) {
        aclrtFree(mem->ptr);
        mem->ptr = NULL;
    }
}

static aclError devmem_alloc(DevMem *mem, size_t n)
{
    return aclrtMalloc(&mem->ptr, n, ACL_MEM_MALLOC_HUGE_FIRST);
}

static void esl_host_sync_cann_log_level(int level)
{
    if (getenv("ASCEND_GLOBAL_LOG_LEVEL") != NULL) {
        return;
    }
    if (level < DLOG_DEBUG) {
        level = DLOG_DEBUG;
    }
    if (level > DLOG_ERROR) {
        level = DLOG_ERROR;
    }
    dlog_setlevel(-1, level, 0);
}

static int resolve_device_id(int cli_dev)
{
    const char *env = getenv("TASK_DEVICE");

    if (env != NULL && env[0] != '\0') {
        return atoi(env);
    }
    return cli_dev;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-d device_id] [--dispatcher PATH] [--aicpu PATH] [--aicore PATH]\n"
            "  defaults:\n"
            "    dispatcher = build/onboard/aicpu/libsimpler_aicpu_dispatcher.so\n"
            "    aicpu      = build/onboard/aicpu/libaicpu_kernel.so\n"
            "    aicore     = build/onboard/aicore/aicore_kernel.o\n",
            prog);
}

int main(int argc, char **argv)
{
    int device_id = 0;
    char dispatcher_path[512] = {0};
    char aicpu_path[512] = {0};
    char aicore_path[512] = {0};
    char *dispatcher_bytes = NULL;
    char *aicpu_bytes = NULL;
    char *aicore_bytes = NULL;
    size_t dispatcher_len = 0;
    size_t aicpu_len = 0;
    size_t aicore_len = 0;
    rtStream_t stream_aicpu = NULL;
    rtStream_t stream_aicore = NULL;
    EslAicpuLoader *loader;
    EslAicoreLauncher *aicore;
    uint64_t dev_regs = 0;
    EslRuntime host_runtime;
    DevMem dev_runtime = {0};
    DevMem dev_wall = {0};
    DevMem dev_k_args = {0};
    DevMem dev_payload = {0};
    EslKernelArgs k_args;
    size_t payload_bytes;
    int i;
    int rc;
    uint64_t stats[8];
    aclError stats_rc;
    const char *env;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dispatcher") == 0 && i + 1 < argc) {
            strncpy(dispatcher_path, argv[++i], sizeof(dispatcher_path) - 1);
        } else if (strcmp(argv[i], "--aicpu") == 0 && i + 1 < argc) {
            strncpy(aicpu_path, argv[++i], sizeof(aicpu_path) - 1);
        } else if (strcmp(argv[i], "--aicore") == 0 && i + 1 < argc) {
            strncpy(aicore_path, argv[++i], sizeof(aicore_path) - 1);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    device_id = resolve_device_id(device_id);

    if (dispatcher_path[0] == '\0') {
        env = getenv("ESL_PROXY_DISPATCHER_SO");
        strncpy(dispatcher_path, env ? env : "build/onboard/aicpu/libsimpler_aicpu_dispatcher.so",
                sizeof(dispatcher_path) - 1);
    }
    if (aicpu_path[0] == '\0') {
        env = getenv("ESL_PROXY_AICPU_SO");
        strncpy(aicpu_path, env ? env : "build/onboard/aicpu/libaicpu_kernel.so", sizeof(aicpu_path) - 1);
    }
    if (aicore_path[0] == '\0') {
        env = getenv("ESL_PROXY_AICORE_ELF");
        strncpy(aicore_path, env ? env : "build/onboard/aicore/aicore_kernel.o", sizeof(aicore_path) - 1);
    }

    if (!read_file(dispatcher_path, &dispatcher_bytes, &dispatcher_len)) {
        return 1;
    }
    if (!read_file(aicpu_path, &aicpu_bytes, &aicpu_len)) {
        free(dispatcher_bytes);
        return 1;
    }
    if (!read_file(aicore_path, &aicore_bytes, &aicore_len)) {
        free(dispatcher_bytes);
        free(aicpu_bytes);
        return 1;
    }

    fprintf(stderr, "[esl_proxy] onboard runner device=%d cores=%d aicpu_threads=%d launch_threads=%d\n", device_id,
            ESL_PROXY_FAKE_AICORE_COUNT, ESL_PROXY_AICPU_THREAD_NUM, ESL_PROXY_CANN_AICPU_LAUNCH_THREADS);
    fprintf(stderr, "[esl_proxy] dispatcher=%s aicpu=%s aicore=%s\n", dispatcher_path, aicpu_path, aicore_path);

    ACL_CHECK(aclInit(NULL), "aclInit");
    esl_host_sync_cann_log_level(ESL_PROXY_KARGS_LOG_LEVEL);
    ACL_CHECK(aclrtSetDevice(device_id), "aclrtSetDevice");
    ACL_CHECK(aclrtCreateStream(&stream_aicpu), "aclrtCreateStream aicpu");
    ACL_CHECK(aclrtCreateStream(&stream_aicore), "aclrtCreateStream aicore");

    loader = esl_aicpu_loader_create();
    rc = esl_aicpu_loader_bootstrap(loader, dispatcher_bytes, dispatcher_len, aicpu_bytes, aicpu_len, stream_aicpu,
                                    device_id);
    if (rc != 0) {
        fprintf(stderr, "bootstrap failed: %d\n", rc);
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }
    rc = esl_aicpu_loader_init(loader);
    if (rc != 0) {
        fprintf(stderr, "loader init failed: %d\n", rc);
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    rc = esl_host_init_aicore_regs((uint64_t)device_id, &dev_regs);
    if (rc != 0) {
        fprintf(stderr, "esl_host_init_aicore_regs failed: %d\n", rc);
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    memset(&host_runtime, 0, sizeof(host_runtime));
    host_runtime.worker_count = ESL_PROXY_FAKE_AICORE_COUNT;
    host_runtime.aicpu_thread_num = ESL_PROXY_AICPU_THREAD_NUM;

    ACL_CHECK(devmem_alloc(&dev_runtime, sizeof(EslRuntime)), "runtime GM");
    ACL_CHECK(devmem_alloc(&dev_wall, 8 * sizeof(uint64_t)), "device stats GM");
    ACL_CHECK(devmem_alloc(&dev_k_args, sizeof(EslKernelArgs)), "device KernelArgs GM");
    payload_bytes = (size_t)ESL_PROXY_FAKE_AICORE_COUNT * 2U * sizeof(EslFakeTaskArgs);
    ACL_CHECK(devmem_alloc(&dev_payload, payload_bytes), "fake task args GM");
    ACL_CHECK(aclrtMemset(dev_payload.ptr, payload_bytes, 0, payload_bytes), "zero payload GM");

    for (i = 0; i < ESL_PROXY_FAKE_AICORE_COUNT; ++i) {
        uint8_t *base = (uint8_t *)dev_payload.ptr + (size_t)i * 2U * sizeof(EslFakeTaskArgs);

        host_runtime.workers[i].task = (uint64_t)(uintptr_t)base;
    }

    ACL_CHECK(aclrtMemcpy(dev_runtime.ptr, sizeof(EslRuntime), &host_runtime, sizeof(EslRuntime), ACL_MEMCPY_HOST_TO_DEVICE),
              "H2D runtime");
    ACL_CHECK(aclrtMemset(dev_wall.ptr, 8 * sizeof(uint64_t), 0, 8 * sizeof(uint64_t)), "zero stats");

    memset(&k_args, 0, sizeof(k_args));
    k_args.runtime_args = (struct EslRuntime *)dev_runtime.ptr;
    k_args.regs = dev_regs;
    k_args.log_level = ESL_PROXY_KARGS_LOG_LEVEL;
    k_args.log_info_v = 5;
    k_args.device_wall_data_base = (uint64_t)(uintptr_t)dev_wall.ptr;
    k_args.device_id = (uint32_t)device_id;
    ACL_CHECK(aclrtMemcpy(dev_k_args.ptr, sizeof(EslKernelArgs), &k_args, sizeof(EslKernelArgs), ACL_MEMCPY_HOST_TO_DEVICE),
              "H2D KernelArgs");

    aicore = esl_aicore_launcher_create();
    rc = esl_aicore_launcher_load(aicore, aicore_bytes, aicore_len);
    if (rc != 0) {
        fprintf(stderr, "aicore load failed: %d\n", rc);
        esl_aicore_launcher_destroy(aicore);
        esl_aicpu_loader_destroy(loader);
        devmem_free(&dev_payload);
        devmem_free(&dev_k_args);
        devmem_free(&dev_wall);
        devmem_free(&dev_runtime);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    fprintf(stderr, "[esl_proxy] launching AICore kernel block_dim=%d\n", ESL_PROXY_FAKE_AICORE_COUNT);
    fprintf(stderr, "[esl_proxy] aicpu init launch\n");
    rc = esl_aicpu_loader_launch(loader, stream_aicpu, &k_args, 1, ESL_AICPU_INIT_NAME);
    if (rc != 0) {
        esl_aicore_launcher_destroy(aicore);
        esl_aicpu_loader_destroy(loader);
        devmem_free(&dev_payload);
        devmem_free(&dev_k_args);
        devmem_free(&dev_wall);
        devmem_free(&dev_runtime);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }
    fprintf(stderr, "[esl_proxy] aicpu init sync\n");
    ACL_CHECK(aclrtSynchronizeStream(stream_aicpu), "sync after aicpu init");

    fprintf(stderr, "[esl_proxy] aicore launch block_dim=%d (stream_aicore, concurrent with exec)\n",
            ESL_PROXY_FAKE_AICORE_COUNT);
    rc = esl_aicore_launcher_launch(aicore, stream_aicore, dev_k_args.ptr, ESL_PROXY_FAKE_AICORE_COUNT);
    if (rc != 0) {
        esl_aicore_launcher_destroy(aicore);
        esl_aicpu_loader_destroy(loader);
        devmem_free(&dev_payload);
        devmem_free(&dev_k_args);
        devmem_free(&dev_wall);
        devmem_free(&dev_runtime);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    fprintf(stderr, "[esl_proxy] aicpu exec launch\n");
    rc = esl_aicpu_loader_launch(loader, stream_aicpu, &k_args, ESL_PROXY_CANN_AICPU_LAUNCH_THREADS, ESL_AICPU_EXEC_NAME);
    if (rc != 0) {
        esl_aicore_launcher_destroy(aicore);
        esl_aicpu_loader_destroy(loader);
        devmem_free(&dev_payload);
        devmem_free(&dev_k_args);
        devmem_free(&dev_wall);
        devmem_free(&dev_runtime);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    fprintf(stderr, "[esl_proxy] aicpu exec sync\n");
    ACL_CHECK(aclrtSynchronizeStream(stream_aicpu), "sync after aicpu exec");
    fprintf(stderr, "[esl_proxy] done (aicore stream left to device reset)\n");
    fflush(stderr);

    memset(stats, 0, sizeof(stats));
    stats_rc = aclrtMemcpy(stats, sizeof(stats), dev_wall.ptr, sizeof(stats), ACL_MEMCPY_DEVICE_TO_HOST);
    if (stats_rc != ACL_SUCCESS) {
        fprintf(stderr, "D2H stats failed: %d\n", (int)stats_rc);
    }

    printf("esl_proxy onboard: task_cnt=%llu subtask_cnt=%llu completed_cnt=%llu wall_ns=%llu\n",
           (unsigned long long)stats[0], (unsigned long long)stats[1], (unsigned long long)stats[2],
           (unsigned long long)stats[3]);
    printf("[diag] commit=%llu n_uncomp=%llu first_uncomp=%u pred_cnt[first]=%u ready_cube=%u ready_vec=%u\n",
           (unsigned long long)stats[4], (unsigned long long)stats[5], (unsigned)(stats[6] & 0xffffffffULL),
           (unsigned)(stats[6] >> 32), (unsigned)(stats[7] & 0xffffffffULL), (unsigned)(stats[7] >> 32));
    fflush(stdout);

    free(dispatcher_bytes);
    free(aicpu_bytes);
    free(aicore_bytes);

    if (stats[0] == 0 || stats[2] != stats[0]) {
        fprintf(stderr, "esl_proxy onboard: FAIL (completed %llu != task_cnt %llu — DAG not fully scheduled)\n",
                (unsigned long long)stats[2], (unsigned long long)stats[0]);
        fflush(stderr);
        _Exit(1);
    }
    printf("esl_proxy onboard: OK (all %llu tasks completed)\n", (unsigned long long)stats[0]);
    fflush(stdout);
    _Exit(0);
}
