#include "elf_fingerprint.h"

#include <cstring>

static bool read_elf_build_id(const char *data, size_t len, uint64_t *out)
{
    if (len < 64) {
        return false;
    }
    if (std::memcmp(data, "\x7f""ELF", 4) != 0 || data[4] != 2) {
        return false;
    }

    uint64_t e_shoff;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
    std::memcpy(&e_shoff, data + 40, 8);
    std::memcpy(&e_shentsize, data + 58, 2);
    std::memcpy(&e_shnum, data + 60, 2);
    std::memcpy(&e_shstrndx, data + 62, 2);
    if (e_shentsize != 64 || e_shoff > len || (uint64_t)e_shentsize * e_shnum > len - e_shoff ||
        e_shstrndx >= e_shnum) {
        return false;
    }

    const char *strtab = nullptr;
    uint64_t strtab_sz = 0;
    {
        const char *sh = data + e_shoff + (uint64_t)e_shentsize * e_shstrndx;
        uint64_t off;
        std::memcpy(&off, sh + 24, 8);
        std::memcpy(&strtab_sz, sh + 32, 8);
        if (off > len || strtab_sz > len - off) {
            return false;
        }
        strtab = data + off;
    }

    for (uint16_t i = 0; i < e_shnum; ++i) {
        const char *sh = data + e_shoff + (uint64_t)e_shentsize * i;
        uint32_t sh_name, sh_type;
        uint64_t sh_off, sh_size;
        std::memcpy(&sh_name, sh + 0, 4);
        std::memcpy(&sh_type, sh + 4, 4);
        std::memcpy(&sh_off, sh + 24, 8);
        std::memcpy(&sh_size, sh + 32, 8);
        if (sh_type != 7 || sh_name >= strtab_sz) {
            continue;
        }
        if (std::strcmp(strtab + sh_name, ".note.gnu.build-id") != 0) {
            continue;
        }
        if (sh_size < 16 || sh_off > len || sh_size > len - sh_off) {
            return false;
        }
        const char *p = data + sh_off;
        uint32_t namesz, descsz, type;
        std::memcpy(&namesz, p + 0, 4);
        std::memcpy(&descsz, p + 4, 4);
        std::memcpy(&type, p + 8, 4);
        if (type != 3 || descsz < 8 || namesz > sh_size) {
            return false;
        }
        size_t name_aligned = (namesz + 3u) & ~3u;
        if (name_aligned > sh_size - 12 || sh_size - 12 - name_aligned < 8) {
            return false;
        }
        const char *desc = p + 12 + name_aligned;
        std::memcpy(out, desc, 8);
        return true;
    }
    return false;
}

static uint64_t fnv1a64(const char *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned char)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t esl_fingerprint_bytes(const void *data, size_t len)
{
    uint64_t v = 0;
    if (read_elf_build_id(reinterpret_cast<const char *>(data), len, &v)) {
        return v;
    }
    return fnv1a64(reinterpret_cast<const char *>(data), len);
}
