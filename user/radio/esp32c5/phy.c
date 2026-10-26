/*
 * The ESP32-C5's PHY, the drv_phy_ functions of osi.h that the radio's MAC and programs call,
 * for now over Espressif's libphy.a, brought up as ESP-IDF's esp_phy_enable brings it up on this chip;
 * see README.md, "The way there".
 *
 * The library also reaches registers of PCR, the PMU and LP_AON, which no program maps:
 * the kernel sets them at boot, and the adapter feigns the library's accesses to them, see the board's radio.h.
 * Its calibration data is not kept between runs, so every bring-up calibrates in full.
 *
 * The modem's clocks are ESP-IDF's modem_clock_module_enable's for the PHY and for Wi-Fi on this chip,
 * whose PHY calibrates on Wi-Fi's baseband clocks, SOC_PHY_CALIBRATION_CLOCK_IS_INDEPENDENT;
 * the register addresses and bits are those of ESP-IDF's soc/esp32c5/include/modem/.
 */

#include <stdbool.h>
#include <stdint.h>

#include "drv.h"
#include "osi.h"

#define REG(a) (*(volatile uint32_t *)(a))

#define MODEM_SYSCON_BASE              0x600A9C00u
#define MODEM_SYSCON_CLK_CONF_POWER_ST (MODEM_SYSCON_BASE + 0x0Cu)
#define MODEM_SYSCON_MODEM_RST_CONF    (MODEM_SYSCON_BASE + 0x10u)
#define RST_WIFIMAC                    (1u << 9)
#define MODEM_SYSCON_CLK_CONF1         (MODEM_SYSCON_BASE + 0x14u)
#define MODEM_LPCON_BASE               0x600AF000u
#define MODEM_LPCON_WIFI_LP_CLK_CONF   (MODEM_LPCON_BASE + 0x0Cu)
#define MODEM_LPCON_CLK_CONF           (MODEM_LPCON_BASE + 0x18u)
#define MODEM_LPCON_CLK_CONF_POWER_ST  (MODEM_LPCON_BASE + 0x20u)

/* MODEM_SYSCON_CLK_CONF1 */
#define CLK_WIFIBB_ALL     0x1FBu     /* 22M, 40M and 80M to 160X1, modem_syscon_ll_clk_wifibb_configure's */
#define CLK_WIFIBB_44M_EN  (1u << 2)
#define CLK_WIFIMAC_EN     (1u << 9)
#define CLK_WIFI_APB_EN    (1u << 10)
#define CLK_FE_80M_EN      (1u << 13)
#define CLK_FE_160M_EN     (1u << 14)
#define CLK_FE_APB_EN      (1u << 15)
#define CLK_FE_PWDET_EN    (1u << 19)
#define CLK_FE_ADC_EN      (1u << 20)
#define CLK_FE_DAC_EN      (1u << 21)
/* MODEM_LPCON_WIFI_LP_CLK_CONF: the source of the Wi-Fi power block's slow clock, RC_SLOW, with no divider */
#define CLK_WIFIPWR_LP_SEL_OSC_SLOW (1u << 0)
/* MODEM_LPCON_CLK_CONF */
#define CLK_WIFIPWR_EN (1u << 0)
#define CLK_COEX_EN    (1u << 1)
#define CLK_I2C_MST_EN (1u << 2)

/* The clock domains' gating maps, as modem_clock_domain_icg_config sets them: no gating while the PMU is active. */
#define SYSCON_ICG_MAPS 0x64646400u
#define LPCON_ICG_MAPS  0x66660000u

#define PHY_RF_CAL_FULL 2

typedef struct {
    uint8_t params[256];
} esp_phy_init_data_t;

typedef struct {
    uint8_t version[4];
    uint8_t mac[6];
    uint8_t opaque[1894];
} esp_phy_calibration_data_t;

/* libphy.a's, as ESP-IDF's esp_phy calls them; phy_change_channel takes the channel's centre in MHz. */
int register_chipv7_phy(const esp_phy_init_data_t *init_data, esp_phy_calibration_data_t *cal_data, int mode);
void phy_wakeup_init(void);
void phy_close_rf(void);
void phy_xpd_tsens(void);
void phy_wifi_enable_set(uint8_t enable);
int phy_change_channel(uint32_t mhz, uint32_t a1, uint32_t a2, uint32_t width);
void phy_disable_low_rate(void);
void phy_param_track_tot(bool en_wifi, bool en_ble_154);
void set_bb_wdg(uint32_t enable, uint32_t a1, uint32_t busy, uint32_t idle);
char *get_phy_version_str(void);

