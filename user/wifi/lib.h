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
void *memmove(void *dst, const void *src, size_t n);
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

/*
 * A ring of text in memory a writer shares with a reader, as a child's page holds its log:
 * head counts every byte the writer put, and byte n lies at bytes[n % PLOG_SIZE].
 * The reader keeps its own count, and finds itself overtaken when head runs more than PLOG_SIZE ahead.
 */
#define PLOG_SIZE 1536u
struct plog {
    volatile uint32_t head;
    char bytes[PLOG_SIZE];
};

/* Puts a byte; 1 at the end of a line, when the reader is to be told. */
int plog_put(struct plog *l, char c);
/* Hands the reader every byte past taken, and returns the new count. */
uint32_t plog_take(const struct plog *l, uint32_t taken, void (*put)(char));

#define REG32(addr) (*(volatile uint32_t *)(addr))

#endif
