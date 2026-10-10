/*
 * The modem's clocks and the PHY, as ESP-IDF's esp_phy_enable and modem_clock bring them up on the ESP32-C6,
 * through the frames over MODEM_SYSCON and MODEM_LPCON; register addresses and bits are ESP-IDF's soc/esp32c6.
 * The first enable calibrates fully, as ESP-IDF does with no calibration stored; later ones only wake the PHY.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lib/libc.h"
#include "osi.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

#define MODEM_SYSCON_CLK_CONF          (MODEM_SYSCON_BASE + 0x04u)
#define MODEM_SYSCON_CLK_CONF_POWER_ST (MODEM_SYSCON_BASE + 0x0Cu)
#define MODEM_SYSCON_RST_CONF          (MODEM_SYSCON_BASE + 0x10u)
#define MODEM_SYSCON_CLK_CONF1         (MODEM_SYSCON_BASE + 0x14u)
#define MODEM_SYSCON_WIFI_BB_CFG       (MODEM_SYSCON_BASE + 0x1Cu)
#define MODEM_LPCON_WIFI_LP_CLK_CONF   (MODEM_LPCON_BASE + 0x0Cu)
#define MODEM_LPCON_CLK_CONF           (MODEM_LPCON_BASE + 0x18u)
#define MODEM_LPCON_CLK_CONF_POWER_ST  (MODEM_LPCON_BASE + 0x20u)

/* MODEM_SYSCON_CLK_CONF1 */
#define CLK_WIFIBB_ALL     0x1ffu
#define CLK_WIFIMAC_EN     (1u << 9)
#define CLK_WIFI_APB_EN    (1u << 10)
#define CLK_FE_80M_EN      (1u << 13)
#define CLK_FE_160M_EN     (1u << 14)
#define CLK_FE_CAL_160M_EN (1u << 15)
#define CLK_FE_APB_EN      (1u << 16)
#define CLK_BT_APB_EN      (1u << 17)
#define CLK_BT_EN          (1u << 18)
/* MODEM_SYSCON_CLK_CONF */
#define CLK_MODEM_SEC_APB_EN (1u << 28)
/* MODEM_SYSCON_RST_CONF */
#define RST_WIFIMAC (1u << 10)
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
    uint8_t params[128];
} esp_phy_init_data_t;

typedef struct {
    uint8_t version[4];
    uint8_t mac[6];
    uint8_t opaque[1894];
} esp_phy_calibration_data_t;

int register_chipv7_phy(const esp_phy_init_data_t *init_data, esp_phy_calibration_data_t *cal_data, int mode);
void phy_bbpll_en_usb(bool enable);
void phy_wakeup_init(void);
void phy_close_rf(void);
void phy_xpd_tsens(void);
void phy_wifi_enable_set(uint8_t enable);
void phy_set_tsens_power(bool on);
void wait_i2c_sdm_stable(void);
int rtc_clk_xtal_freq_get(void);
void phy_get_xtal_freq(void);
void phy_xpd_rf(void);
void tsens_read_init_new(void);
void pwdet_reg_init_new(void);
void open_i2c_xpd_new(bool cycle);
void esp_phy_efuse_get_mac(uint8_t *mac);
void disable_agc(void);
void chip_v7_set_chan(uint8_t channel, uint8_t second);
extern const esp_phy_init_data_t phy_init_data;
extern uint8_t phy_param[];

/* The ROM's table of its PHY functions, which the ROM's own call through and libphy.a fills some slots of. */
typedef void (*romfunc)(void);
romfunc *phy_get_romfuncs(void);
#define ROMFUNC_PWDET_SAR2_INIT 6
#define ROMFUNC_I2C_WRITE_REG   22 /* rom_i2c_writeReg: block, host, register, value */

static esp_phy_calibration_data_t cal_data;
static int calibrated;
static uint32_t enabled;

/*
 * libphy.a's functions that also reach PCR, the PMU or the LP domain, which stay the kernel's:
 * build.mk beside this file weakens the library's in a copy, so these take their place.
 * Each does the library's work in the driver's frames and leaves out the rest,
 * which the kernel's board_init has set as the library would.
 */