extern const esp_phy_init_data_t phy_init_data; /* tools/esp-fetch.py's, from ESP-IDF's phy_init_data.c */

static esp_phy_calibration_data_t cal_data;
static uint32_t enabled;
static int calibrated;

void drv_phy_clock_enable(void)
{
    REG(MODEM_SYSCON_CLK_CONF_POWER_ST) |= SYSCON_ICG_MAPS;
    REG(MODEM_LPCON_CLK_CONF_POWER_ST) |= LPCON_ICG_MAPS;
    uint32_t fe = CLK_FE_APB_EN | CLK_FE_80M_EN | CLK_FE_160M_EN | CLK_FE_DAC_EN | CLK_FE_PWDET_EN | CLK_FE_ADC_EN;
    REG(MODEM_SYSCON_CLK_CONF1) |= fe | CLK_WIFI_APB_EN | CLK_WIFIBB_ALL | CLK_WIFIBB_44M_EN;
    REG(MODEM_LPCON_CLK_CONF) |= CLK_I2C_MST_EN;
}

void drv_wifi_clock_enable(void)
{
    REG(MODEM_SYSCON_CLK_CONF1) |= CLK_WIFIMAC_EN | CLK_WIFI_APB_EN | CLK_WIFIBB_ALL;
    /* The MAC's timers and power block run on a slow clock of their own, RC_SLOW, as on the ESP32-C6. */
    REG(MODEM_LPCON_WIFI_LP_CLK_CONF) = CLK_WIFIPWR_LP_SEL_OSC_SLOW;
    REG(MODEM_LPCON_CLK_CONF) |= CLK_WIFIPWR_EN | CLK_COEX_EN;
}

void drv_phy_on(void)
{
    if (enabled++ == 0) {
        drv_phy_clock_enable();
        if (!calibrated) {
            drv_say("phy: %s\n", get_phy_version_str());
            for (int i = 0; i < 6; i++) {
                cal_data.mac[i] = drv_self->mac[i];
            }
            int ret = register_chipv7_phy(&phy_init_data, &cal_data, PHY_RF_CAL_FULL);
            drv_say("phy: calibrated, register_chipv7_phy returned %d\n", ret);
            calibrated = 1;
        } else {
            phy_wakeup_init();
        }
    }
}

void drv_phy_off(void)
{
    if (enabled != 0 && --enabled == 0) {
        phy_close_rf();
        phy_xpd_tsens();
    }
}

void drv_phy_enable(void)
{
    drv_phy_on();
    phy_wifi_enable_set(1);
}

void drv_phy_disable(void)
{
    phy_wifi_enable_set(0);
    drv_phy_off();
}

/* A channel's centre in MHz: 2.4 GHz's 1 to 14, and 5 GHz's from 36 up. */
uint32_t drv_phy_channel_mhz(uint32_t channel)
{
    if (channel == 14) {
        return 2484;
    }
    return channel < 14 ? 2407 + 5 * channel : 5000 + 5 * channel;
}

void drv_phy_channel(uint32_t channel)
{
    phy_change_channel(drv_phy_channel_mhz(channel), 1, 0, 0);
}

/* The MAC's reset pulsed, as the libraries' wifi_reset_mac does through the adapter. */
void drv_wifi_reset_mac(void)
{
    REG(MODEM_SYSCON_MODEM_RST_CONF) |= RST_WIFIMAC;
    REG(MODEM_SYSCON_MODEM_RST_CONF) &= ~RST_WIFIMAC;
}

/* The low rates' receiving off, the PHY's half of the MAC's low-rate group: libphy.a's. */
void drv_phy_low_rate_disable(void)
{
    phy_disable_low_rate();
}

/* The PHY kept to the temperature, as ESP-IDF's phy_track_pll does once a second: libphy.a's tracking. */
void drv_phy_track(void)
{
    phy_param_track_tot(true, false);
}

/* The baseband's watchdog as the station's start sets it: its busy check on, 0x18, its idle check off, 0xaa. */
void drv_phy_busy_check(void)
{
    set_bb_wdg(1, 0, 0x18, 0xaa);
}
