/*
 * The harness the PHY tracer runs: what ESP-IDF does on the ESP32-C6 from esp_phy_enable
 * up to and including register_chipv7_phy, with a full calibration, and nothing else.
 * Every register access, this file's and those of libphy.a and the ROM, faults to the tracer; see phytrace.h.
 * This file's accesses stand for ESP-IDF's open code, not for the library.
 */

#include <stdbool.h>
#include <stdint.h>

#include "phytrace.h"

/* ESP-IDF's components/esp_phy/include/esp_phy_init.h. */
typedef struct {
    uint8_t params[128];
} esp_phy_init_data_t;

typedef struct {
    uint8_t version[4];
    uint8_t mac[6];
    uint8_t opaque[1894];
} esp_phy_calibration_data_t;

#define PHY_RF_CAL_FULL 2

/* libphy.a. */
int register_chipv7_phy(const esp_phy_init_data_t *init_data, esp_phy_calibration_data_t *cal_data, int mode);
void phy_bbpll_en_usb(bool enable);

/* ESP-IDF's default init data, generated from its phy_init_data.c by tools/phy-init-data.py. */
extern const esp_phy_init_data_t phy_init_data;

void phyblob_main(void);
void *memcpy(void *dst, const void *src, unsigned int n);
int rtc_clk_xtal_freq_get(void);

/*
 * Registers, from ESP-IDF's soc/esp32c6 headers.
 * The modem's clock bits are the ones modem_clock_impl.c enables for PERIPH_PHY_MODULE
 * and PERIPH_PHY_CALIBRATION_MODULE; esp_phy_enable asserts CLK_CONF1 holds 0x7e1ff after them.
 */
#define MODEM_SYSCON_CLK_CONF          0x600a9804u
#define MODEM_SYSCON_CLK_CONF_POWER_ST 0x600a980cu
#define MODEM_SYSCON_CLK_CONF1         0x600a9814u
#define MODEM_LPCON_CLK_CONF           0x600af018u
#define MODEM_LPCON_CLK_CONF_POWER_ST  0x600af020u
#define EFUSE_RD_MAC_SPI_SYS_0         0x600b0844u
#define EFUSE_RD_MAC_SPI_SYS_1         0x600b0848u

#define CLK_MODEM_SEC_APB_EN (1u << 28)
#define CLK_WIFIBB_ALL       0x1ffu
#define CLK_FE_80M_EN        (1u << 13)
#define CLK_FE_160M_EN       (1u << 14)
#define CLK_FE_CAL_160M_EN   (1u << 15)
#define CLK_FE_APB_EN        (1u << 16)
#define CLK_BT_APB_EN        (1u << 17)
#define CLK_BT_EN            (1u << 18)
#define CLK_I2C_MST_EN       (1u << 2)

/*
 * The clock domains' gating maps, as modem_clock_domain_icg_config sets them:
 * no gating while the PMU is active (bit 2), and in modem state (bit 1) where ESP-IDF asks for it,
 * four bits a domain.
 */
#define SYSCON_ICG_MAPS 0x64646400u /* modem APB, peripherals, Wi-Fi, BT, FE, 802.15.4 */
#define LPCON_ICG_MAPS  0x66660000u /* LP APB, I2C master, coexistence, Wi-Fi power */

static esp_phy_calibration_data_t cal_data;

static void request(uint32_t what, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    register uint32_t t0 __asm__("t0") = what;
    register uint32_t a0 __asm__("a0") = arg0;
    register uint32_t a1 __asm__("a1") = arg1;
    register uint32_t a2 __asm__("a2") = arg2;
    __asm__ volatile("ebreak" : : "r"(t0), "r"(a0), "r"(a1), "r"(a2) : "memory");
}

static void mark(const char *phase)
{
    request(PHYBLOB_MARK, (uint32_t)phase, 0, 0);
}

static uint32_t reg_read(uint32_t reg)
{
    return *(volatile uint32_t *)reg;
}

static void reg_set(uint32_t reg, uint32_t bits)
{
    *(volatile uint32_t *)reg = reg_read(reg) | bits;
}

void phyblob_main(void)
{
    mark("modem clock gating maps, as modem_clock_module_icg_map_init_all");
    reg_set(MODEM_SYSCON_CLK_CONF_POWER_ST, SYSCON_ICG_MAPS);
    reg_set(MODEM_LPCON_CLK_CONF_POWER_ST, LPCON_ICG_MAPS);

    mark("PERIPH_PHY_MODULE clocks, as esp_phy_common_clock_enable");
    reg_set(MODEM_SYSCON_CLK_CONF1, CLK_FE_APB_EN | CLK_FE_80M_EN);
    reg_set(MODEM_SYSCON_CLK_CONF1, CLK_FE_CAL_160M_EN | CLK_FE_160M_EN);
    reg_set(MODEM_LPCON_CLK_CONF, CLK_I2C_MST_EN);

    mark("PERIPH_PHY_CALIBRATION_MODULE clocks, as phy_module_enable");
    reg_set(MODEM_SYSCON_CLK_CONF1, CLK_WIFIBB_ALL);
    reg_set(MODEM_SYSCON_CLK_CONF1, CLK_BT_EN);
    reg_set(MODEM_SYSCON_CLK_CONF1, CLK_BT_APB_EN);
    reg_set(MODEM_SYSCON_CLK_CONF, CLK_MODEM_SEC_APB_EN);

    /* esp_efuse_mac_get_default: the factory MAC, whose bytes the eFuse holds last first. */
    mark("the factory MAC from eFuse, as esp_efuse_mac_get_default");
    uint32_t mac0 = reg_read(EFUSE_RD_MAC_SPI_SYS_0);
    uint32_t mac1 = reg_read(EFUSE_RD_MAC_SPI_SYS_1);
    uint8_t fused[6] = {
        (uint8_t)mac0, (uint8_t)(mac0 >> 8), (uint8_t)(mac0 >> 16), (uint8_t)(mac0 >> 24),
        (uint8_t)mac1, (uint8_t)(mac1 >> 8),
    };
    for (unsigned i = 0; i < 6; i++) {
        cal_data.mac[i] = fused[5 - i];
    }

    /* CONFIG_ESP_PHY_ENABLE_USB, which ESP-IDF sets when the console is on USB, as the tracer's is. */
    mark("phy_bbpll_en_usb(true)");
    phy_bbpll_en_usb(true);

    mark("register_chipv7_phy(init_data, cal_data, PHY_RF_CAL_FULL)");
    int ret = register_chipv7_phy(&phy_init_data, &cal_data, PHY_RF_CAL_FULL);

    mark("done");
    request(PHYBLOB_PRINTF, (uint32_t)"register_chipv7_phy returned %d\n", (uint32_t)ret, 0);
    request(PHYBLOB_DUMP, (uint32_t)&cal_data, sizeof(cal_data), 0);
}

void *memcpy(void *dst, const void *src, unsigned int n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n-- != 0) {
        *d++ = *s++;
    }
    return dst;
}

/* RTC_XTAL_FREQ_40M: the ESP32-C6's crystal is 40 MHz. */
int rtc_clk_xtal_freq_get(void)
{
    return 40;
}
