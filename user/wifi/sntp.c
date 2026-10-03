/*
 * DNS and SNTP messages; see sntp.h.
 */

#include "sntp.h"

#include "bytes.h"
#include "lib/libc.h"

#define DNS_HEADER    12u
#define DNS_RESPONSE  0x8000u
#define DNS_RECURSE   0x0100u
#define DNS_TYPE_A    1u
#define DNS_CLASS_IN  1u
#define NTP_UNIX      2208988800u /* seconds from 1900 to 1970, which NTP counts from */
#define NTP_ORIGIN    24u
#define NTP_TRANSMIT  40u

uint32_t dns_query(uint8_t *out, uint16_t id, const char *name)
{
    memset(out, 0, DNS_HEADER);
    put_be16(out, id);
    put_be16(out + 2, DNS_RECURSE);
    put_be16(out + 4, 1);
    /* The name as labels, each its length and its bytes, and a zero. */
    uint32_t at = DNS_HEADER;
    for (const char *label = name; *label != '\0';) {
        uint32_t n = 0;
        while (label[n] != '\0' && label[n] != '.') {
            n++;
        }
        if (n == 0 || n > 63u || at + 1u + n + 5u > DNS_HEADER + 255u) {
            return 0;
        }
        out[at++] = (uint8_t)n;
        memcpy(out + at, label, n);
        at += n;
        label += n;
        if (*label == '.') {
            label++;
        }
    }
    if (at == DNS_HEADER) {
        return 0;
    }
    out[at++] = 0;
    put_be16(out + at, DNS_TYPE_A);
    put_be16(out + at + 2, DNS_CLASS_IN);
    return at + 4u;
}

/* The offset past the name at at, labels or a pointer to them, or 0 if it runs past len. */
static uint32_t skip_name(const uint8_t *msg, uint32_t len, uint32_t at)
{
    while (at < len) {
        uint32_t b = msg[at];
        if ((b & 0xc0u) == 0xc0u) {
            return at + 2u <= len ? at + 2u : 0;
        }
        if (b == 0) {
            return at + 1u;
        }
        at += 1u + b;
    }
    return 0;
}

uint32_t dns_answer(const uint8_t *msg, uint32_t len, uint16_t id)
{
    if (len < DNS_HEADER || be16(msg) != id) {
        return 0;
    }
    uint32_t flags = be16(msg + 2);
    if (!(flags & DNS_RESPONSE) || (flags & 0x7800u) != 0 || (flags & 0x000fu) != 0) {
        return 0;
    }
    uint32_t questions = be16(msg + 4), answers = be16(msg + 6);
    uint32_t at = DNS_HEADER;
    /* Each record takes bytes, so neither count can take the walk past len. */
    for (uint32_t i = 0; i < questions; i++) {
        at = skip_name(msg, len, at);
        if (at == 0 || at + 4u > len) {
            return 0;
        }
        at += 4u;
    }
    for (uint32_t i = 0; i < answers; i++) {
        at = skip_name(msg, len, at);
        if (at == 0 || at + 10u > len) {
            return 0;
        }
        uint32_t type = be16(msg + at), class = be16(msg + at + 2), size = be16(msg + at + 8);
        at += 10u;
        if (at + size > len) {
            return 0;
        }
        if (type == DNS_TYPE_A && class == DNS_CLASS_IN && size == 4u) {
            uint32_t ip;
            memcpy(&ip, msg + at, 4);
            return ip;
        }
        at += size;
    }
    return 0;
}

void ntp_query(uint8_t *out, uint32_t nonce)
{
    memset(out, 0, NTP_MSG);
    out[0] = 0x23; /* no leap warning, version 4, a client */
    put_be32(out + NTP_TRANSMIT, nonce);
    put_be32(out + NTP_TRANSMIT + 4, ~nonce);
}

uint32_t ntp_answer(const uint8_t *msg, uint32_t len, uint32_t nonce)
{
    if (len < NTP_MSG) {
        return 0;
    }
    uint32_t leap = msg[0] >> 6, mode = msg[0] & 7u, stratum = msg[1];
    if (leap == 3u || mode != 4u || stratum == 0 || stratum > 15u) {
        return 0; /* not synchronised, not a server's, or a kiss of death */
    }
    if (be32(msg + NTP_ORIGIN) != nonce || be32(msg + NTP_ORIGIN + 4) != ~nonce) {
        return 0;
    }
    uint32_t seconds = be32(msg + NTP_TRANSMIT);
    /* NTP's count wraps in 2036; the difference wraps with it, and holds until 2106. */
    return seconds == 0 ? 0 : seconds - NTP_UNIX;
}

static char *two(char *p, uint32_t v)
{
    p[0] = (char)('0' + v / 10u % 10u);
    p[1] = (char)('0' + v % 10u);
    return p + 2;
}

void utc_format(char *out, uint32_t seconds)
{
    /* The civil date of a count of days, as in Howard Hinnant's days_from_civil, backwards. */
    uint32_t z = seconds / 86400u + 719468u, rest = seconds % 86400u;
    uint32_t era = z / 146097u, doe = z - era * 146097u;
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    uint32_t day = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t month = mp < 10u ? mp + 3u : mp - 9u;
    uint32_t year = yoe + era * 400u + (month <= 2u);
    char *p = two(two(out, year / 100u), year);
    *p++ = '-';
    p = two(p, month);
    *p++ = '-';
    p = two(p, day);
    *p++ = ' ';
    p = two(p, rest / 3600u);
    *p++ = ':';
    p = two(p, rest / 60u % 60u);
    *p++ = ':';
    p = two(p, rest % 60u);
    memcpy(p, " UTC", 5);
}