/* The crystal's frequency, as libphy keeps it, in its phy_param's byte 0x51: 2 at 32 MHz, 1 at 26, 0 at 40. */
#define PHY_PARAM_XTAL 0x51

/* Registers of the front end's power detector, unnamed in any header, and of the SAR ADC. */
#define FE_PWDET_CONF0           (MODEM_BASE + 0x810u) /* the front end's block is the modem's first */
#define FE_PWDET_CONF1           (MODEM_BASE + 0x814u)
#define FE_PWDET_CONF2           (MODEM_BASE + 0x818u)
#define FE_PWDET_CONF3           (MODEM_BASE + 0x81Cu)
#define FE_PWDET_CONF4           (MODEM_BASE + 0x820u)
#define APB_SARADC_CTRL          (SARADC_BASE + 0x00u)
#define APB_SARADC2_PWDET_DRV    (1u << 29)
#define APB_SARADC_TSENS_CTRL2   (SARADC_BASE + 0x5Cu)
#define APB_SARADC_TSENS_CLK_SEL (1u << 15)

/* Leaves out PCR's tick count of the crystal, which the chip resets to 40 MHz's. */
void phy_get_xtal_freq(void)
{
    int mhz = rtc_clk_xtal_freq_get();
    phy_param[PHY_PARAM_XTAL] = mhz == 32 ? 2 : mhz == 26;
}

/* Leaves out the temperature sensor's clock and reset in PCR. */
void tsens_read_init_new(void)
{
    REG(APB_SARADC_TSENS_CTRL2) |= APB_SARADC_TSENS_CLK_SEL;
    phy_set_tsens_power(true);
}

/* Leaves out the SAR ADC's capacitance in LP_AON. */
void pwdet_reg_init_new(void)
{
    REG(FE_PWDET_CONF2) = 0x0F0F0FFFu;
    REG(FE_PWDET_CONF3) = 0x00FF0F64u;
    REG(FE_PWDET_CONF0) = (REG(FE_PWDET_CONF0) & ~0x00000FF0u) | 0x00000500u;
    REG(FE_PWDET_CONF4) = 0x0000AAAAu;
    REG(FE_PWDET_CONF0) = (REG(FE_PWDET_CONF0) & ~0x00700000u) | 0x00200000u;
    REG(FE_PWDET_CONF1) = (REG(FE_PWDET_CONF1) & ~0x000000FFu) | 0x00000008u;
    REG(APB_SARADC_CTRL) |= APB_SARADC2_PWDET_DRV;
}

/*
 * Leaves out powering the analog I2C buses in the PMU, off for 100 us first when cycle is set,
 * for which the kernel's powering them at boot stands; what is left is the wait for them to settle.
 */
void open_i2c_xpd_new(bool cycle)
{
    (void)cycle;
    wait_i2c_sdm_stable();
}

/*
 * Leaves out powering the RF's analog buses down in the PMU, PMU_RF_PWC's XPD_TXRF_I2C to XPD_PLL_I2C,
 * bits 28 to 31 at 0x600B0154, which the kernel's board_init powers up, so they stay on until the next boot;
 * see TODO.md.
 * What is left is the library's own order: the AGC off, bit 1 of the Wi-Fi baseband's configuration cleared,
 * a bit ESP-IDF's headers leave unnamed, and register 2 of the analog block 0x67 set to 6 through the ROM's table.
 */
