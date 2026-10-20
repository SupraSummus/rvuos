/*
 * The modem's clocks and the PHY, as ESP-IDF's esp_phy_enable and modem_clock bring them up on the ESP32-C6,
 * through the frames over MODEM_SYSCON and MODEM_LPCON; register addresses and bits are ESP-IDF's soc/esp32c6.
 * The first enable calibrates fully, as ESP-IDF does with no calibration stored; later ones only wake the PHY.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lib/libc.h"
#include "osi.h"
#include "prof.h"

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
void phy_wakeup_init(void);
void phy_close_rf(void);
void phy_xpd_tsens(void);
void phy_set_tsens_power(bool on);
void phy_wifi_enable_set(uint8_t enable);
int phy_get_max_pwr(uint32_t index, uint8_t out[2]);
void wait_i2c_sdm_stable(void);
int rtc_clk_xtal_freq_get(void);
void phy_get_xtal_freq(void);
void phy_xpd_rf(void);
void tsens_read_init_new(void);
void pwdet_reg_init_new(void);
void open_i2c_xpd_new(bool cycle);
void esp_phy_efuse_get_mac(uint8_t *mac);
void disable_agc(void);
void enable_agc(void);
void chip_v7_set_chan(uint32_t chan, uint32_t second);
void phy_change_channel(uint32_t mhz, uint32_t a1, uint32_t a2, uint32_t width);
uint32_t mhz2ieee(uint32_t mhz);
uint32_t chan_to_freq(uint32_t chan);
void set_channel_rfpll_freq(uint32_t freq, uint32_t xtal, int32_t param);
void set_chan_reg(uint32_t on);
void set_rx_comp_new(void);
void write_gain_mem_new(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void phy_i2c_master_mem_txcap(void);
void set_freq_i2c(uint32_t freq);
void wifi_get_tx_tab(uint32_t chan, uint8_t *out8, uint16_t *out9, uint16_t *out10, uint32_t power);
void wifi_set_tx_gain(uint32_t chan, uint32_t power);
void phy_set_most_tpw(uint8_t tpw);
void chan14_mic_cfg(uint32_t on);
void wr_rx_gain_mem(uint32_t p1, uint32_t p2, const uint32_t *p3, const uint16_t *p4, const uint16_t *p5,
                    const uint16_t *p6, uint32_t p7, uint32_t p8);
/* The leaves of libphy's phy_wakeup_init the driver's own, taken over with its disassembly. */
void i2c_clk_sel(uint32_t sel);
void i2c_bbpll_set(int on);
void reg_set_new(void);
void freq_reg_init(uint32_t param1, uint32_t param2);
void bias_reg_set(int on);
void noise_floor_auto_set_new(void);
void bt_filter_reg_new(void);
void pbus_clear_reg(void);
void phy_i2c_init2(void);
void filter_dcap_set(void);
void freq_get_i2c_data(uint8_t *b0, uint8_t *b1, uint8_t *b2, uint32_t *b3, uint32_t count);
void freq_i2c_write_set_new(const uint8_t *p0, const uint8_t *p1, const uint8_t *p2, const uint32_t *p3,
                            uint32_t count);
void freq_i2c_mem_write_new(uint32_t addr, uint32_t data, uint32_t len);
void freq_i2c_data_write_new(void);
void rx_11b_opt(uint32_t on);
void tx_paon_set_new(void);
void agc_reg_init_new(uint32_t param1, uint32_t param2);
void bb_reg_init_new(void);
void btbb_wifi_bb_cfg2(void);
void phy_reg_init(void);
/* The ROM's own, which this file calls as the library does when taking its functions over. */
void phy_bbpll_cal(uint32_t enable);
void freq_module_resetn(void);
uint32_t tsens_code_read(void);
int code_to_temp(uint32_t code, int offset);
extern const esp_phy_init_data_t phy_init_data;
extern uint8_t phy_param[];

/* The ROM's table of its PHY functions, which the ROM's own call through and libphy.a fills some slots of. */
typedef void (*romfunc)(void);
romfunc *phy_get_romfuncs(void);
#define ROMFUNC_PWDET_SAR2_INIT 6
#define ROMFUNC_SET_CHAN_CAL    2  /* rom_set_chan_cal_interp: a calibration table and a channel, answers its value */
#define ROMFUNC_PBUS_XPD_RX_ON  33 /* rom_pbus_xpd_rx_on: powering the receiver in the front end */
#define ROMFUNC_I2C_ENTER_CRITICAL 13 /* rom_i2c_enter_critical */
#define ROMFUNC_I2C_EXIT_CRITICAL  14 /* rom_i2c_exit_critical */
#define ROMFUNC_I2C_MST0_MASK      16 /* rom_get_i2c_mst0_mask: the analog I2C master's mask bits */
#define ROMFUNC_I2C_READ_REG    20 /* rom_i2c_readReg: block, host, register, answers the byte */
#define ROMFUNC_I2C_WRITE_REG   22 /* rom_i2c_writeReg: block, host, register, value */
#define ROMFUNC_I2C_WRITE_MASK  24 /* rom_i2c_writeReg_Mask: block, host, register, msb, lsb, value */
#define ROMFUNC_PBUS_FORCE_TEST 29 /* rom_pbus_force_test: a window, its channel and a value */

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

/* The library's own flag for its phy_xpd_tsens: clear until it has run once. */
#define PHY_PARAM_TSENS_XPD 0x19D

/* The temperature the sensor's read keeps, a halfword at phy_param's start. */
#define PHY_PARAM_TEMPERATURE 0x00
/* The temperature sensor's DAC index, which the read keeps and the library's rf_init reads. */
#define PHY_PARAM_TSENS_INDEX 0x16
/* The library's mark that the RF is off: its wakeup clears it, phy_close_rf sets it, and its tracking skips while set. */
#define PHY_PARAM_RF_OFF 0x17
/* The library's mark that its bring-up has run, set at register_chipv7_phy's end; phy_close_rf's work waits on it. */
#define PHY_PARAM_PHY_UP 0x25
/* The library's mark whether Wi-Fi's baseband is up, under which its wakeup clears the baseband at its end. */
#define PHY_PARAM_WIFI_BB_ENABLE 0x19e

/* Registers of the front end's power detector, unnamed in any header, and of the SAR ADC. */
#define FE_PWDET_CONF0           (MODEM_BASE + 0x810u) /* the front end's block is the modem's first */
#define FE_PWDET_CONF1           (MODEM_BASE + 0x814u)
#define FE_PWDET_CONF2           (MODEM_BASE + 0x818u)
#define FE_PWDET_CONF3           (MODEM_BASE + 0x81Cu)
#define FE_PWDET_CONF4           (MODEM_BASE + 0x820u)
#define APB_SARADC_CTRL          (SARADC_BASE + 0x00u)
#define APB_SARADC2_PWDET_DRV    (1u << 29)
#define APB_SARADC_TSENS_CTRL    (SARADC_BASE + 0x58u)
#define APB_SARADC_TSENS_PU      (1u << 22)
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
 * Our own of libphy's wait_i2c_sdm_stable, which open_i2c_xpd_new below calls, taken from its disassembly:
 * read the analog I2C's block 0x63, host 1, register 0, until it answers 0x5b,
 * or until the MAC's microsecond counter has moved more than 0x270f.
 */
#define MAC_TSF_CLOCK      (MODEM_BASE + 0xd000u)
#define I2C_SDM_SETTLE_MAX 0x270fu
#define I2C_SDM_SETTLED    0x5bu

void wait_i2c_sdm_stable(void)
{
    uint32_t start = REG(MAC_TSF_CLOCK);

    for (;;) {
        if (REG(MAC_TSF_CLOCK) - start > I2C_SDM_SETTLE_MAX) {
            break;
        }
        if (((int (*)(uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_READ_REG])(0x63, 1, 0) ==
            I2C_SDM_SETTLED) {
            break;
        }
    }
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
            int ret = register_chipv7_phy(&phy_init_data, &cal_data, PHY_RF_CAL_FULL);
            drv_say("phy: calibrated, register_chipv7_phy returned %d\n", ret);
            calibrated = 1;
        } else {
            phy_wakeup_init();
        }
    }
}

/* The front end's registers of the frequency setting and of the forced TX/RX state, MODEM_BASE's FE block. */
#define FE_FREQ_SET_CONF    (MODEM_BASE + 0xC0u)
#define FE_FREQ_SET_STATUS  (MODEM_BASE + 0xCCu)
#define FE_FREQ_SET_ENABLE  0x04000000u /* the setting's enable, its bit 26 */
#define FE_FREQ_SET_BUSY    (1u << 8)   /* clear once the setting has settled */
#define FE_FREQ_SET_WAIT_US 0x3e8u      /* of the MAC's microsecond counter, the wait's bound */
#define FE_TXRX_OFF         (MODEM_BASE + 0x910u)
#define FE_TXRX_FORCE_MASK  0xfffff0ffu /* bits 8 to 11, the forcing's, which each step keeps off */

/* The ROM's rom_i2c_writeReg_Mask, block, host, register, msb, lsb, data, through its table's slot. */
static void rom_i2c_write_masked(uint8_t block, uint8_t host, uint8_t reg, uint8_t msb, uint8_t lsb, uint8_t data)
{
    ((void (*)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_MASK])(
        block, host, reg, msb, lsb, data);
}

