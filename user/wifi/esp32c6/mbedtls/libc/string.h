#ifndef RVUOS_WIFI_ESP_LIBC_STRING_H
#define RVUOS_WIFI_ESP_LIBC_STRING_H

/* What Mbed TLS takes of a C library, which the driver lacks: user/lib/libc.c's, glue.c's and the ROM's functions. */

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

#endif
