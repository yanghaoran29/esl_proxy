#ifndef ESL_PROXY_ELF_FINGERPRINT_H
#define ESL_PROXY_ELF_FINGERPRINT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t esl_fingerprint_bytes(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ELF_FINGERPRINT_H */
