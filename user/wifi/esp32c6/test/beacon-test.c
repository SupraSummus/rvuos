/*
 * beacon.c on the host, under the sanitizers: a beacon and a probe response read,
 * a beacon without a DS parameter set taken for the channel it was heard on, other frames and a long SSID refused,
 * and every cut of a beacon, each in a buffer of its own length, read or refused without a byte read past it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "beacon.h"

static const uint8_t header[] = {
    0x80, 0x00, 0x00, 0x00,                   /* a beacon, no duration */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,       /* to everyone */
    0x02, 0x00, 0x00, 0x00, 0x00, 0x01,       /* from */
    0x02, 0x00, 0x00, 0x00, 0x00, 0x01,       /* the BSSID */
    0x00, 0x00,                               /* sequence */
    0, 0, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0x11, 0x04, /* timestamp, 100 TU, capabilities */
};
static const uint8_t ssid[] = { 0x00, 0x05, 'r', 'v', 'u', 'o', 's' };
static const uint8_t rates[] = { 0x01, 0x04, 0x82, 0x84, 0x8b, 0x96 };
static const uint8_t ds[] = { 0x03, 0x01, 0x06 };

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("beacon-test: FAIL %s\n", what);
        failures++;
    }
}

/* A frame of the header and the elements given, each a byte array with its size. */
static uint32_t frame(uint8_t *f, const uint8_t *const *elements, const uint32_t *sizes, uint32_t n)
{
    uint32_t len = sizeof(header);
    memcpy(f, header, len);
    for (uint32_t i = 0; i < n; i++) {
        memcpy(f + len, elements[i], sizes[i]);
        len += sizes[i];
    }
    return len;
}

/* beacon_read of len bytes in a buffer of exactly that many, so that the sanitizer sees a read past them. */
static int read_alone(const uint8_t *f, uint32_t len, struct beacon *b)
{
    uint8_t *copy = malloc(len ? len : 1);
    memcpy(copy, f, len);
    int ok = beacon_read(copy, len, 11, b);
    free(copy);
    return ok;
}

int main(void)
{
    uint8_t f[256];
    struct beacon b;
    const uint8_t *full[] = { ssid, rates, ds };
    const uint32_t full_sizes[] = { sizeof(ssid), sizeof(rates), sizeof(ds) };
    uint32_t len = frame(f, full, full_sizes, 3);

    check(read_alone(f, len, &b) == 1, "a beacon read");
    check(b.ssid_len == 5 && memcmp(b.ssid, "rvuos", 5) == 0, "its SSID");
    check(memcmp(b.bssid, header + 16, 6) == 0, "its BSSID");
    check(b.channel == 6, "its channel, from its DS parameter set");
    f[0] = 0x50;
    check(read_alone(f, len, &b) == 1, "a probe response read");
    f[0] = 0x40;
    check(read_alone(f, len, &b) == 0, "a probe request refused");
    f[0] = 0x80;

    for (uint32_t cut = 0; cut < len; cut++) {
        int ok = read_alone(f, cut, &b);
        int whole = cut == sizeof(header) + sizeof(ssid) || cut == sizeof(header) + sizeof(ssid) + sizeof(rates);
        if (ok != whole) {
            printf("beacon-test: FAIL a beacon cut at %u bytes %s\n", cut, ok ? "read" : "refused");
            failures++;
        }
        if (ok) {
            check(b.channel == 11, "a beacon without a DS parameter set, on the channel it was heard on");
        }
    }

    uint8_t long_ssid[2 + 33] = { 0x00, 33 };
    const uint8_t *too_long[] = { long_ssid, ds };
    const uint32_t too_long_sizes[] = { sizeof(long_ssid), sizeof(ds) };
    len = frame(f, too_long, too_long_sizes, 2);
    check(read_alone(f, len, &b) == 0, "an SSID of 33 bytes refused");

    const uint8_t *no_ssid[] = { rates, ds };
    const uint32_t no_ssid_sizes[] = { sizeof(rates), sizeof(ds) };
    len = frame(f, no_ssid, no_ssid_sizes, 2);
    check(read_alone(f, len, &b) == 0, "a beacon without an SSID refused");

    printf("beacon-test: %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