/* The ROM's rom_i2c_writeReg, block, host, register, data, through its table's slot. */
static void rom_i2c_write(uint8_t block, uint8_t host, uint8_t reg, uint8_t data)
{
    ((void (*)(uint8_t, uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_REG])(block, host, reg, data);
}

/* The ROM's rom_i2c_readReg, block, host, register, through its table's slot, answering the byte. */
static uint8_t rom_i2c_read(uint8_t block, uint8_t host, uint8_t reg)
{
    return (uint8_t)((int (*)(uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_READ_REG])(block, host, reg);
}

/* libphy's force_txrx_off: the front end's TX and RX forced off, its bits 8 to 11, in two steps with a 1 us wait. */
static void force_txrx_off(uint32_t on)
{
    uint32_t v = REG(FE_TXRX_OFF) & FE_TXRX_FORCE_MASK;

    if (on != 0u) {
        REG(FE_TXRX_OFF) = v | 0x800u;
        ets_delay_us(1);
        REG(FE_TXRX_OFF) = (REG(FE_TXRX_OFF) & FE_TXRX_FORCE_MASK) | 0xa00u;
    } else {
        REG(FE_TXRX_OFF) = v | 0x200u;
        ets_delay_us(1);
        REG(FE_TXRX_OFF) = REG(FE_TXRX_OFF) & FE_TXRX_FORCE_MASK;
    }
    ets_delay_us(1);
}

/* libphy's phy_dis_hw_set_freq_reg: the frequency setting's enable, then a 2 us wait. */
static void phy_dis_hw_set_freq_reg(void)
{
    REG(FE_FREQ_SET_CONF) |= FE_FREQ_SET_ENABLE;
    ets_delay_us(2);
}

/*
 * libphy's phy_wait_freq_set_busy: wait, up to 0x3e8 of the MAC's microseconds, for the frequency setting to settle;
 * past that the ROM resets the frequency module, as the library does, whose print of the timeout is left out.
 */
static uint32_t phy_wait_freq_set_busy(void)
{
    uint32_t start = REG(MAC_TSF_CLOCK);
    uint32_t waited = 0;

    while ((REG(FE_FREQ_SET_STATUS) & FE_FREQ_SET_BUSY) == 0u) {
        waited = 1;
        if (REG(MAC_TSF_CLOCK) - start > FE_FREQ_SET_WAIT_US) {
            freq_module_resetn();
            break;
        }
    }
    return waited;
}

/* libphy's phy_dis_hw_set_freq_1: the wait for the frequency setting, then its enable. */
static void phy_dis_hw_set_freq_1(void)
{
    phy_wait_freq_set_busy();
    phy_dis_hw_set_freq_reg();
}

/* libphy's bias_reg_reset: three masked writes to the analog bias block 0x6a, host 1. */
static void bias_reg_reset(void)
{
    rom_i2c_write_masked(0x6a, 1, 0, 3, 0, 7);
    rom_i2c_write_masked(0x6a, 1, 1, 3, 0, 7);
    rom_i2c_write_masked(0x6a, 1, 0, 7, 4, 7);
}

/*
 * libphy's tsens_dac_to_index: the sensor's DAC code names one of the five entries its calibration writes,
 * and a code the library does not know names none.
 */
static uint32_t tsens_dac_to_index(uint32_t dac)
{
    switch (dac) {
    case 5u:
        return 0u;
    case 7u:
        return 1u;
    case 0xfu:
        return 2u;
    case 0xbu:
        return 3u;
    case 0xau:
        return 4u;
    default:
        return 5u;
    }
}

/* libphy's phy_tsens_attribute, its rodata at 0x4200ad8c: five entries of a DAC code and a temperature range. */
static const struct {
    int8_t offset; /* the offset the ROM's code_to_temp takes for the entry: its bytes 0xfe and 0xff are -2 and -1 */
    uint8_t dac;   /* the DAC code kept while the temperature lies in the entry's range */
    int16_t low;
    int16_t high;
} tsens_attribute[5] = {
    { -2, 0x05, 50, 125 },
    { -1, 0x07, 20, 100 },
    { 0, 0x0f, -10, 80 },
    { 1, 0x0b, -30, 50 },
    { 2, 0x0a, -40, 20 },
};

/*
 * libphy's tsens_dac_cal_new: a temperature in its entry's range keeps that entry's DAC code, and one outside
 * takes a corrected code, which the sensor's block 0x69, host 0, register 6 takes.
 * A DAC code the library does not know, one its calibration never writes, names no entry of the library's five;
 * the library's code reads a sixth past them then, which corrects the DAC too, so this corrects it as well.
 */
static uint32_t tsens_dac_cal_new(int temp, uint32_t index)
{
    uint32_t dac;

    if (index < 5u && temp >= tsens_attribute[index].low && temp <= tsens_attribute[index].high) {
        return tsens_attribute[index].dac;
    }
    if (temp > 99) {
        dac = 5u;
    } else if (temp > 79) {
        dac = 7u;
    } else if (temp >= -9) {
        dac = 0xfu;
    } else if (temp < -29) {
        dac = 0xau;
    } else {
        dac = 0xbu;
    }
    rom_i2c_write_masked(0x69, 0, 6, 3, 0, (uint8_t)dac);
    return dac;
}

/*
 * libphy's tsens_temp_read_new, with its phy_get_tsens_value and ram_tsens_temp_read_new folded in:
 * the sensor's block 0x69, host 0, register 6 read through the ROM's table for its DAC code,
 * that code's index kept in phy_param, the sensor's code turned to a temperature by the ROM's code_to_temp,
 * the DAC corrected where the temperature lies outside its entry's range, and the temperature kept in phy_param.
 */
static void tsens_temp_read_new(void)
{
    uint32_t dac =
        (uint32_t)((int (*)(uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_READ_REG])(0x69, 0, 6) & 0xfu;
    uint32_t index = tsens_dac_to_index(dac);
    uint32_t code;
    int temp;

    phy_param[PHY_PARAM_TSENS_INDEX] = (uint8_t)index;
    code = tsens_code_read();
    temp = code_to_temp(code, index < 5u ? tsens_attribute[index].offset : 0);
    tsens_dac_cal_new(temp, index);
    phy_param[PHY_PARAM_TEMPERATURE] = (uint8_t)temp;
    phy_param[PHY_PARAM_TEMPERATURE + 1u] = (uint8_t)((uint32_t)temp >> 8);
}

/*
 * Our own phy_close_rf, this step of taking the PHY from libphy.a: the library's phy_close_rf_ [0x42003892],
 * the one drv_phy_off calls.
 * While the temperature sensor still has its power it reads the temperature,
 * then, only once the library's bring-up has marked the PHY up [0x42004070], it brings the frequency setting down,
 * forces the TX and RX off, takes the RF down through phy_xpd_rf above, calibrates the RF PLL with the ROM,
 * resets the analog bias, and marks the RF off.
 * The library runs this inside the ROM's critical section, the pair of empty returns [0x40002e1e, 0x40002e22],
 * so this leaves that out. build.mk weakens the library's phy_close_rf, so this takes its place; nothing in the
 * library calls it, and the library's own phy_xpd_rf is already this file's.
 */
void phy_close_rf(void)
{
    if (phy_param[PHY_PARAM_TSENS_XPD] == 0) {
        tsens_temp_read_new();
    }
    if (phy_param[PHY_PARAM_PHY_UP] == 0) {
        return;
    }
    phy_dis_hw_set_freq_1();
    force_txrx_off(1);
    phy_xpd_rf();
    phy_bbpll_cal(1);
    bias_reg_reset();
    phy_param[PHY_PARAM_RF_OFF] = 1;
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

/*
 * Our own phy_wifi_enable_set, this step of taking the PHY from libphy.a:
 * bit 1 of MODEM_SYSCON_WIFI_BB_CFG, the one bit the library's sets and clears in one read-modify-write --
 * lui 0x600aa; lw -0x7e4(a4); ori 2 / andi ~2; sw -- with no other effect,
 * so the two are equal by their disassembly alone.
 * build.mk weakens the library's, so this takes its place, as the library's own wakeup path calls it too.
 */
void phy_wifi_enable_set(uint8_t enable)
{
    if (enable) {
        REG(MODEM_SYSCON_WIFI_BB_CFG) |= 0x2u;
    } else {
        REG(MODEM_SYSCON_WIFI_BB_CFG) &= ~0x2u;
    }
}

/*
 * Our own phy_set_tsens_power, the same step of the library's:
 * bit 22, APB_SARADC_TSENS_PU, of APB_SARADC_APB_TSENS_CTRL_REG, 0x6000E058,
 * which the library's sets from its argument's low bit in one read-modify-write and touches nothing else.
 */
void phy_set_tsens_power(bool on)
{
    uint32_t v = REG(APB_SARADC_TSENS_CTRL);
    REG(APB_SARADC_TSENS_CTRL) = (v & ~APB_SARADC_TSENS_PU) | (on ? APB_SARADC_TSENS_PU : 0u);
}

/*
 * Our own phy_xpd_tsens, as the library's: while its flag in phy_param is clear, phy_set_tsens_power(0),
 * then set the flag, so the power comes off once and later calls only keep the flag.
 */
void phy_xpd_tsens(void)
{
    if (phy_param[PHY_PARAM_TSENS_XPD] == 0) {
        phy_set_tsens_power(false);
    }
    phy_param[PHY_PARAM_TSENS_XPD] = 1;
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

/*
 * The PHY kept to the chip's temperature, as ESP-IDF's phy_track_pll_timer_callback keeps it once a second while
 * Wi-Fi's modem is on: libphy's phy_param_track_tot, Wi-Fi's part alone, which retunes the RF PLL's capacitance
 * and corrects Wi-Fi's power and calibrations as the temperature has moved since it last ran.
 * ESP-IDF takes its PHY lock around it, which its enable and disable take; here the station's thread alone enables
 * and tunes, and calls this, so mac.c keeps it from a frame on the air alone.
 */
void phy_param_track_tot(bool en_wifi, bool en_ble_154);

void drv_phy_track(void)
{
    uint32_t c = prof_cycles();
    phy_param_track_tot(true, false);
    prof_add(&drv_prof.phy_track, prof_cycles() - c);
}

/* The radio on channel, 20 MHz wide, as libphy's set_chanfreq tunes it for the libraries. */
void drv_phy_channel(uint32_t channel)
{
    chip_v7_set_chan(channel, 0);
}

/*
 * The leaves of libphy's chip_v7_set_chan the driver's own, this step; each is held to the library's with
 * phy-ab.py, which runs both from one state in an emulator, and build.mk then weakens the library's in its copy.
 */

/* libphy's set_rx_comp_new [0x42004bdc]: two read-modify-writes of the baseband's receive compensation. */
#define WIFI_BB_RX_COMP0 (MODEM_BASE + 0x702Cu)
#define WIFI_BB_RX_COMP1 (MODEM_BASE + 0x70A0u)

void set_rx_comp_new(void)
{
    REG(WIFI_BB_RX_COMP0) = (REG(WIFI_BB_RX_COMP0) & ~0xffu) | 0xe9u;
    REG(WIFI_BB_RX_COMP1) = (REG(WIFI_BB_RX_COMP1) & 0x00ffffffu) | 0xe9000000u;
}

/* libphy's write_gain_mem_new [0x42005004]: three words of the front end's gain memory, then its fourth. */
#define FE_GAIN_MEM0     (MODEM_BASE + 0x8CCu)
#define FE_GAIN_MEM1     (MODEM_BASE + 0x8D0u)
#define FE_GAIN_MEM2     (MODEM_BASE + 0x8D4u)
#define FE_GAIN_MEM_CONF (MODEM_BASE + 0x8C8u)

void write_gain_mem_new(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    REG(FE_GAIN_MEM0) = a;
    REG(FE_GAIN_MEM1) = b;
    REG(FE_GAIN_MEM2) = c;
    REG(FE_GAIN_MEM_CONF) = (REG(FE_GAIN_MEM_CONF) & 0xfff00000u) | ((d << 11) + 0x80000u);
}

/*
 * libphy's phy_i2c_master_mem_txcap [0x42002e5e]:
 * the analog I2C's block 0x6b, host 1, register 2 read through the ROM's table, its byte above the constant.
 */
#define I2C_ANA_MST_TXCAP (MODEM_BASE + 0xFC04u)

void phy_i2c_master_mem_txcap(void)
{
    uint32_t v =
        (uint32_t)((int (*)(uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_READ_REG])(0x6b, 1, 2);

    REG(I2C_ANA_MST_TXCAP) = (v << 16) | 0x26bu;
}

/*
 * libphy's set_freq_i2c [0x42003094]: the temperature read again, then the frequency and the temperature
 * choosing the low four bits of the analog block 0x6b, host 1, register 7.
 */
void set_freq_i2c(uint32_t freq)
{
    int16_t temp;
    uint32_t data;

    tsens_temp_read_new();
    temp = (int16_t)((uint16_t)phy_param[PHY_PARAM_TEMPERATURE] |
                     ((uint16_t)phy_param[PHY_PARAM_TEMPERATURE + 1u] << 8));
    if (temp > 54) {
        data = 7u;
    } else if (temp < 0) {
        data = 5u;
    } else {
        data = 8u;
    }
    if (freq > 0x9a6u) {
        data = 5u;
    }
    rom_i2c_write_masked(0x6b, 1, 7, 3, 0, (uint8_t)data);
}

/*
 * The transmit gain chain of chip_v7_set_chan_misc, the driver's own:
 * wifi_get_tx_tab builds the per-subband gains, wifi_get_tx_gain_new walks them, and get_tx_gain_value_new
 * is the search itself. The two tables are libphy's rodata at 0x4200adbc and 0x4200addc, and the middle
 * buffer is the library's own for wifi_get_tx_gain_new: 0x80 at each end of sixteen words.
 */
#define PHY_PARAM_MOST_TPW     0x06 /* the byte phy_set_most_tpw keeps */
#define PHY_PARAM_TX_GAIN_B    0x08
#define PHY_PARAM_TX_GAIN_TAB  0xf1 /* the calibration table rom_set_chan_cal_interp interpolates */
#define PHY_PARAM_TX_GAIN_IDX  0xf7
#define PHY_PARAM_TX_GAIN_CHAN 0x123

static const uint16_t tx_gain_tab[16] = { 63, 63, 47, 39, 31, 23, 14, 13, 12, 7, 5, 3, 2, 1, 0, 0 };
static const int16_t tx_gain_offset[16] = { 34,  26,  20,  15,  9,  0,   -16, -22,
                                            -28, -35, -45, -59, -69, -83, -99, -107 };
static const uint16_t tx_gain_mid[16] = { 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0 };

/*
 * libphy's get_tx_gain_value_new [0x42009d40]: the subband of a value in a table of sixteen thresholds,
 * and the three gains there, the search walking one entry at a time until the value lies between two,
 * or sixteen steps pass.
 */
static uint32_t get_tx_gain_value_new(uint32_t index, int value, uint16_t *out0, uint16_t *out1, int16_t *out2,
                                      const uint16_t *tab0, const uint16_t *tab1, const int16_t *tab2,
                                      uint8_t n, int16_t add, int16_t limit)
{
    uint32_t a0 = index & 0xffu;
    uint32_t i = 0;

    for (;;) {
        if (i == n) {
            break;
        }
        if (value < tab2[a0]) {
            if (a0 == (uint32_t)(uint8_t)(n - 1u)) {
                break;
            }
            a0 = (a0 + 1u) & 0xffu;
            if (value >= tab2[a0]) {
                break;
            }
        } else {
            if (a0 == 0u) {
                break;
            }
            if (value < tab2[a0 - 1u]) {
                break;
            }
            a0 = (a0 - 1u) & 0xffu;
        }
        i = (i + 1u) & 0xffu;
    }
    *out2 = (int16_t)(uint16_t)((uint16_t)value - (uint16_t)tab2[a0]);
    if (limit < (int16_t)(uint16_t)((uint16_t)*out2 + (uint16_t)add) && a0 != 0u) {
        a0 = (a0 - 1u) & 0xffu;
        *out2 = (int16_t)(uint16_t)((uint16_t)value - (uint16_t)tab2[a0]);
    }
    *out0 = tab0[a0];
    *out1 = tab1[a0];
    return a0;
}

/*
 * libphy's wifi_get_tx_gain_new [0x42009f4e]: for each of sixteen subbands the value the channel's calibration
 * gives, its gain from the tables, and the difference the caller's power asks; the library's print here is left out.
 */
static void wifi_get_tx_gain_new(uint32_t chan, const uint8_t *cal, int p3, int p4, const uint16_t *tab5,
                                 const uint16_t *tab6, const int16_t *tab7, uint8_t *out8, uint16_t *out9,
                                 uint16_t *out10, uint8_t verbose)
{
    uint32_t u = (uint32_t)((int (*)(const uint8_t *, uint32_t))phy_get_romfuncs()[ROMFUNC_SET_CHAN_CAL])(
                     cal, chan) &
                 0xffffu;
    uint32_t carry = 0;

    (void)verbose;
    for (uint32_t i = 0; i < 0x20u; i++) {
        int8_t s = (int8_t)(uint8_t)(phy_param[PHY_PARAM_MOST_TPW] - (uint8_t)(i * 4u));
        uint16_t b0, b1;
        int16_t b2;

        carry = get_tx_gain_value_new(carry, (int)s - p3 + p4, &b0, &b1, &b2, tab5, tab6, tab7, 0x10u,
                                      (int16_t)(uint16_t)(0u - u), -8);
        out8[i] = (uint8_t)(b2 - (int)u);
        out9[i] = b1;
        out10[i] = b0;
    }
}

/*
 * libphy's wifi_get_tx_tab [0x4200a0a0], the driver's own: the channel's calibration and where its power lies,
 * from the tables above, into the caller's three buffers: the sixteen byte gains, then two words each.
 */
void wifi_get_tx_tab(uint32_t chan, uint8_t *out8, uint16_t *out9, uint16_t *out10, uint32_t power)
{
    wifi_get_tx_gain_new(chan, &phy_param[PHY_PARAM_TX_GAIN_TAB], (int)(int8_t)phy_param[PHY_PARAM_TX_GAIN_IDX],
                         (int)(int8_t)(uint8_t)(phy_param[PHY_PARAM_TX_GAIN_CHAN] - phy_param[PHY_PARAM_TX_GAIN_B]),
                         tx_gain_tab, tx_gain_mid, tx_gain_offset, out8, out9, out10, (uint8_t)power);
}

/* The channel's little-endian word of phy_param, which phy_set_most_tpw reads its channel from. */
static uint16_t phy_param16(uint32_t off)
{
    return (uint16_t)((uint16_t)phy_param[off] | ((uint16_t)phy_param[off + 1u] << 8));
}

/* The front end's gain memory channel, and its table base in phy_param, which set_tx_gain_mem reads. */
#define FE_GAIN_CHAN       (MODEM_BASE + 0x410u)
#define PHY_PARAM_TX_GAIN_OFF 0x07 /* nonzero leaves set_tx_gain_mem out */
#define PHY_PARAM_GAIN_TABLE  0xd0

/* The ROM's two gain lookups: a baseband gain's index, from the Wi-Fi and the Bluetooth tables. */
uint32_t txbbgain_to_index(uint32_t gain);
uint32_t bt_bb_to_index(uint32_t gain);

/*
 * libphy's set_tx_gain_mem [0x42009b52]: for each of the caller's subbands, its gain's eight-byte entry in
 * phy_param's table, packed into write_gain_mem_new's four words, the front end forced off around the run.
 */
static void set_tx_gain_mem(uint32_t p1, uint32_t p2, const uint16_t *p3, const uint16_t *p4,
                            const uint8_t *p5, const uint8_t *p6, const uint8_t *p7)
{
    uint32_t base = ((p1 != 0u ? 0x20u : 0u) + (REG(FE_GAIN_CHAN) >> 24)) & 0xffu;
    uint32_t table = (uint32_t)(p7[0] | ((uint16_t)p7[1] << 8));

    force_txrx_off(1);
    for (uint32_t i = 0; i < p2; i++) {
        uint32_t gain = p1 == 0u ? p4[i] : p4[0];
        uint32_t idx = p1 == 0u ? (txbbgain_to_index(gain) & 0xffffu) : bt_bb_to_index(gain);
        uint16_t v1 = p3[i];
        uint16_t local[4];

        for (uint32_t k = 0; k < 4u; k++) {
            local[k] = (uint16_t)(p6[idx * 8u + k * 2u] | ((uint16_t)p6[idx * 8u + k * 2u + 1u] << 8));
        }
        write_gain_mem_new((table & 0x1fffu) | ((uint32_t)local[2] << 22) | ((uint32_t)local[1] << 31) |
                               ((uint32_t)local[3] << 13),
                           ((uint32_t)local[0] << 8) | ((uint32_t)local[1] >> 1) | (((gain >> 6) & 0xffu) << 17) |
                               ((uint32_t)(v1 & 7u) << 31) | ((gain & 0x3fu) << 20) | 0x10000000u,
                           ((uint32_t)(int8_t)p5[i] << 7) | ((uint32_t)(v1 & 7u) >> 1) |
                               ((uint32_t)(v1 >> 1) & 0x1cu),
                           (base + i) & 0xffu);
    }
    force_txrx_off(0);
}

/*
 * libphy's wifi_set_tx_gain [0x4200a14c], the driver's own: the transmit gains of a channel, as the calibration
 * and the power ask, built by wifi_get_tx_tab and written into the front end by set_tx_gain_mem.
 */
void wifi_set_tx_gain(uint32_t chan, uint32_t power)
{
    uint8_t cfg[16];
    uint8_t out8[0x20];
    uint16_t out9[0x20];
    uint16_t out10[0x20];

    cfg[0] = phy_param[0x18e];
    cfg[1] = phy_param[0x18f];
    cfg[2] = phy_param[0x190];
    cfg[3] = phy_param[0x191];
    cfg[4] = 0x00;
    cfg[5] = 0x01;
    cfg[6] = 0x00;
    cfg[7] = 0x01;
    cfg[8] = phy_param[0x192];
    cfg[9] = phy_param[0x193];
    cfg[10] = phy_param[0x194];
    cfg[11] = phy_param[0x195];
    cfg[12] = 0x00;
    cfg[13] = 0x01;
    cfg[14] = 0x00;
    cfg[15] = 0x01;

    wifi_get_tx_tab(chan, out8, out9, out10, power);
    if (phy_param[PHY_PARAM_TX_GAIN_OFF] == 0u) {
        set_tx_gain_mem(0, 0x20u, out10, out9, out8, cfg, &phy_param[PHY_PARAM_GAIN_TABLE]);
    }
}

/*
 * libphy's phy_set_most_tpw [0x4200164a]: the byte kept in phy_param's 0x06, then the channel's transmit gain.
 */
#define PHY_PARAM_CHAN14_TPW 0x24

void phy_set_most_tpw(uint8_t tpw)
{
    phy_param[PHY_PARAM_MOST_TPW] = tpw;
    wifi_set_tx_gain((uint32_t)phy_param16(0x11cu), 0u); /* phy_param's channel, the halfword chip_v7_set_chan writes */
}

/*
 * libphy's chan14_mic_cfg [0x42000550]: channel 14's microphone configuration, the baseband's bits 13 and 14,
 * and the byte phy_param keeps with each: 0x24 for channel 14 and 0x06 otherwise.
 */
#define WIFI_BB_CHAN14 (MODEM_BASE + 0x7400u)

void chan14_mic_cfg(uint32_t on)
{
    if (on == 1u) {
        REG(WIFI_BB_CHAN14) = (REG(WIFI_BB_CHAN14) & ~0x6000u) | 0x2000u;
        phy_set_most_tpw((uint8_t)(int8_t)phy_param[PHY_PARAM_CHAN14_TPW]);
    } else {
        REG(WIFI_BB_CHAN14) |= 0x6000u;
        phy_set_most_tpw((uint8_t)(int8_t)phy_param[PHY_PARAM_MOST_TPW]);
    }
}

/*
 * libphy's rfrx_gain_index_new [0x4200703e]: the receive gain's index in the library's two tables,
 * eight entries for Wi-Fi's and seven for Bluetooth's, or the table's count when the gain is not there.
 */
static uint32_t rfrx_gain_index_new(uint32_t hw, uint32_t gain)
{
    static const uint16_t wifi_tab[8] = { 0x40, 0x41, 0x43, 0x6b, 0x77, 0x79, 0x7b, 0x7f };
    static const uint16_t bt_tab[7] = { 0x41, 0x43, 0x6b, 0x77, 0x79, 0x7b, 0x7f };
    const uint16_t *tab = hw == 0u ? wifi_tab : bt_tab;
    uint32_t bound = hw == 0u ? 8u : 7u;
    uint32_t i = 0;

    for (;;) {
        if (tab[i] == gain) {
            return i & 0xffu;
        }
        i++;
        if (bound <= (i & 0xffu)) {
            return i & 0xffu;
        }
    }
}

/* The ROM's receive-gain support, which wr_rx_gain_mem runs through. */
void pbus_debugmode(void);
void pbus_workmode(void);
void set_rxclk_en(uint32_t on);
void set_txclk_en(uint32_t on);

#define PHY_PARAM_RFRX_A 0x14e /* the receive gains and the words the packed entry takes */
#define PHY_PARAM_RFRX_B 0x16e
#define PHY_PARAM_RFRX_C 0x172

/*
 * libphy's wr_rx_gain_mem [0x4200730c]: the receive gains of a run of subbands,
 * each from its index in the tables above or the caller's own, packed into write_gain_mem_new's words,
 * between the debug mode, the receive and transmit clocks and the ROM's powering of the receiver.
 */
void wr_rx_gain_mem(uint32_t p1, uint32_t p2, const uint32_t *p3, const uint16_t *p4,
                    const uint16_t *p5, const uint16_t *p6, uint32_t p7, uint32_t p8)
{
    static const uint16_t gain_tab[7] = { 0, 0x20, 0x30, 0x38, 0x3c, 0x3e, 0x3f };
    /* The library's seven lengths, and the zero its stack holds past them, which its last index reads. */
    static const uint8_t byte_tab[8] = { 0x0b, 0x0a, 0x06, 0x06, 0x06, 0x06, 0x08, 0x00 };
    static const uint8_t sel_tab[8] = { 0x40, 0x41, 0x43, 0x6b, 0x77, 0x79, 0x7b, 0x7f };
    static const uint8_t len_tab[8] = { 0x03, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08 };
    uint32_t base = p1 == 0u ? 0u : 0x50u;
    uint32_t sum = 0;
    uint32_t u10 = 7;
    uint32_t u9;
    uint32_t i4;

    for (uint32_t k = 0; k < 7u; k++) {
        sum = (sum + byte_tab[k]) & 0xffffu;
    }
    if (p8 == 0u) {
        sum = 0;
        u10 = 0;
    }
    i4 = ((p7 - 1u) & 0xfeu) * 2u;
    sum &= 0xffu;
    u9 = (uint32_t)len_tab[u10] & 0xffffu;

    pbus_debugmode();
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_XPD_RX_ON])(0u);
    set_rxclk_en(1);
    set_txclk_en(1);

    for (; sum < p2; sum = (sum + 1u) & 0xffu) {
        uint32_t v;
        uint32_t gidx;
        uint32_t x1, x2;
        uint32_t w0, w1, w2;

        if (p8 == 0u) {
            v = p3[sum];
        } else {
            if ((uint32_t)(uint16_t)(byte_tab[u10] + len_tab[u10]) <= u9 && u10 != 7u) {
                u10 = (u10 + 1u) & 0xffffu;
                u9 = (uint32_t)len_tab[u10] & 0xffffu;
            }
            uint32_t q = u9 / 6u;
            uint32_t r = u9 % 6u;

            u9 = (u9 + 1u) & 0xffffu;
            v = (((uint32_t)gain_tab[q] * 16u + r) & 0xffffu) | ((uint32_t)sel_tab[u10] << 12);
        }
        gidx = rfrx_gain_index_new(p1, (v >> 12) & 0xffffu);
        if (p8 == 0u) {
            const uint16_t *e = (const uint16_t *)((const uint8_t *)p4 + (gidx & 0x7fu) * 4u);

            x2 = e[0];
            x1 = e[1];
        } else {
            x2 = *(const uint16_t *)((const uint8_t *)p6 + i4);
            x1 = *(const uint16_t *)((const uint8_t *)p6 + i4 + 2u);
        }
        if (p1 == 0u) {
            w0 = p5[0];
            w1 = p5[1];
            w2 = phy_param16(0xd4u);
        } else {
            w0 = 0x100;
            w1 = 0x100;
            w2 = 0;
        }
        write_gain_mem_new((w0 << 31) | (w1 << 13) | (x1 << 22) | (w2 & 0x1fffu),
                           (w0 >> 1) | ((uint32_t)phy_param[0x02] << 29) | ((v & 7u) << 17) |
                               (((v >> 4) & 0x7fu) << 20) | (x2 << 8),
                           (((v >> 10) & 0xe0u) | (((v >> 12) & 7u) << 2)) | ((uint32_t)phy_param[0x02] >> 6),
                           (sum + base) & 0xffu);
    }
    set_rxclk_en(0);
    set_txclk_en(0);
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_XPD_RX_ON])(0u);
    pbus_workmode();
}

/*
 * Our own of the ROM's phy_disable_low_rate, which the library's hal_mac_disable_low_rate calls first:
 * its three stores, in their order, each a read-modify-write that clears one bit and touches nothing else --
 * 0x600A8060 bit 10, then 0x600A8060 bit 11, then 0x600A807C bit 11 (disassembly 0x400031B6).
 * mac_low_rate_disable calls this in the ROM's place.
 */
#define WIFI_BB_LOW_RATE0      (MODEM_BASE + 0x8060u)
#define WIFI_BB_LOW_RATE1      (MODEM_BASE + 0x807Cu)
#define WIFI_BB_LOW_RATE_BIT10 (1u << 10)
#define WIFI_BB_LOW_RATE_BIT11 (1u << 11)

void drv_phy_low_rate_disable(void)
{
    REG(WIFI_BB_LOW_RATE0) &= ~WIFI_BB_LOW_RATE_BIT10;
    REG(WIFI_BB_LOW_RATE0) &= ~WIFI_BB_LOW_RATE_BIT11;
    REG(WIFI_BB_LOW_RATE1) &= ~WIFI_BB_LOW_RATE_BIT11;
}

/*
 * The driver's own of libphy's power path, rate_to_index through phy_get_max_pwr, taken from its disassembly
 * and named for the driver; every byte is libphy's own arithmetic over phy_param, and none is the driver's invention.
 * Its result is held to the library's in one run before the driver's takes the library's place.
 */
#define PHY_PARAM_TX_CAP   0x6    /* the cap every rate's power is held to, signed */
#define PHY_PARAM_SUBBAND  0x50   /* above 9 the second byte is the first's own, not the table's */
#define PHY_PARAM_TARGET   0x52   /* get_chan_target_power_new's per-rate limits, 0x12 of them */
#define PHY_PARAM_FCC_CHAN 0x66   /* 1 asks for the four band limits of rate_fcc_index below */
#define PHY_PARAM_FCC_A    0x70   /* rate_fcc_index's first table, read at chan-1, chan+0xd and chan+0x1b */
#define PHY_PARAM_FCC_B    0x67   /* its second, one more limit */
#define PHY_PARAM_CHANNEL  0x11c  /* the halfword chip_v7_set_chan writes the last channel to */

/* The four band limits libphy holds before the ROM's get_rate_fcc_index replaces them: its rodata's 0x54545454. */
#define PHY_TARGET_LIMITS 0x54545454u

static uint8_t clamp100(uint8_t v)
{
    return v > 0x64u ? 0x64u : v;
}

/*
 * The ROM's get_rate_fcc_index, written out: four band limits for the channel, each held to 0x64, three from
 * phy_param's 0x70 at the channel less one, plus 0xd and plus 0x1b, and the fourth from phy_param's 0x67 at the
 * channel less three, or its zero below channel 3 and its eighth from channel 12; the ROM's disassembly 0x40008A3E.
 */
static void rate_fcc_index(uint32_t chan, uint8_t limits[4], const uint8_t *a, const uint8_t *b)
{
    uint32_t i = (chan - 3u) & 0xffffu;
    uint8_t vb = i > 8u ? (chan > 2u ? b[8] : b[0]) : b[i];

    limits[0] = clamp100(a[chan - 1u]);
    limits[1] = clamp100(a[chan + 0xdu]);
    limits[2] = clamp100(a[chan + 0x1bu]);
    limits[3] = clamp100(vb);
}

/* libphy's wifi_11g_rate_chg_new table, its rodata: { 6, 4, 2, 0, 7, 5, 3, 1 }. */
static const uint8_t rate_chg[8] = { 6, 4, 2, 0, 7, 5, 3, 1 };

static uint8_t wifi_11g_rate_chg(uint32_t rate)
{
    uint32_t i = (rate - 8u) & 0xffu;

    return i <= 7u ? rate_chg[i] : 0u;
}

/*
 * libphy's rate_to_index: the rate's index into the target table, from 0 to 0x11,
 * the low-rate word's bit 10 choosing between the eleven-bit rates' two index forms.
 */
static uint32_t rate_to_index(uint32_t rate)
{
    uint8_t t = wifi_11g_rate_chg(rate);

    if (rate <= 7u) {
        return (rate >> 1) & 1u;
    }
    if ((REG(WIFI_BB_LOW_RATE0) & WIFI_BB_LOW_RATE_BIT10) != 0u) {
        return 0u;
    }
    if (rate <= 0xfu) {
        return ((uint32_t)(t >> 1) + 2u) & 0xffu;
    }
    if (rate <= 0x17u) {
        return ((rate >> 1) & 7u) + 6u;
    }
    return (rate - 0xeu) & 0xffu;
}

/* libphy's get_chan_target_power_new: each of 0x12 rates kept to its band's limit and to the cap, both signed. */
static void get_chan_target_power(uint32_t chan, int cap, uint8_t *out,
                                  const uint8_t *limits, uint32_t fcc,
                                  const uint8_t *fcc_a, const uint8_t *fcc_b)
{
    uint32_t band = PHY_TARGET_LIMITS;
    int8_t b[4];

    if (fcc == 1u) {
        uint8_t lim[4];

        rate_fcc_index(chan, lim, fcc_a, fcc_b);
        band = (uint32_t)lim[0] | ((uint32_t)lim[1] << 8u) | ((uint32_t)lim[2] << 16u) |
               ((uint32_t)lim[3] << 24u);
    }
    b[0] = (int8_t)band;
    b[1] = (int8_t)(band >> 8);
    b[2] = (int8_t)(band >> 16);
    b[3] = (int8_t)(band >> 24);
    for (uint32_t i = 0; i < 0x12u; i++) {
        int8_t lim = b[0];
        int8_t v;

        if (i > 1u) {
            lim = b[1];
            if (i > 5u) {
                lim = b[2];
                if (i > 0xdu) {
                    lim = b[3];
                }
            }
        }
        v = (int8_t)limits[i];
        if (lim < v) {
            v = lim;
        }
        if (cap < v) {
            v = cap;
        }
        out[i] = (uint8_t)v;
    }
}

/* libphy's wifi_get_target_power_new: the channel and the calibration's bytes folded into get_chan_target_power. */
static void wifi_get_target_power(uint32_t chan, uint8_t *out)
{
    get_chan_target_power(chan, (int8_t)phy_param[PHY_PARAM_TX_CAP], out,
                          &phy_param[PHY_PARAM_TARGET], phy_param[PHY_PARAM_FCC_CHAN],
                          &phy_param[PHY_PARAM_FCC_A], &phy_param[PHY_PARAM_FCC_B]);
}

/* libphy's get_target_pwr: a rate's two bytes from that table, each held to 0x54; at index 6 to 9 the second is 8 on. */
static void get_target_pwr(uint32_t rate, uint8_t out[2])
{
    uint8_t buf[0x12];
    uint16_t chan = (uint16_t)(phy_param[PHY_PARAM_CHANNEL] | (phy_param[PHY_PARAM_CHANNEL + 1u] << 8));
    uint32_t i = rate_to_index(rate);
    int8_t c;

    wifi_get_target_power(chan, buf);
    c = (int8_t)buf[i];
    out[0] = (uint8_t)c;
    if (((i - 6u) & 0xffu) < 4u) {
        int8_t v = (int8_t)buf[i + 8u];

        out[1] = (uint8_t)v;
        if (c > 0x54) {
            out[0] = 0x54u;
        }
        if (v > 0x54) {
            out[1] = 0x54u;
        }
    } else {
        out[1] = (uint8_t)c;
        if (c > 0x54) {
            out[0] = 0x54u;
            out[1] = 0x54u;
        }
    }
}

/*
 * libphy's phy_get_max_pwr, the driver's own: a quarter each, the second its table's own only above ten subbands.
 * Held to the library's for every channel 1 to 13 and every rate the driver's table is read at, 0 to 0x1b,
 * 364 pairs and none different; see the driver's NOTES.md.
 */
int phy_get_max_pwr(uint32_t index, uint8_t out[2])
{
    uint8_t t2[2];

    /*
     * libphy's rate_to_index runs past its own 0x12-byte target table above rate 0x1f, and its stack answers;
     * the driver asks no rate above 0x1b, so a rate above 0x1f is a fault here, not a value.
     */
    if (index > 0x1fu) {
        drv_must("the rate of the power table", ESP_FAIL);
        return 0;
    }
    get_target_pwr(index, t2);
    out[0] = (uint8_t)((int8_t)t2[0] >> 2);
    if (phy_param[PHY_PARAM_SUBBAND] > 9u) {
        out[1] = (uint8_t)((int8_t)t2[1] >> 2);
    } else {
        out[1] = out[0];
    }
    return 0;
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

/*
 * libphy's chip_v7_set_chan_misc [0x4200580c]: the channel's transmit gain, then its receive gains.
 * The buffer the library passes to wr_rx_gain_mem is not read for its last argument set, which this passes.
 */
static void chip_v7_set_chan_misc(uint32_t channel)
{
    uint32_t buf[4];

    set_chan_reg(1);
    wifi_set_tx_gain(channel, 0u);
    wr_rx_gain_mem(0u, (uint32_t)(uint8_t)(phy_param[0x121] + 1u), buf, (const uint16_t *)&phy_param[0x14e],
                   (const uint16_t *)&phy_param[0x16e], (const uint16_t *)&phy_param[0x172], channel, 1u);
}

/*
 * libphy's phy_11p_set [0x420018a2]: the 11p band's settings, which the driver keeps at zero.
 * Its first argument clear writes the two bytes phy_param keeps, and set writes a value from the calibration;
 * both set the baseband's configuration and the analog block 0x67, host 1, registers 4 to 0xf.
 */
#define WIFI_BB_11P (MODEM_BASE + 0x7030u)

static void phy_11p_set(uint32_t p1, uint32_t p2)
{
    phy_param[0x28] = (uint8_t)p1;
    phy_param[0x29] = (uint8_t)p2;
    if (p1 == 0u) {
        REG(MODEM_SYSCON_WIFI_BB_CFG) &= ~0x1cu;
        REG(WIFI_BB_11P) |= 0x20u;
        rom_i2c_write(0x67, 1, 4, phy_param[0xe9]);
        rom_i2c_write(0x67, 1, 5, phy_param[0xe9]);
        rom_i2c_write(0x67, 1, 6, phy_param[0xea]);
        rom_i2c_write(0x67, 1, 7, phy_param[0xea]);
        rom_i2c_write(0x67, 1, 0xc, phy_param[0xe9]);
        rom_i2c_write(0x67, 1, 0xd, phy_param[0xe9]);
        rom_i2c_write(0x67, 1, 0xe, phy_param[0xea]);
        rom_i2c_write(0x67, 1, 0xf, phy_param[0xea]);
    } else {
        uint32_t k = p2 == 0u ? 4u : 5u;
        uint32_t div = p2 == 0u ? 100u : 0x32u;
        int32_t v = (int32_t)(((uint32_t)(phy_param[0xe8] + 0x38u) * 0x67u) / div) - 8;
        uint8_t d;

        REG(MODEM_SYSCON_WIFI_BB_CFG) = (REG(MODEM_SYSCON_WIFI_BB_CFG) & ~0x1cu) | (k << 2);
        REG(WIFI_BB_11P) &= ~0x20u;
        if (v > 0x3f) {
            v = 0x3f;
        }
        d = (uint8_t)v;
        rom_i2c_write(0x67, 1, 4, d);
        rom_i2c_write(0x67, 1, 5, d);
        rom_i2c_write(0x67, 1, 6, d);
        rom_i2c_write(0x67, 1, 7, d);
        rom_i2c_write(0x67, 1, 0xc, d);
        rom_i2c_write(0x67, 1, 0xd, d);
        rom_i2c_write(0x67, 1, 0xe, d);
        rom_i2c_write(0x67, 1, 0xf, d);
    }
}

/*
 * Our own chip_v7_set_chan, the library's phy_change_channel_ and set_chanfreq's work: the front end tuned to a
 * channel, as the library's chip_v7_set_chan [0x42005868] does.
 * It writes the channel and the second argument into phy_param's 0x11c to 0x11f, takes the AGC down, forces the
 * TX and RX off, calibrates the RF PLL, tunes through the ROM and the driver's own leaves, and puts the AGC back.
 * The library's critical section around it is a pair of empty returns, so this leaves it out.
 * build.mk weakens the library's, so this takes its place; its other callers in the library call it too.
 */
void chip_v7_set_chan(uint32_t chan, uint32_t second)
{
    int32_t p20 = (int16_t)phy_param16(0x20u);
    uint32_t ch = chan;
    uint32_t freq;

    if (chan > 2000u) {
        ch = mhz2ieee(chan) & 0xffffu;
    }
    /*
     * The driver keeps to 2.4 GHz channels 1 to 13 (EN 300 328's band); the library tunes any channel,
     * so a caller that asks for one outside them is a fault here, not a tuning.
     * The driver's own calls are channels 1 to 13 and the library's are bb_init's 11 and the tracking
     * functions' current channel, so nothing reaches this but a request the driver must not make.
     */
    if (ch < 1u || ch > 13u) {
        drv_must("the channel to tune", ESP_FAIL);
        return;
    }
    phy_param[0x11f] = (uint8_t)second;
    phy_param[0x11e] = second != 0u ? 1u : 0u;
    phy_param[0x11c] = (uint8_t)ch;
    phy_param[0x11d] = (uint8_t)(ch >> 8);
    disable_agc();
    force_txrx_off(1);
    phy_bbpll_cal(1);
    freq = chan_to_freq(ch);
    set_freq_i2c(freq);
    set_channel_rfpll_freq(freq, phy_param[PHY_PARAM_XTAL], p20);
    chip_v7_set_chan_misc(ch);
    phy_i2c_master_mem_txcap();
    if (phy_param[0x26] != 0u) {
        chan14_mic_cfg(ch == 0xeu ? 1u : 0u);
    }
    if (phy_param[0x28] != 0u) {
        phy_11p_set(phy_param[0x28], phy_param[0x29]);
    }
    set_rx_comp_new();
    phy_bbpll_cal(0);
    force_txrx_off(0);
    enable_agc();
}

/*
 * Our own phy_change_channel, which mac.c's home-channel programming calls: the library's takes the width as its
 * fourth argument, turns the frequency into the band's channel through the ROM's mhz2ieee, and tunes with
 * chip_v7_set_chan above; the library's set_chanfreq between them has nothing else in it.
 */
void phy_change_channel(uint32_t mhz, uint32_t a1, uint32_t a2, uint32_t width)
{
    (void)a1;
    (void)a2;
    chip_v7_set_chan(mhz2ieee(mhz) & 0xffffu, width);
}

/*
 * The leaves of libphy's phy_wakeup_init the driver's own, this step; each is held to the library's with
 * phy-ab.py, which runs both from one state in an emulator, and build.mk then weakens the library's in its copy.
 */

/* The analog I2C master's three clock words in MODEM_LPCON, which i2c_clk_sel below sets. */
#define MODEM_LPCON_I2C_CLK0 (MODEM_LPCON_BASE + 0x824u)
#define MODEM_LPCON_I2C_CLK1 (MODEM_LPCON_BASE + 0x828u)
#define MODEM_LPCON_I2C_CLK2 (MODEM_LPCON_BASE + 0x82Cu)

/*
 * libphy's i2c_clk_sel [0x42002678]: each of the three words takes the selector's bits, two read-modify-writes
 * each as the library writes it, bits 6 to 10 from the selector's bits 2 to 6 and bits 0 to 5 from its 1 to 6.
 */
void i2c_clk_sel(uint32_t sel)
{
    uint32_t hi = (sel << 4) & 0x7c0u;
    uint32_t lo = (sel >> 1) & 0x3fu;

    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t reg = MODEM_LPCON_I2C_CLK0 + i * 4u;

        REG(reg) = (REG(reg) & ~0x7c0u) | hi;
        REG(reg) = (REG(reg) & ~0x3fu) | lo;
    }
}

/* The analog block and register i2c_bbpll_set keeps the byte it reads. */
#define I2C_BBPLL_BLOCK       0x66
#define PHY_PARAM_BBPLL_BYTE  0x4a

/*
 * libphy's i2c_bbpll_set [0x42002ae4]: powering the baseband PLL up takes the byte of block 0x66, host 0,
 * register 4 above bit 2 and keeps it in phy_param as it answers; powering it down writes that byte back.
 */
void i2c_bbpll_set(int on)
{
    if (on != 0) {
        rom_i2c_write_masked(I2C_BBPLL_BLOCK, 0, 4, 3, 2, 2);
        phy_param[PHY_PARAM_BBPLL_BYTE] = rom_i2c_read(I2C_BBPLL_BLOCK, 0, 4);
    } else {
        rom_i2c_write(I2C_BBPLL_BLOCK, 0, 4, phy_param[PHY_PARAM_BBPLL_BYTE]);
    }
}

/* The front end's and the baseband's words the library's reg_set_new writes, unnamed in any header. */
#define FE_REG_SET0     (MODEM_BASE + 0x8F8u)
#define FE_REG_SET1     (MODEM_BASE + 0x44Cu)
#define FE_REG_SET2     (MODEM_BASE + 0x46Cu)
#define FE_REG_SET3     (MODEM_BASE + 0x450u)
#define WIFI_BB_REG_SET (MODEM_BASE + 0x7428u)

/* libphy's reg_set_new [0x420030ea]: five read-modify-writes bringing the front end and the baseband up. */
void reg_set_new(void)
{
    REG(FE_REG_SET0) = (REG(FE_REG_SET0) & ~0xffu) | 4u;
    REG(FE_REG_SET1) |= 0x08000000u;
    REG(FE_REG_SET2) &= ~0x20000000u;
    REG(FE_REG_SET3) |= 0x80008000u;
    REG(WIFI_BB_REG_SET) = (REG(WIFI_BB_REG_SET) & ~0x3f00u) | 0x1500u;
}

/* The frequency module's own two words the library's freq_reg_init writes, unnamed in any header. */
#define FE_FREQ_SET_CONF2 (MODEM_BASE + 0xC8u)
#define FE_FREQ_SET_CONF4 (MODEM_BASE + 0xD4u)

/* phy_param's byte the library's freq_reg_init reads for its default. */
#define PHY_PARAM_FREQ_SEL 0x19b

/*
 * libphy's freq_reg_init [0x42001c62]: the frequency setting's word cleared and its hold bit set,
 * then the two parameters at bits 17 to 24, with phy_param 0x19b naming the default pair, then the
 * module's own three words -- a 2-bit field, a 0x19800249 and a 0x25824e50, which is the setting's status word.
 */
void freq_reg_init(uint32_t param1, uint32_t param2)
{
    REG(FE_FREQ_SET_CONF) &= 0x5bffbfffu;
    REG(FE_FREQ_SET_CONF) |= 0x02000000u;
    if (phy_param[PHY_PARAM_FREQ_SEL] != 0u) {
        param1 = 0u;
        param2 = 2u;
    }
    REG(FE_FREQ_SET_CONF) = (REG(FE_FREQ_SET_CONF) & 0xfe01ffffu) |
                            ((((param2 << 4) | param1) << 17) & 0x01fe0000u);
    REG(FE_FREQ_SET_CONF4) = (REG(FE_FREQ_SET_CONF4) & 0xfffff87fu) | 0x300u;
    REG(FE_FREQ_SET_CONF2) = 0x19800249u;
    REG(FE_FREQ_SET_STATUS) = 0x25824e50u;
}

/*
 * libphy's bias_reg_set [0x42002cc2], whose argument it does not read: the analog bias block 0x6a, host 1,
 * three masked writes, 0xf to register 0 and 0xc to registers 1 and 0's high nibble.
 */
void bias_reg_set(int on)
{
    (void)on;
    rom_i2c_write_masked(0x6a, 1, 0, 3, 0, 0x0f);
    rom_i2c_write_masked(0x6a, 1, 1, 3, 0, 0x0c);
    rom_i2c_write_masked(0x6a, 1, 0, 7, 4, 0x0c);
}

/* The baseband's noise-floor word, which the library's noise_floor_auto_set_new sets. */
#define WIFI_BB_NOISE_FLOOR (MODEM_BASE + 0x7018u)

/* libphy's noise_floor_auto_set_new [0x42004f46]: two bits of the baseband's noise-floor word, set in order. */
void noise_floor_auto_set_new(void)
{
    REG(WIFI_BB_NOISE_FLOOR) |= 0x00800000u;
    REG(WIFI_BB_NOISE_FLOOR) |= 0x10000000u;
}

/* The front end's Bluetooth filter word, which the library's bt_filter_reg_new sets. */
#define FE_BT_FILTER (MODEM_BASE + 0x900u)

/* libphy's bt_filter_reg_new [0x42004f60]: three read-modify-writes of the front end's filter word. */
void bt_filter_reg_new(void)
{
    REG(FE_BT_FILTER) |= 0x02000000u;
    REG(FE_BT_FILTER) &= 0xffbfffffu;
    REG(FE_BT_FILTER) &= 0xfe7fffffu;
}

/* The ROM's i2c_paral_write_num: six seven-byte entries written through the analog I2C in parallel. */
void i2c_paral_write_num(const void *a0, const void *a1, const void *a2, const void *a3, const void *a4,
                         const void *a5, uint32_t count, uint32_t flag);

/*
 * libphy's pbus_clear_reg [0x420040c6]: the ROM's pbus_debugmode, twelve calls of the ROM's pbus_force_test
 * through the table clearing a window of the front end in pairs of channels, then pbus_workmode.
 */
void pbus_clear_reg(void)
{
    static const struct {
        uint8_t win;
        uint8_t chan;
        uint16_t value;
    } steps[12] = {
        { 4, 1, 0x000 }, { 4, 2, 0x000 }, { 5, 1, 0x000 }, { 5, 2, 0x000 },
        { 0, 1, 0x000 }, { 0, 2, 0x000 }, { 1, 1, 0x000 }, { 1, 2, 0x000 },
        { 2, 1, 0x100 }, { 3, 1, 0x100 }, { 2, 2, 0x100 }, { 3, 2, 0x100 },
    };

    pbus_debugmode();
    for (uint32_t i = 0; i < 12u; i++) {
        ((void (*)(uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_FORCE_TEST])(
            steps[i].win, steps[i].chan, steps[i].value);
    }
    pbus_workmode();
}

/* The analog I2C master's parallel-write word in MODEM_LPCON, which the library's phy_i2c_init2 sets. */
#define MODEM_LPCON_I2C_MST0 (MODEM_LPCON_BASE + 0x820u)

/*
 * libphy's phy_i2c_init2 [0x4200296e]: six entries the analog I2C writes in parallel -- 0x6b everywhere,
 * the channel pairs 2 to 8 and the block 0x67's, one pair with a 0x44 -- the master mask the ROM answers
 * with at bits 4 to 15, the entries, the mask 0x1f00, all inside the ROM's I2C critical section.
 */
void phy_i2c_init2(void)
{
    uint8_t e0[7] = { 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b };
    uint8_t e1[7] = { 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    uint8_t e2[7] = { 0x72, 0x3b, 0xa4, 0x02, 0x88, 0xb8, 0x81 };
    uint8_t e3[7] = { 0x62, 0x62, 0x62, 0x62, 0x67, 0x67, 0x67 };
    uint8_t e4[7] = { 0x00, 0x04, 0x0f, 0x0b, 0x02, 0x02, 0x02 };
    uint8_t e5[7] = { 0x68, 0x28, phy_param[0x196], 0x44, 0x26, 0x26, 0x26 };
    uint32_t mask;

    ((void (*)(void))phy_get_romfuncs()[ROMFUNC_I2C_ENTER_CRITICAL])();
    mask = ((uint32_t (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_I2C_MST0_MASK])(0x6bu);
    REG(MODEM_LPCON_I2C_MST0) = (REG(MODEM_LPCON_I2C_MST0) & 0xfffe000fu) | (mask & 0xfff0u);
    i2c_paral_write_num(e0, e1, e2, e3, e4, e5, 7u, 0u);
    REG(MODEM_LPCON_I2C_MST0) = (REG(MODEM_LPCON_I2C_MST0) & 0xfffe000fu) | 0x1f00u;
    ((void (*)(void))phy_get_romfuncs()[ROMFUNC_I2C_EXIT_CRITICAL])();
}

/*
 * libphy's filter_dcap_set [0x420026e0]: eighteen writes of the analog block 0x67, host 1, from the
 * calibration's bytes in phy_param -- 0xed at 0x14, 0x15, 0x18, 0x19; 0xee at 0x16, 0x17; 0xf0 at 0x1c
 * to 0x1f, its top bit set at 0x1e; 0xe9 at 4, 5, 0xc, 0xd; 0xea at 6, 7, 0xe, 0xf.
 */
void filter_dcap_set(void)
{
    rom_i2c_write(0x67, 1, 0x14, phy_param[0xed]);
    rom_i2c_write(0x67, 1, 0x15, phy_param[0xed]);
    rom_i2c_write(0x67, 1, 0x16, phy_param[0xee]);
    rom_i2c_write(0x67, 1, 0x17, phy_param[0xee]);
    rom_i2c_write(0x67, 1, 0x18, phy_param[0xed]);
    rom_i2c_write(0x67, 1, 0x19, phy_param[0xed]);
    rom_i2c_write(0x67, 1, 0x1c, phy_param[0xf0]);
    rom_i2c_write(0x67, 1, 0x1d, phy_param[0xf0]);
    rom_i2c_write(0x67, 1, 0x1e, (uint8_t)(phy_param[0xf0] | 0x40u));
    rom_i2c_write(0x67, 1, 0x1f, phy_param[0xf0]);
    rom_i2c_write(0x67, 1, 0x04, phy_param[0xe9]);
    rom_i2c_write(0x67, 1, 0x05, phy_param[0xe9]);
    rom_i2c_write(0x67, 1, 0x06, phy_param[0xea]);
    rom_i2c_write(0x67, 1, 0x07, phy_param[0xea]);
    rom_i2c_write(0x67, 1, 0x0c, phy_param[0xe9]);
    rom_i2c_write(0x67, 1, 0x0d, phy_param[0xe9]);
    rom_i2c_write(0x67, 1, 0x0e, phy_param[0xea]);
    rom_i2c_write(0x67, 1, 0x0f, phy_param[0xea]);
}

/* The ROM's frequency-memory lookups the library's frequency chain runs. */
void freq_i2c_num_addr(void *buf, uint32_t count);
uint32_t get_freq_mem_param(uint32_t index);

/* The frequency module's memory-write word, which the library's freq_i2c_mem_write_new writes. */
#define FE_FREQ_SET_MEM (MODEM_BASE + 0xD0u)

/*
 * libphy's freq_i2c_mem_write_new [0x420021ba]: the sub-address at the frequency setting's bits 4 to 11,
 * then the data and the length at 0x600a00d0, the setting's own bit 15 pulsed.
 */
void freq_i2c_mem_write_new(uint32_t addr, uint32_t data, uint32_t len)
{
    REG(FE_FREQ_SET_CONF) = (REG(FE_FREQ_SET_CONF) & 0xffffc00fu) | ((addr << 4) & 0x3ff0u);
    REG(FE_FREQ_SET_MEM) = (len << 24) | data;
    REG(FE_FREQ_SET_CONF) |= 0x8000u;
    REG(FE_FREQ_SET_CONF) &= ~0x8000u;
}

/*
 * libphy's freq_get_i2c_data [0x42001cde]: the table of the ten I2C words the frequency module is programmed
 * through -- block, host or register and a counter each, and the four data words -- built from three reads of
 * the analog blocks 0x62, 0x63 and 0x67; entries past the tenth are zero.
 */
void freq_get_i2c_data(uint8_t *b0, uint8_t *b1, uint8_t *b2, uint32_t *b3, uint32_t count)
{
    uint32_t s7, s3, s6, dat;
    uint8_t a5 = 0x20u, a4 = 0x10u, a6 = 0x00u;

    rom_i2c_write_masked(0x62, 1, 0x0b, 6, 6, 1);
    s6 = rom_i2c_read(0x62, 1, 0x0b);
    uint32_t v2 = rom_i2c_read(0x63, 1, 0);
    uint32_t v3 = rom_i2c_read(0x67, 1, 3);

    s7 = v2 & ~0x08u;
    s3 = (v2 | 0x08u) & 0xffu;
    dat = ((v3 & ~0x04u) << 8) | (((v3 | 0x04u) & 0xffu) << 16) | ((v3 | 0x04u) & 0xffu);

    for (uint32_t k = 0; k < count; k++) {
        switch (k) {
        case 0: b0[0] = 0x62; b1[0] = 1;     b2[0] = a5;          a5++; break;
        case 1: b0[1] = 0x62; b1[1] = 2;     b2[1] = a5;          a5++; break;
        case 2: b0[2] = 0x63; b1[2] = 0;     b2[2] = a4; b3[2] = s7; a4++; break;
        case 3: b0[3] = 0x63; b1[3] = 6;     b2[3] = a5;          a5++; break;
        case 4: b0[4] = 0x63; b1[4] = 5;     b2[4] = a5;          a5++; break;
        case 5: b0[5] = 0x63; b1[5] = 4;     b2[5] = a5;          a5++; break;
        case 6: b0[6] = 0x63; b1[6] = 3;     b2[6] = a5;          a5++; break;
        case 7: b0[7] = 0x63; b1[7] = 0;     b2[7] = a4; b3[7] = s3; a4++; break;
        case 8: b0[8] = 0x62; b1[8] = 0x0b;  b2[8] = a4; b3[8] = s6; a4++; break;
        case 9: b0[9] = 0x67; b1[9] = 3;     b2[9] = a6; b3[9] = dat; a6++; break;
        default: b0[k] = 0; b1[k] = 0; b2[k] = 0; b3[k] = 0; break;
        }
    }
}

/*
 * libphy's freq_i2c_write_set_new [0x42001ff6]: each of the ten entries freq_get_i2c_data built written into
 * the frequency module, one to three memory writes each: the entry's low nibble with the ROM's lookup of its
 * high nibble deciding the address; then the ROM's freq_i2c_num_addr takes the entry table when the run ends.
 */
void freq_i2c_write_set_new(const uint8_t *p0, const uint8_t *p1, const uint8_t *p2, const uint32_t *p3,
                            uint32_t count)
{
    uint8_t cbuf[0x20] = { 0 };

    for (uint32_t i = 0; i < count; i++) {
        uint32_t s10 = ((uint32_t)p1[i] << 8) | p0[i];
        uint32_t word = p3[i];
        uint32_t hi = p2[i] >> 4;
        uint32_t lo = p2[i] & 0xfu;
        uint32_t cb = lo;

        if (hi == 1u) {
            cb = lo + 1u;
        } else if (hi != 0u) {
            cb = lo + 4u;
        }
        cbuf[i] = (uint8_t)cb;

        uint32_t ret = get_freq_mem_param(hi);
        uint32_t low = ret & 0xffu;
        uint32_t high = (ret >> 8) & 0xffu;
        uint32_t packed = ((word << 16) & 0xff0000u) | s10;

        if (hi == 0u) {
            uint32_t addr0 = lo * 3u + low;

            freq_i2c_mem_write_new(addr0, packed, 7);
            freq_i2c_mem_write_new((high + lo) * 3u + low, ((word << 8) & 0xff0000u) | s10, 7);
            freq_i2c_mem_write_new(addr0 + high * 6u, (word & 0xff0000u) | s10, 7);
        } else if (hi == 1u) {
            freq_i2c_mem_write_new(lo * 3u + low, packed, 7);
        } else {
            freq_i2c_mem_write_new(lo * 2u + low, s10, 3);
        }
    }
    freq_i2c_num_addr(cbuf, count);
}

/*
 * libphy's freq_i2c_data_write_new [0x42002146]: the ten entries built and then written, over six buffers
 * on its own stack; the driver's own as the library's.
 */
void freq_i2c_data_write_new(void)
{
    uint8_t b0[0x20], b1[0x20], b2[0x20];
    uint32_t b3[0x20];

    freq_get_i2c_data(b0, b1, b2, b3, 10u);
    freq_i2c_write_set_new(b0, b1, b2, b3, 10u);
}

/* The ROM's bring-up leaves phy_reg_init calls, which this file calls as the library does. */
void iq_corr_enable(void);
void wifi_agc_sat_gain(uint32_t value);
void bb_wdg_cfg(void);
void tx_pwctrl_bg_init(void);
void phy_ant_init(void);
void mac_enable_bb(uint32_t on);
void fe_txrx_reset(void);
void i2c_master_reset(void);
void i2cmst_reg_init(void);
void fe_reg_init(void);
void write_chan_freq(uint32_t chan);
void set_pbus_reg(void);
void phy_en_hw_set_freq(void);

/* The baseband's and the front end's bring-up words, unnamed in any header, named for the leaf that writes them. */
#define WIFI_BB_11B_0 (MODEM_BASE + 0x7044u)
#define WIFI_BB_11B_1 (MODEM_BASE + 0x7124u)
#define WIFI_BB_11B_2 (MODEM_BASE + 0x7104u)
#define WIFI_BB_11B_3 (MODEM_BASE + 0x8004u)

/*
 * libphy's rx_11b_opt [0x42004aa4]: the two long-range 11b settings of the baseband and the front end,
 * five read-modify-writes each way, with the last two shared: the baseband's 0x7104 clearing its high bits
 * and setting 0x1c8 either way.
 */
void rx_11b_opt(uint32_t on)
{
    if (on != 0u) {
        REG(WIFI_BB_11B_0) |= 0x003f0000u;
        REG(WIFI_BB_11B_0) = (REG(WIFI_BB_11B_0) & 0xffffc0ffu) | 0x2100u;
        REG(WIFI_BB_11B_1) = (REG(WIFI_BB_11B_1) & 0xffff03ffu) | 0x8400u;
        REG(WIFI_BB_11B_1) = (REG(WIFI_BB_11B_1) & ~0x0fu) | 0x3u;
        REG(WIFI_BB_11B_3) = (REG(WIFI_BB_11B_3) & 0xffff0fffu) | 0x9000u;
    } else {
        REG(WIFI_BB_11B_0) = (REG(WIFI_BB_11B_0) & 0xffc0ffffu) | 0x003e0000u;
        REG(WIFI_BB_11B_0) = (REG(WIFI_BB_11B_0) & 0xffffc0ffu) | 0x1800u;
        REG(WIFI_BB_11B_1) = (REG(WIFI_BB_11B_1) & 0xffff03ffu) | 0x6000u;
        REG(WIFI_BB_11B_1) = (REG(WIFI_BB_11B_1) & ~0x0fu) | 0x4u;
        REG(WIFI_BB_11B_3) = (REG(WIFI_BB_11B_3) & 0xffff0fffu) | 0x6000u;
    }
    REG(WIFI_BB_11B_2) = (REG(WIFI_BB_11B_2) & 0xfffffe00u) | 0x1c8u;
}

/* The front end's and the baseband's words the library's tx_paon_set_new writes. */
#define WIFI_BB_PAON_0 (MODEM_BASE + 0x7C00u)
#define WIFI_BB_PAON_1 (MODEM_BASE + 0x7C6Cu)
#define WIFI_BB_PAON_2 (MODEM_BASE + 0x7C30u)
#define FE_REG_SET4    (MODEM_BASE + 0x8FCu)

/* libphy's tx_paon_set_new [0x42004b68]: the transmit PA's settings in six read-modify-writes. */
void tx_paon_set_new(void)
{
    REG(WIFI_BB_PAON_0) = (REG(WIFI_BB_PAON_0) & 0xffe007ffu) | 0x0000a000u;
    REG(FE_REG_SET0) = (REG(FE_REG_SET0) & 0xffff00ffu) | 0x00005000u;
    REG(WIFI_BB_PAON_1) = 0x0140c81eu;
    REG(WIFI_BB_PAON_2) = (REG(WIFI_BB_PAON_2) & ~0x3ffu) | 0x1eu;
    REG(FE_REG_SET4) = (REG(FE_REG_SET4) & 0x0000ffffu) | 0x0a0e0000u;
    REG(FE_REG_SET4) = (REG(FE_REG_SET4) & 0xffff00ffu) | 0x0000c800u;
}

/* The baseband's and the front end's words the library's agc_reg_init_new writes. */
#define WIFI_BB_AGC_0 (MODEM_BASE + 0x713Cu)
#define WIFI_BB_AGC_1 (MODEM_BASE + 0x7094u)
#define WIFI_BB_AGC_2 (MODEM_BASE + 0x702Cu)
#define WIFI_BB_AGC_3 (MODEM_BASE + 0x7128u)
#define WIFI_BB_AGC_4 (MODEM_BASE + 0x7068u)
#define FE_AGC_0      (MODEM_BASE + 0x958u)

/*
 * libphy's agc_reg_init_new [0x42004c04]: the automatic gain control's settings from the two calibration
 * bytes, eight read-modify-writes, the last word 0x404 outright.
 */
void agc_reg_init_new(uint32_t param1, uint32_t param2)
{
    uint32_t n = param1 - 1u;

    REG(WIFI_BB_AGC_0) = (REG(WIFI_BB_AGC_0) & 0xfe03ffffu) | ((n << 18) & 0x01fc0000u);
    REG(WIFI_BB_AGC_1) = (REG(WIFI_BB_AGC_1) & ~0x1fcu) | ((n << 2) & 0x1fcu);
    REG(WIFI_BB_AGC_2) = (REG(WIFI_BB_AGC_2) & 0xffff80ffu) | ((param1 << 8) & 0x7f00u);
    REG(FE_AGC_0) = (REG(FE_AGC_0) & 0xfffff00fu) | (param1 << 4);
    REG(FE_AGC_0) = (REG(FE_AGC_0) & 0xfff00fffu) | (((param2 + 0x50u) << 12) & 0x000ff000u);
    REG(WIFI_BB_AGC_2) = (REG(WIFI_BB_AGC_2) & 0x00ffffffu) | 0x32000000u;
    REG(WIFI_BB_AGC_2) |= 0x00800000u;
    REG(WIFI_BB_AGC_2) &= 0xff7fffffu;
    REG(WIFI_BB_AGC_3) = (REG(WIFI_BB_AGC_3) & 0x00ffffffu) | 0xd2000000u;
    REG(WIFI_BB_AGC_4) = 0x404u;
}

/* The baseband's and the front end's words the library's bb_reg_init_new writes. */
#define WIFI_BB_INIT_0  (MODEM_BASE + 0x7400u)
#define WIFI_BB_INIT_1  (MODEM_BASE + 0x7848u)
#define WIFI_BB_INIT_2  (MODEM_BASE + 0x7808u)
#define WIFI_BB_INIT_3  (MODEM_BASE + 0x78DCu)
#define WIFI_BB_INIT_4  (MODEM_BASE + 0x78E4u)
#define WIFI_BB_INIT_5  (MODEM_BASE + 0x790Cu)
#define WIFI_BB_INIT_6  (MODEM_BASE + 0x7CA8u)
#define WIFI_BB_INIT_7  (MODEM_BASE + 0x7980u)
#define WIFI_BB_INIT_8  (MODEM_BASE + 0x7890u)
#define WIFI_BB_INIT_9  (MODEM_BASE + 0x7A28u)
#define WIFI_BB_INIT_10 (MODEM_BASE + 0x7424u)
#define WIFI_BB_INIT_11 (MODEM_BASE + 0x7CD0u)
#define WIFI_BB_INIT_12 (MODEM_BASE + 0x7438u)

/* libphy's btbb_wifi_bb_cfg2 [0x42004cd0]: two nibbles of the baseband's 0x7cd0 set. */
void btbb_wifi_bb_cfg2(void)
{
    REG(WIFI_BB_INIT_11) |= 0x000f000bu;
}

/*
 * libphy's bb_reg_init_new [0x42004ce6]: the baseband's and the front end's bring-up, thirteen words,
 * then the transmit and receive filter bits of the frequency setting, ending in btbb_wifi_bb_cfg2 above.
 */
void bb_reg_init_new(void)
{
    REG(WIFI_BB_INIT_0) |= 0x6000u;
    REG(WIFI_BB_INIT_1) = (REG(WIFI_BB_INIT_1) & 0xff000000u) | 0x000433afu;
    REG(WIFI_BB_INIT_1) = (REG(WIFI_BB_INIT_1) & 0xe0ffffffu) | 0x17000000u;
    REG(WIFI_BB_INIT_2) = (REG(WIFI_BB_INIT_2) & 0xffffc07fu) | 0x3000u;
    REG(WIFI_BB_INIT_3) = (REG(WIFI_BB_INIT_3) & 0xffffc07fu) | 0x100u;
    REG(WIFI_BB_INIT_4) = REG(WIFI_BB_INIT_4) & 0xffbfffffu;
    REG(WIFI_BB_PAON_2) = REG(WIFI_BB_PAON_2) & 0xfff00fffu;
    REG(WIFI_BB_INIT_5) = REG(WIFI_BB_INIT_5) & 0xfffff7ffu;
    REG(WIFI_BB_INIT_6) |= 0x00100000u;
    REG(WIFI_BB_INIT_7) = REG(WIFI_BB_INIT_7) & 0xfdffffffu;
    REG(WIFI_BB_INIT_8) = REG(WIFI_BB_INIT_8) & 0xfdffffffu;
    REG(WIFI_BB_INIT_8) |= 0x01000000u;
    REG(WIFI_BB_INIT_9) = REG(WIFI_BB_INIT_9) & 0xffbfffffu;
    REG(WIFI_BB_INIT_10) = (REG(WIFI_BB_INIT_10) & 0xe01fffffu) | 0x01000000u;
    REG(WIFI_BB_INIT_11) |= 0x000f000fu;
    REG(WIFI_BB_PAON_0) |= 0x200u;
    REG(MODEM_SYSCON_WIFI_BB_CFG) |= 0x800u;
    REG(WIFI_BB_INIT_12) = REG(WIFI_BB_INIT_12) & ~0xc0u;
    REG(WIFI_BB_INIT_12) = REG(WIFI_BB_INIT_12) & ~0x100u;
    btbb_wifi_bb_cfg2();
}

/*
 * libphy's phy_reg_init [0x420031e4]: the front end's and the baseband's register bring-up in thirteen calls,
 * the automatic gain control's two calibration bytes from phy_param.
 */
void phy_reg_init(void)
{
    reg_set_new();
    iq_corr_enable();
    agc_reg_init_new(phy_param[0x121], phy_param[0x120]);
    wifi_agc_sat_gain(0x81825u);
    bb_reg_init_new();
    bb_wdg_cfg();
    tx_paon_set_new();
    rx_11b_opt(1u);
    tx_pwctrl_bg_init();
    noise_floor_auto_set_new();
    phy_ant_init();
    bt_filter_reg_new();
    mac_enable_bb(1u);
}

/*
 * Our own phy_wakeup_init, this step of taking the PHY from libphy.a: the library's phy_wakeup_init_
 * [0x42003270], the one drv_phy_on calls when the PHY was calibrated before.
 * It powers the baseband PLL and the analog bias, brings MODEM_SYSCON's Wi-Fi clocks and the frequency
 * setting up through the driver's own and the ROM's leaves, then takes them down again, clears the RF-off
 * and sensor flags, and clears the Wi-Fi baseband where phy_param 0x19e asks.
 * The library runs this inside the ROM's critical section, the pair of empty returns [0x40002e1e, 0x40002e22],
 * so this leaves that out. build.mk weakens the library's phy_wakeup_init, so this takes its place.
 */
void phy_wakeup_init(void)
{
    uint32_t clk_conf1;

    phy_bbpll_cal(1);
    bias_reg_set(1);
    clk_conf1 = REG(MODEM_SYSCON_CLK_CONF1);
    REG(MODEM_SYSCON_CLK_CONF1) |= 0x404u;
    REG(FE_FREQ_SET_CONF) = (REG(FE_FREQ_SET_CONF) & 0xe7ffffffu) | 0x10000000u;
    phy_set_tsens_power(true);
    phy_get_xtal_freq();
    open_i2c_xpd_new(false);
    force_txrx_off(1);
    pbus_clear_reg();
    i2c_clk_sel(8u);
    fe_txrx_reset();
    i2c_master_reset();
    i2c_bbpll_set(0);
    i2cmst_reg_init();
    freq_reg_init(2u, 4u);
    fe_reg_init();
    pwdet_reg_init_new();
    phy_i2c_init2();
    freq_i2c_data_write_new();
    filter_dcap_set();
    write_chan_freq(phy_param[0x11c]);
    set_pbus_reg();
    phy_reg_init();
    set_chan_reg(1u);
    set_rx_comp_new();
    enable_agc();
    phy_wait_freq_set_busy();
    phy_en_hw_set_freq();
    phy_bbpll_cal(0);
    force_txrx_off(0);
    phy_param[PHY_PARAM_RF_OFF] = 0;
    phy_param[PHY_PARAM_TSENS_XPD] = 0;
    REG(FE_FREQ_SET_CONF) &= 0xe7ffffffu;
    REG(MODEM_SYSCON_CLK_CONF1) = clk_conf1;
    if (phy_param[PHY_PARAM_WIFI_BB_ENABLE] != 0u) {
        phy_wifi_enable_set(0);
    }
}
