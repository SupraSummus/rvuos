#ifndef RVUOS_WIFI_CONF_H
#define RVUOS_WIFI_CONF_H

/*
 * The configuration of a Wi-Fi system, as WIFI_CONFIG's file leaves it in the input region:
 * lines of key=value, of which each root task reads the keys it knows.
 * It ends at its first NUL, which the loaders' --text writes after the file,
 * since RAM keeps what an earlier run's longer file left past it.
 */

#include <stdint.h>

#include "lib/libc.h"

/* Copies the value of a "key=value" line of the configuration into out, if the key is there. */
static inline void config_value(const char *conf, uint32_t len, const char *key, char *out, uint32_t room)
{
    uint32_t klen = strlen(key);
    for (uint32_t i = 0; i < len && conf[i] != '\0'; i++) {
        if ((i == 0 || conf[i - 1] == '\n') && i + klen < len && memcmp(conf + i, key, klen) == 0 &&
            conf[i + klen] == '=') {
            uint32_t n = 0;
            for (uint32_t j = i + klen + 1; j < len && conf[j] != '\n' && conf[j] != '\0' && n + 1 < room; j++) {
                out[n++] = conf[j];
            }
            out[n] = '\0';
            return;
        }
    }
}

/* The number of a "key=value" line of the configuration, or dflt if the key is not there. */
static inline uint32_t config_number(const char *conf, uint32_t len, const char *key, uint32_t dflt)
{
    char value[12];
    value[0] = '\0';
    config_value(conf, len, key, value, sizeof(value));
    uint32_t n = dflt;
    for (uint32_t i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
        n = (i == 0 ? 0 : n * 10u) + (uint32_t)(value[i] - '0');
    }
    return n;
}

/* Whether a "key=value" line's value, words apart by commas, holds word. */
static inline int config_has(const char *conf, uint32_t len, const char *key, const char *word)
{
    char value[64];
    value[0] = '\0';
    config_value(conf, len, key, value, sizeof(value));
    for (const char *v = value; *v != '\0';) {
        uint32_t i = 0;
        while (word[i] != '\0' && v[i] == word[i]) {
            i++;
        }
        if (word[i] == '\0' && (v[i] == ',' || v[i] == '\0')) {
            return 1;
        }
        while (*v != '\0' && *v++ != ',') {
        }
    }
    return 0;
}

/*
 * The MAC address of a "key=value" line of the configuration, six bytes in hexadecimal apart by colons, into mac:
 * 1, or 0 if the key is not there or holds no such address.
 */
static inline int config_mac(const char *conf, uint32_t len, const char *key, uint8_t mac[6])
{
    char value[18];
    uint8_t got[6];
    value[0] = '\0';
    config_value(conf, len, key, value, sizeof(value));
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t b = 0;
        for (uint32_t j = 0; j < 2; j++) {
            char c = (char)(value[3 * i + j] | 0x20); /* a letter in lower case; a digit or ':' as it is */
            if (c >= '0' && c <= '9') {
                b = b * 16u + (uint32_t)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                b = b * 16u + (uint32_t)(c - 'a' + 10);
            } else {
                return 0;
            }
        }
        if (value[3 * i + 2] != (i == 5 ? '\0' : ':')) {
            return 0;
        }
        got[i] = (uint8_t)b;
    }
    memcpy(mac, got, 6);
    return 1;
}

#endif
