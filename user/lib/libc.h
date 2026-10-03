#ifndef RVUOS_LIB_LIBC_H
#define RVUOS_LIB_LIBC_H

/*
 * What a program has of the C library: the memory functions the compiler may call, and strlen.
 * The compiler calls them for a struct's initialiser or copy even where the source names none,
 * and on ARM its __aeabi_ ones, which libc.c has too.
 */

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
