#ifndef RVUOS_WIFI_SNTP_H
#define RVUOS_WIFI_SNTP_H

/*
 * The time from the network: a DNS question for a name's address, RFC 1035,
 * and an SNTP request and its answer, RFC 4330.
 * It builds and reads messages and nothing else, with no system call and no global, so the host tests it.
 */

#include <stdint.h>

#define DNS_PORT    53u
#define DNS_MSG_MAX 512u
#define NTP_PORT    123u
#define NTP_MSG     48u

/* A question for the IPv4 address of name, with id, into out of DNS_MSG_MAX bytes: its length, or 0 if the name is not one. */
uint32_t dns_query(uint8_t *out, uint16_t id, const char *name);
/* The first IPv4 address an answer to id gives, in network order, or 0. */
uint32_t dns_answer(const uint8_t *msg, uint32_t len, uint16_t id);

/* An SNTP request, NTP_MSG bytes, carrying nonce as its transmit time, which the answer returns. */
void ntp_query(uint8_t *out, uint32_t nonce);
/* The time a synchronised server's answer to nonce gives, in seconds since 1970, or 0. */
uint32_t ntp_answer(const uint8_t *msg, uint32_t len, uint32_t nonce);

#define UTC_TEXT 24u /* "2026-10-03 12:00:00 UTC" and its zero */
/* The date and time of seconds since 1970, in UTC, into out of UTC_TEXT bytes. */
void utc_format(char *out, uint32_t seconds);

#endif
