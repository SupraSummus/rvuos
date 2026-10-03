#ifndef RVUOS_WIFI_BYTES_H
#define RVUOS_WIFI_BYTES_H

/* Numbers in big-endian order, the network's, read from bytes and written into them. */

#include <stdint.h>

static inline uint32_t be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static inline uint32_t be32(const uint8_t *p)
{
    return be16(p) << 16 | be16(p + 2);
}

static inline void put_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
    put_be16(p, v >> 16);
    put_be16(p + 2, v);
}

#endif