void phy_xpd_rf(void)
{
    disable_agc();
    REG(MODEM_SYSCON_WIFI_BB_CFG) &= ~0x2u;
    ets_delay_us(1);
    ((void (*)(uint8_t, uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_REG])(0x67, 1, 2, 6);
}

/*
 * The ROM's rom_pwdet_sar2_init, which rom_en_pwdet calls through the table, without its last store:
 * the SAR ADC's capacitance again, at 0x600B1C54 in LP_WDT's page, where ESP-IDF's headers list no register.
 */
static void pwdet_sar2_init(void)
{
    REG(FE_PWDET_CONF1) |= 0x00003000u;
    REG(FE_PWDET_CONF1) &= ~0x00000200u;
    REG(FE_PWDET_CONF4) = 0x0000016Au;
}

/* The factory MAC address as the eFuse holds it, last byte first, which the root task read for the driver. */
void esp_phy_efuse_get_mac(uint8_t *mac)
{
    for (int i = 0; i < 6; i++) {
        mac[i] = drv_self->mac[5 - i];
    }
}

void drv_phy_clock_enable(void)
{
    REG(MODEM_SYSCON_CLK_CONF_POWER_ST) |= SYSCON_ICG_MAPS;
    REG(MODEM_LPCON_CLK_CONF_POWER_ST) |= LPCON_ICG_MAPS;
    uint32_t fe = CLK_FE_APB_EN | CLK_FE_80M_EN | CLK_FE_160M_EN | CLK_FE_CAL_160M_EN;
    REG(MODEM_SYSCON_CLK_CONF1) |= fe | CLK_WIFIBB_ALL | CLK_BT_EN | CLK_BT_APB_EN;
    REG(MODEM_SYSCON_CLK_CONF) |= CLK_MODEM_SEC_APB_EN;
    REG(MODEM_LPCON_CLK_CONF) |= CLK_I2C_MST_EN;
}

/* The PHY alone on for one more user, as esp_phy_enable turns it on for any modem, 802.15.4's among them. */
void drv_phy_on(void)
{
    if (enabled++ == 0) {
        drv_phy_clock_enable();
        if (!calibrated) {
            phy_get_romfuncs()[ROMFUNC_PWDET_SAR2_INIT] = pwdet_sar2_init;
            memcpy(cal_data.mac, drv_self->mac, 6);
            phy_bbpll_en_usb(true);
            int ret = register_chipv7_phy(&phy_init_data, &cal_data, PHY_RF_CAL_FULL);
            drv_say("phy: calibrated, register_chipv7_phy returned %d\n", ret);
            calibrated = 1;
        } else {
            phy_wakeup_init();
        }
    }
}

/*
 * Once nothing uses the PHY, the RF and the temperature sensor are turned off, as esp_phy_disable does,
 * but for the RF's power in the PMU, which phy_xpd_rf leaves on.
 */
void drv_phy_off(void)
{
    if (enabled > 0 && --enabled == 0) {
        phy_close_rf();
        phy_xpd_tsens();
    }
}

/* For Wi-Fi, as ESP-IDF's adapter enables it: the PHY, then Wi-Fi's part of it. */
void drv_phy_enable(void)
{
    osi_trace("phy_enable", 0, 0);
    drv_phy_on();
    phy_wifi_enable_set(1);
}

void drv_phy_disable(void)
{
    osi_trace("phy_disable", 0, 0);
    phy_wifi_enable_set(0);
    drv_phy_off();
}

/* The radio on channel, 20 MHz wide, as libphy's set_chanfreq tunes it for the libraries. */
void drv_phy_channel(uint32_t channel)
{
    chip_v7_set_chan((uint8_t)channel, 0);
}

void drv_wifi_clock_enable(void)
{
    osi_trace("wifi_clock_enable", 0, 0);
    REG(MODEM_SYSCON_CLK_CONF1) |= CLK_WIFIMAC_EN | CLK_WIFI_APB_EN | CLK_WIFIBB_ALL;
    /*
     * The MAC's timers and power block, at 0x600AD000, run on a clock of their own,
     * which ESP-IDF's startup selects and enables before anything of Wi-Fi: RC_SLOW, as its slow clock is.
     * Without it the MAC's microsecond counter stands still,
     * and the PHY's calibration, which waits on it, waits for ever.
     */
    REG(MODEM_LPCON_WIFI_LP_CLK_CONF) = CLK_WIFIPWR_LP_SEL_OSC_SLOW;
    REG(MODEM_LPCON_CLK_CONF) |= CLK_WIFIPWR_EN | CLK_COEX_EN;
}

void drv_wifi_reset_mac(void)
{
    osi_trace("wifi_reset_mac", 0, 0);
    REG(MODEM_SYSCON_RST_CONF) |= RST_WIFIMAC;
    REG(MODEM_SYSCON_RST_CONF) &= ~RST_WIFIMAC;
}
