#ifndef TLS_RNG_H
#define TLS_RNG_H

#include <stddef.h>

// CTR_DRBG seed function (see tls_rng.c). Pass to mbedtls_ctr_drbg_seed()
// instead of mbedtls_entropy_func to bypass the platform entropy poll.
int tls_rng(void *p_rng, unsigned char *output, size_t len);

#endif
