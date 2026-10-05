#ifndef RVUOS_WIFI_ESP_MBEDTLS_CONFIG_H
#define RVUOS_WIFI_ESP_MBEDTLS_CONFIG_H

/*
 * Mbed TLS's configuration for the ESP32-C6's Wi-Fi driver, MBEDTLS_CONFIG_FILE, which test/sae-test.c builds with too:
 * big numbers and P-256 for ec.c, nothing else.
 * Memory comes from the driver's heap, see heap.c, and of a C library Mbed TLS takes only what libc/ declares.
 * The fixed point speed-up is off: SAE multiplies no fixed point, and it would keep a table for the generator.
 */

#include <stddef.h>

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_CALLOC_MACRO osi_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO   osi_free

void *osi_calloc(size_t n, size_t size);
void osi_free(void *p);

#endif
