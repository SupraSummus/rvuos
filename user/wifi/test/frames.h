#ifndef RVUOS_WIFI_TEST_FRAMES_H
#define RVUOS_WIFI_TEST_FRAMES_H

/*
 * Frames a network would send the Wi-Fi system's IP stack, for its tests on the host:
 * Ethernet around IPv4 around UDP, from the router's address, with their checksums.
 */

#include <stdint.h>
#include <string.h>

static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void put_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* The Internet checksum over len bytes, starting from sum; zero over bytes that hold their own checksum. */
static inline uint16_t checksum(const uint8_t *p, uint32_t len, uint32_t sum)
{
    for (; len > 1; len -= 2, p += 2) {
        sum += be16(p);
    }
    if (len) {
        sum += (uint32_t)p[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* UDP's pseudo header, summed, for checksum's start. */
static inline uint32_t pseudo_sum(uint32_t from, uint32_t to, uint32_t udp_len)
{
    const uint8_t *f = (const uint8_t *)&from, *t = (const uint8_t *)&to;
    return be16(f) + be16(f + 2) + be16(t) + be16(t + 2) + 17u + udp_len;
}

static const uint8_t pico[6] = { 0x2c, 0xcf, 0x67, 0x01, 0x02, 0x03 };
static const uint8_t router[6] = { 0x02, 0x00, 0x00, 0xaa, 0xbb, 0xcc };

/* An Ethernet frame with an IPv4 header around a payload already at f + 34; returns its length. */
static inline uint32_t ip_frame(uint8_t *f, const uint8_t *to_mac, uint32_t from, uint32_t to, uint32_t proto, uint32_t len)
{
    memcpy(f, to_mac, 6);
    memcpy(f + 6, router, 6);
    put_be16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    put_be16(ip + 2, 20 + len);
    put_be16(ip + 4, 1);
    put_be16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = (uint8_t)proto;
    put_be16(ip + 10, 0);
    memcpy(ip + 12, &from, 4);
    memcpy(ip + 16, &to, 4);
    put_be16(ip + 10, checksum(ip, 20, 0));
    return 14 + 20 + len;
}

static inline uint32_t udp_frame(uint8_t *f, const uint8_t *to_mac, uint32_t from, uint32_t to, uint16_t sport, uint16_t dport,
                          const uint8_t *data, uint32_t len)
{
    uint8_t *u = f + 34;
    put_be16(u, sport);
    put_be16(u + 2, dport);
    put_be16(u + 4, 8 + len);
    put_be16(u + 6, 0);
    memcpy(u + 8, data, len);
    put_be16(u + 6, checksum(u, 8 + len, pseudo_sum(from, to, 8 + len)));
    return ip_frame(f, to_mac, from, to, 17, 8 + len);
}

#endif
