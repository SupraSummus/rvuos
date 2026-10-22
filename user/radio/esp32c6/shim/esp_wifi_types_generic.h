/*
 * SPDX-FileCopyrightText: Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RVUOS_WIFI_SHIM_ESP_WIFI_TYPES_GENERIC_H
#define RVUOS_WIFI_SHIM_ESP_WIFI_TYPES_GENERIC_H

/*
 * What ESP-IDF's esp_wifi_regulatory.c needs of esp_wifi_types_generic.h, as ESP-IDF 4d59230d declares it,
 * for a chip of 2.4 GHz alone: the driver compiles that file, which tools/esp-fetch.py fetches, against this.
 */

#include <stdint.h>

#define WIFI_MAX_REGULATORY_RULE_NUM 2

typedef struct {
    uint8_t start_channel;
    uint8_t end_channel;
    uint16_t max_bandwidth : 3;
    uint16_t max_eirp : 6;
    uint16_t is_dfs : 1;
    uint16_t reserved : 6;
} wifi_reg_rule_t;

typedef struct {
    uint8_t n_reg_rules;
    wifi_reg_rule_t reg_rules[WIFI_MAX_REGULATORY_RULE_NUM];
} wifi_regulatory_t;

typedef struct {
    char cn[2];
    uint8_t regulatory_type;
} wifi_regdomain_t;

#endif
