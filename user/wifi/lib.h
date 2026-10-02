#ifndef RVUOS_WIFI_LIB_H
#define RVUOS_WIFI_LIB_H

/*
 * What every process of the Wi-Fi system needs and the C library would give:
 * the memory functions the compiler may call, and printing.
 * Nothing here keeps state, so a process other than the root task may call all of it.
 */

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

/* Where a process's text goes: a function and what it is handed. */
struct out {
    void (*put)(void *to, char c);
    void *to;
};

void print(const struct out *o, const char *s);
void print_hex(const struct out *o, uint32_t v);
void print_dec(const struct out *o, uint32_t v);

#define REG32(addr) (*(volatile uint32_t *)(addr))

#endif
