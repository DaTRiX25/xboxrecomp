/*
 * xr_sha256.h - SHA-256 for the data installer (disc_install.h): every file
 * copied from the user's disc is hashed on the way and compared with the
 * manifest. Plain C, no library, so the installer is the same code on every
 * host (the kernel's OpenSSL is not on Windows).
 */
#ifndef XR_SHA256_H
#define XR_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t  buf[64];
    unsigned fill;
} xr_sha256;

void xr_sha256_init(xr_sha256 *s);
void xr_sha256_update(xr_sha256 *s, const void *data, size_t n);
void xr_sha256_final(xr_sha256 *s, uint8_t out[32]);

/* 64 lower-case hex digits and a terminator. */
void xr_sha256_hex(const uint8_t digest[32], char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* XR_SHA256_H */
