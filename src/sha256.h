/* sha256.h - SHA-256 and HMAC-SHA256 (FIPS 180-4 / RFC 2104), no dependencies (C99) */
#ifndef CSQLPAGE_SHA256_H
#define CSQLPAGE_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t      h[8];
    uint64_t      bytes;      /* total message length so far */
    unsigned char buf[64];
    size_t        buflen;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t n);
void sha256_final(sha256_t *s, unsigned char out[32]);

void hmac_sha256(const void *key, size_t keylen,
                 const void *msg, size_t msglen, unsigned char out[32]);

#endif
