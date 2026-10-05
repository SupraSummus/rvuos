#ifndef INCLUDES_H
#define INCLUDES_H

/*
 * hostap's includes.h for the Wi-Fi driver of the ESP32-C6.
 * Each of hostap's files the driver builds, and each of the driver's own that reads hostap's headers,
 * is compiled with this one first (-include), under the include guard of hostap's own,
 * which then adds nothing.
 * hostap's brings in a hosted C library and build_config.h; the driver has no C library,
 * so this one says what hostap takes from one, which the driver, the user library and the ROM define,
 * and holds the configuration.
 */

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The configuration: the 4-way and group key handshakes of rsn_supp/, with hostap's own crypto,
 * and WPA3's SAE of common/, whose curves are ec.c's on Mbed TLS.
 * No RC4, so no WPA's TKIP key data, and no pool of hostap's own for randomness:
 * os_get_random reads the chip's generator, see hostap.c.
 */
#define CONFIG_CRYPTO_INTERNAL
#define CONFIG_SHA256
#define CONFIG_SAE
#define CONFIG_NO_RC4
#define CONFIG_NO_RANDOM_POOL

/* What common.h asks a system's headers for. */
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __BYTE_ORDER    __LITTLE_ENDIAN
#define bswap_16        __builtin_bswap16
#define bswap_32        __builtin_bswap32
#define bswap_64        __builtin_bswap64

typedef int32_t ssize_t;
typedef struct FILE FILE; /* os.h names it, in a function nothing here calls */

/* hostap's allocations, on the driver's heap; see heap.c. */
void *osi_malloc(size_t size);
void *osi_realloc(void *p, size_t size);
void osi_free(void *p);
#define os_malloc(s)     osi_malloc(s)
#define os_realloc(p, s) osi_realloc((p), (s))
#define os_free(p)       osi_free(p)

/* The C library hostap calls, which os.h names it by: user/lib/libc.c's, glue.c's, hostap.c's and the ROM's. */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strdup(const char *s);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
void qsort(void *base, size_t n, size_t size, int (*compare)(const void *, const void *));
int abs(int v);
int atoi(const char *s);
long strtol(const char *s, char **end, int base);

/* A wpabuf written past its end is a fault, as hostap's abort makes it on a system. */
#define abort() __builtin_trap()

static inline int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

static inline int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline int isspace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

static inline int isprint(int c)
{
    return c >= ' ' && c <= '~';
}

#endif
