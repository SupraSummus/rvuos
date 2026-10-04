#ifndef RVUOS_WIFI_CONF_H
#define RVUOS_WIFI_CONF_H

/*
 * The configuration of a Wi-Fi system, as WIFI_CONFIG's file leaves it in the input region:
 * lines of key=value, of which each root task reads the keys it knows.
 */

#include <stdint.h>

#include "lib/libc.h"

/* Copies the value of a "key=value" line of the configuration into out, if the key is there. */
static inline void config_value(const char *conf, uint32_t len, const char *key, char *out, uint32_t room)
{
    uint32_t klen = strlen(key);
    for (uint32_t i = 0; i < len; i++) {
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

#endif
