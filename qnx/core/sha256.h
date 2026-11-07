/* SHA-256 (FIPS 180-4), portable C99, for the hash-chained record. */
#ifndef FE_SHA256_H
#define FE_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} fe_sha256_t;

void fe_sha256_init(fe_sha256_t *s);
void fe_sha256_update(fe_sha256_t *s, const void *data, size_t n);
void fe_sha256_final(fe_sha256_t *s, uint8_t out[32]);
void fe_sha256_hex(const void *data, size_t n, char hex[65]);

#endif
