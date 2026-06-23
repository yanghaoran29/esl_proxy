/*
 * Shared onboard utilities: file I/O, fingerprinting, dispatcher offsets.
 * AICPU CANN logging lives in onboard_log.h (implementation in aicpu_kernel.c).
 */
#ifndef ESL_PROXY_ONBOARD_TOOLS_H
#define ESL_PROXY_ONBOARD_TOOLS_H

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#define GET_TID() syscall(SYS_gettid)
#else
#define GET_TID() 0
#endif

#define DISPATCHER_KERNEL_ARGS_DEVICE_ARGS_OFF 40
#define DISPATCHER_DEVICE_ARGS_AICPU_SO_BIN_OFF 96
#define DISPATCHER_DEVICE_ARGS_AICPU_SO_LEN_OFF 104
#define DISPATCHER_DEVICE_ARGS_DEVICE_ID_OFF 112
#define DISPATCHER_DEVICE_ARGS_INNER_SO_BIN_OFF 120
#define DISPATCHER_DEVICE_ARGS_INNER_SO_LEN_OFF 128

static inline int32_t popcount64(uint64_t v)
{
    return (int32_t)__builtin_popcountll(v);
}

static inline uint64_t fnv1a64(const char *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;

    for (i = 0; i < len; ++i) {
        h ^= (unsigned char)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static inline int read_elf_build_id(const char *data, size_t len, uint64_t *out)
{
    uint64_t e_shoff;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
    const char *strtab;
    uint64_t strtab_sz;
    uint16_t i;

    if (len < 64) {
        return 0;
    }
    if (memcmp(data, "\x7f""ELF", 4) != 0 || data[4] != 2) {
        return 0;
    }

    memcpy(&e_shoff, data + 40, 8);
    memcpy(&e_shentsize, data + 58, 2);
    memcpy(&e_shnum, data + 60, 2);
    memcpy(&e_shstrndx, data + 62, 2);
    if (e_shentsize != 64 || e_shoff > len || (uint64_t)e_shentsize * e_shnum > len - e_shoff || e_shstrndx >= e_shnum) {
        return 0;
    }

    {
        const char *sh = data + e_shoff + (uint64_t)e_shentsize * e_shstrndx;
        uint64_t off;

        memcpy(&off, sh + 24, 8);
        memcpy(&strtab_sz, sh + 32, 8);
        if (off > len || strtab_sz > len - off) {
            return 0;
        }
        strtab = data + off;
    }

    for (i = 0; i < e_shnum; ++i) {
        const char *sh = data + e_shoff + (uint64_t)e_shentsize * i;
        uint32_t sh_name;
        uint32_t sh_type;
        uint64_t sh_off;
        uint64_t sh_size;

        memcpy(&sh_name, sh + 0, 4);
        memcpy(&sh_type, sh + 4, 4);
        memcpy(&sh_off, sh + 24, 8);
        memcpy(&sh_size, sh + 32, 8);
        if (sh_type != 7 || sh_name >= strtab_sz) {
            continue;
        }
        if (strcmp(strtab + sh_name, ".note.gnu.build-id") != 0) {
            continue;
        }
        if (sh_size < 16 || sh_off > len || sh_size > len - sh_off) {
            return 0;
        }
        {
            const char *p = data + sh_off;
            uint32_t namesz;
            uint32_t descsz;
            uint32_t type;
            size_t name_aligned;
            const char *desc;

            memcpy(&namesz, p + 0, 4);
            memcpy(&descsz, p + 4, 4);
            memcpy(&type, p + 8, 4);
            if (type != 3 || descsz < 8 || namesz > sh_size) {
                return 0;
            }
            name_aligned = (namesz + 3u) & ~3u;
            if (name_aligned > sh_size - 12 || sh_size - 12 - name_aligned < 8) {
                return 0;
            }
            desc = p + 12 + name_aligned;
            memcpy(out, desc, 8);
            return 1;
        }
    }
    return 0;
}

static inline uint64_t esl_fingerprint_bytes(const void *data, size_t len)
{
    uint64_t v = 0;

    if (read_elf_build_id((const char *)data, len, &v)) {
        return v;
    }
    return fnv1a64((const char *)data, len);
}

static inline int64_t *grow_array(int64_t **arr, size_t *cap, size_t *len, int64_t value)
{
    if (*len >= *cap) {
        size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
        int64_t *new_arr = (int64_t *)realloc(*arr, new_cap * sizeof(int64_t));

        if (new_arr == NULL) {
            return NULL;
        }
        *arr = new_arr;
        *cap = new_cap;
    }
    (*arr)[(*len)++] = value;
    return *arr;
}

static inline int read_file(const char *path, char **out_data, size_t *out_len)
{
    FILE *f;
    long sz;
    char *buf;
    size_t nread;

    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "seek %s failed\n", path);
        fclose(f);
        return 0;
    }
    sz = ftell(f);
    if (sz <= 0) {
        fprintf(stderr, "empty file: %s\n", path);
        fclose(f);
        return 0;
    }
    buf = (char *)malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        free(buf);
        fclose(f);
        return 0;
    }
    nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nread != (size_t)sz) {
        free(buf);
        return 0;
    }
    *out_data = buf;
    *out_len = (size_t)sz;
    return 1;
}

static inline int write_bytes(const char *path, const char *data, uint64_t len)
{
    char tmp_path[320];
    FILE *f;
    size_t written;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());
    f = fopen(tmp_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "open %s for write failed: %s\n", tmp_path, strerror(errno));
        return 0;
    }
    written = fwrite(data, 1, (size_t)len, f);
    if (written != (size_t)len || ferror(f)) {
        fprintf(stderr, "write %s failed\n", tmp_path);
        fclose(f);
        unlink(tmp_path);
        return 0;
    }
    fclose(f);
    (void)chmod(tmp_path, 0755);
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "rename %s -> %s failed: %s\n", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return 0;
    }
    return 1;
}

#endif /* ESL_PROXY_ONBOARD_TOOLS_H */
