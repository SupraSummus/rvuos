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
#include "macregs.h"

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
void chip_v7_set_chan_offset(uint32_t chan);
void phy_change_channel(uint32_t mhz, uint32_t a1, uint32_t a2, uint32_t width);
uint32_t mhz2ieee(uint32_t mhz);
uint32_t chan_to_freq(uint32_t chan);
void set_channel_rfpll_freq(uint32_t freq, uint32_t xtal, int32_t param);
void phy_freq_correct(uint32_t enable, int32_t offset);
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
void wr_rf_freq_mem_new(uint32_t index, const uint32_t *words);
void save_pbus_reg(void);
void write_pbus_mem(const uint32_t *p0, const uint32_t *p1, const uint32_t *p2, const uint32_t *p3,
                    const uint32_t *p4, const uint32_t *p5, const uint32_t *p6, const uint32_t *p7,
                    const uint32_t *p8, const uint32_t *p9, const uint32_t *p10, const uint32_t *p11);
void set_pbus_mem(void);
void rfpll_chgp_cal(void);
void get_rf_freq_init_new(uint32_t count, uint32_t param);
void set_chan_freq_hw_init(uint32_t param1, uint32_t param2);
void freq_i2c_data_write_new(void);
void rx_11b_opt(uint32_t on);
void tx_paon_set_new(void);
void agc_reg_init_new(uint32_t param1, uint32_t param2);
void bb_reg_init_new(void);
void btbb_wifi_bb_cfg2(void);
void phy_reg_init(void);
/* The leaves of libphy's phy_param_track_tot the driver's own, taken over with its disassembly. */
uint32_t read_rf_freq_mem_new(uint32_t addr, uint32_t field);
void pll_cap_mem_update_new(int32_t delta);
int32_t phy_rfpll_cap_correct_new(uint32_t verbose);
int32_t phy_rfpll_cap_init_cal_new(void);
uint32_t phy_wait_freq_set_busy_new(void);
void phy_en_hw_set_freq_new(void);
void phy_dis_hw_set_freq_new(void);
void phy_rfpll_cap_track_new(uint32_t verbose);
int32_t ram_temp_to_power(int32_t temp, int32_t base, uint32_t bt);
void txpwr_cal_track(uint32_t en_bt, uint32_t threshold, uint32_t verbose);
void bt_track_tx_power(uint32_t threshold, uint32_t verbose);
void wifi_track_tx_power(uint32_t threshold, uint32_t verbose);
void tx_i2c_track(void);
uint32_t gen_rx_gain_table_new(uint32_t *out, uint8_t limit, const uint16_t *gains, const int8_t *a,
                               const int8_t *b, uint8_t count, uint32_t verbose);
void phy_force_rx_gain_trig(void);
void set_rx_gain_cal_dc_new(uint32_t mode, uint32_t flag, int16_t *pairs, int16_t *out_a, int16_t *out_b);
void ram_iq_est_enable(uint32_t a0, uint32_t n);
void dc_iq_est_new(uint32_t a0, uint32_t a1, int32_t out[3], uint32_t a3);
void rxdc_est_min_new(uint32_t a0, uint32_t a1, int32_t out[3], uint32_t a3);
void pbus_rx_dco_cal_1step_new(uint32_t a0, uint32_t mode, uint32_t div, int16_t dc[2], uint8_t *flag);
void rx_pbus_reset(uint32_t on);
void set_rx_gain_table(uint32_t p1, uint32_t p2);
/* The receive table and the transmit channel's offset, the driver's own this step (package 6). */
void phy_rx_table_init(void);
void mac_tx_chan_offset_new(uint32_t chan);
/* The receive IQ's mismatch and the saturation checks, the driver's own this step (package 7a). */
void rfrx_sat_rst(uint32_t on);
void phy_check_rx_sat(void);
void dc_iq_est_high(uint32_t a0, uint32_t a1, int32_t out[3]);
void rxiq_get_mis_new(uint32_t gain, uint8_t out[2], uint32_t verbose);
/* The receive IQ's mismatch correction, the driver's own this step (package 7b). */
void rxiq_cover_mg_mp_new(uint32_t gain, uint8_t *out1, uint8_t *out2, uint32_t verbose);
void rfcal_rxiq_new(uint32_t a0, uint32_t a1, uint32_t a2, uint8_t *out, uint32_t verbose);
int32_t get_rfcal_rxiq_data_new(int32_t a0, int32_t a1, uint32_t verbose);
/* The receive IQ calibration's gain search and its block, the driver's own this step (package 7c). */
void set_rx_gain_cal_iq_new(uint32_t save_i2c, uint32_t tone, uint32_t out, uint32_t verbose);
void rxiq_cal_init(uint32_t a0, void *p1, uint32_t a2);
/* The transmit capacitor calibration's three channels and the scan of one byte's two fields, the driver's own
 * this step (package 8). */
void tx_cap_init_loop(void);
void rfcal_txcap_loop(uint32_t verbose, uint8_t *out);
/* The transmit DC calibration's bring-up, Bluetooth's and the loopback transmit IQ set, the driver's own this
 * step (package 9). */
void txdc_cal_init_new(uint16_t *dco, uint32_t a1, uint32_t a2, uint32_t a3);
void bt_txdc_cal_new(void);
void set_lb_txiq_new(uint32_t a0);
/* The head's init-parameter spread, the driver's own this step (package 10a). */
void register_chipv7_phy_init_param(const esp_phy_init_data_t *init_data);
void phy_cal_param_track(uint32_t verbose);
void phy_param_track_tot(uint32_t en_wifi, uint32_t en_ble_154);
void force_txrx_off(uint32_t on);
void txdc_cal_pwdet(uint16_t *dc, uint32_t verbose);
void txdc_cal_pwdet_init(uint32_t a0, uint32_t a1);
void txdc_cal_new(uint16_t dco[4]);
void txiq_cover_new(int32_t param1, int32_t param2, int8_t *out, uint32_t summary, uint32_t verbose);
void rfcal_txiq_new(uint32_t param1, uint16_t *dc, uint16_t *out, uint32_t atten, uint32_t temp, uint32_t mode);
void txiq_cal_init_new(void);

/*
 * The library's own functions the driver's bring-up calls until their packages take them over, and the two
 * analog I2C initialisations and the bring-up itself, the driver's own from this package.
 */
void i2c_sar2_init_code(uint32_t code); /* the ROM's */
void pwdet_code_cal(void);              /* the ROM's */
void i2cmst_reg_init(void);             /* the ROM's */
void fe_reg_init(void);                 /* the ROM's */
void tx_pwctrl_bg_init(void);           /* the ROM's */
void tx_pwctrl_init_new(uint32_t a0);
void bt_tx_gain_init(void);
void phy_set_23dbm(esp_phy_init_data_t *init_data); /* the library's 23 dBm setting, package 11's */
void phy_i2c_init1(void);
void phy_i2c_master_cmd_mem_init(void);
void rf_init(void);
void bb_init(void);
/* The ROM's table of PHY functions, which the library's phy_get_romfunc_addr fills; the driver's own below. */
void phy_get_romfunc_addr(void);
/* The ROM's own, which this file calls as the library does when taking its functions over. */
void phy_bbpll_cal(uint32_t enable);
void freq_module_resetn(void);
uint32_t tsens_code_read(void);
int code_to_temp(uint32_t code, int offset);
extern const esp_phy_init_data_t phy_init_data;
extern uint8_t phy_param[];

/* The RF calibration's saved blob, the driver's own; the definitions and the layout are at the file's end. */
uint32_t phy_get_rf_cal_version(void);
void get_iq_value(uint8_t out[2], uint16_t pair);
void phy_get_mac_addr(uint8_t *mac);
void phy_set_mac_data(uint8_t *cal, uint32_t version);
void phy_rfcal_data_sub(uint8_t *cal, int save);
void rf_cal_data_backup(uint8_t *cal);
void rf_cal_data_recovery(const uint8_t *cal);
uint32_t phy_rfcal_data_check(uint32_t check, uint8_t *cal, const uint8_t *init_data, uint32_t version);
uint32_t phy_rfcal_data_check_value(uint8_t *cal, uint8_t *buf, uint32_t mode);

/* The RC calibration, the driver's own; the definitions and the fields are at the file's end. */
uint8_t get_rc_dout(void); /* the ROM's */
void rc_cal_new(const uint8_t *spans, const uint8_t *divs, const uint8_t *coeffs);
void rc_cal_init(void);
void i2c_rc_cal_set(uint32_t first, uint32_t second);

/* The temperature sensor's read and its calibration, the driver's own; the definitions are below. */
uint32_t tsens_dac_to_index(uint32_t dac);
uint32_t tsens_dac_cal_new(int32_t temp, uint32_t index);
int32_t ram_tsens_temp_read_new(void);
int32_t phy_get_tsens_value(void);
void tsens_temp_read_new(void);
void get_temp_init_new(uint32_t cap_temp_init, uint32_t txcal_temp_init);

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
#define ROMFUNC_I2C_READ_MASK   23 /* rom_i2c_readReg_Mask: block, host, register, msb, lsb, answers the masked bits */
#define ROMFUNC_I2C_WRITE_MASK  24 /* rom_i2c_writeReg_Mask: block, host, register, msb, lsb, value */
#define ROMFUNC_PBUS_FORCE_TEST 29 /* rom_pbus_force_test: a window, its channel and a value */
#define ROMFUNC_RAM_RFPLL_SET_FREQ 12 /* the library's ram_rfpll_set_freq, which fills the table's slot 12: freq, xtal, param, out */

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
/* The temperature sensor's DAC index, which the read keeps and the library's rf_init reads. */
#define PHY_PARAM_TSENS_INDEX 0x16
/* The library's mark that the RF is off: its wakeup clears it, phy_close_rf sets it, and its tracking skips while set. */
#define PHY_PARAM_RF_OFF 0x17
/* The library's mark that its bring-up has run, set at register_chipv7_phy's end; phy_close_rf's work waits on it. */
#define PHY_PARAM_PHY_UP 0x25
/* The library's byte that decides whether its 23 dBm setting applies; its phy_set_23dbm_en writes it, its
 * phy_set_23dbm reads it. What sets it on the board is not known. */
#define PHY_PARAM_23DBM_EN 0x4e
/* The library's mark whether Wi-Fi's baseband is up, under which its wakeup clears the baseband at its end. */
#define PHY_PARAM_WIFI_BB_ENABLE 0x19e

/* phy_param's fields, Espressif's labels from libphy's phy_cal_print and phy_tx_gain_print; PHYREGS.md. */
#define PHY_PARAM_TEMP_CODE         0x00  /* temp_code: the temperature the sensor's read keeps */
#define PHY_PARAM_TXCAL_TEMP        0x48  /* the temperature the transmit calibration block last ran at */
#define PHY_PARAM_TEMP_CODE_INIT    0x12E /* temp_code_init */
#define PHY_PARAM_CAP_TEMP          0x130 /* the temperature the RFPLL capacitance tracking last ran at */
#define PHY_PARAM_TRACK_TEMP_AT     0x04  /* the temperature the transmit power tracking last ran at */
#define PHY_PARAM_PARAM_FLAG        0xA4  /* param_flag: bit 7 the receive DC calibration, 9 the rows, 14 the TXIQ, 23 the RC */
#define PHY_PARAM_WIFI_INIT_POWER   0x52  /* wifi_init_power: the per-rate table, 0x12 of them */
#define PHY_PARAM_WIFI_PWCTRL_ATTEN 0xF1  /* wifi_pwctrl_atten 0xF1..0xF3, the table rom_set_chan_cal_interp reads */
#define PHY_PARAM_WIFI_CAL_POWER    0xF7  /* wifi_cal_power */
#define PHY_PARAM_WIFI_CORRECT_POWER 0x123 /* wifi_correct_power: the channel's corrected power */
#define PHY_PARAM_BT_CORRECT_POWER  0x124 /* bt_correct_power */
#define PHY_PARAM_RXDC_RFRX_WIFI    0x14E /* RXDC_RFRX_WIFI: the receive gains and the packed entry's words */
#define PHY_PARAM_RXDC_CHAN_WIFI    0x172 /* RXDC_CHAN_WIFI */
#define PHY_PARAM_RX_SAT            0x1A4 /* rx_sat; the estimate's loop lets a result through where it is 1 */
#define PHY_PARAM_TXIQ              0xD0  /* TXIQ, the halfword pair set_tx_gain_mem takes with the gain entries */
#define PHY_PARAM_LB_TXIQ           0xE6  /* LB_TXIQ, the loopback's pair the TXIQ calibration writes in mode 2 */
#define PHY_PARAM_TXDC              0xA8  /* TXDC: 0xA8, 0xAA, 0xAC, 0xAE */
#define PHY_PARAM_TXDC_CAL_PWDET    0x18E /* txdc_cal_pwdet: the settled pairs at +0, +2, +4 and +6 */
#define PHY_PARAM_CHANNEL           0x11C /* the halfword chip_v7_set_chan writes the last channel to */
#define PHY_PARAM_CHANNEL_SECOND    0x11F /* the byte beside it, the retune's second argument; no label names it */
#define PHY_PARAM_I2C_RFPLL_0F      0x196 /* i2c_rfpll's register 0x0f read back; rf_init and rfpll_chgp_cal keep it, phy_i2c_init2 reads it */

/* phy_param's `rc_dout` and `RXIQ` fields, Espressif's labels from libphy's phy_cal_print. */
#define PHY_PARAM_RC_DOUT      0xE8 /* one byte */
#define PHY_PARAM_RC_DOUT_WIFI 0xE9 /* four bytes from here, rc_dout: wifi */
#define PHY_PARAM_RC_DOUT_BT   0xED /* four bytes from here, rc_dout: bt */
#define PHY_PARAM_RXIQ         0xD4 /* the receive IQ mismatch, a halfword pair */

/* Registers of the front end's power detector, unnamed in any header, and of the SAR ADC. */
#define FECTRL_PWDET_CONF0           (MODEM_BASE + 0x810u) /* the front end's block is the modem's first */
#define FECTRL_PWDET_CONF1           (MODEM_BASE + 0x814u)
#define FECTRL_PWDET_CONF2           (MODEM_BASE + 0x818u)
#define FECTRL_PWDET_CONF3           (MODEM_BASE + 0x81Cu)
#define FECTRL_PWDET_CONF4           (MODEM_BASE + 0x820u)
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
    REG(FECTRL_PWDET_CONF2) = 0x0F0F0FFFu;
    REG(FECTRL_PWDET_CONF3) = 0x00FF0F64u;
    REG(FECTRL_PWDET_CONF0) = (REG(FECTRL_PWDET_CONF0) & ~0x00000FF0u) | 0x00000500u;
    REG(FECTRL_PWDET_CONF4) = 0x0000AAAAu;
    REG(FECTRL_PWDET_CONF0) = (REG(FECTRL_PWDET_CONF0) & ~0x00700000u) | 0x00200000u;
    REG(FECTRL_PWDET_CONF1) = (REG(FECTRL_PWDET_CONF1) & ~0x000000FFu) | 0x00000008u;
    REG(APB_SARADC_CTRL) |= APB_SARADC2_PWDET_DRV;
}

/* The analog I2C's blocks, libphy's phy_i2c_check's names beside the IDF register headers under
 * build/esp/idf-esp32c6-regi2c/. The host is per call; 0x6A's is host 1 here where regi2c_bias.h says 0. */
#define I2C_RFPLL          0x62 /* i2c_rfpll */
#define I2C_RFPLL_SDM      0x63 /* i2c_rfpll_sdm */
#define I2C_BBPLL          0x66 /* i2c_bbpll; regi2c_bbpll.h I2C_BBPLL 0x66, host 0 */
#define I2C_BBTOP          0x67 /* i2c_bbtop */
#define I2C_PERIF          0x69 /* i2c_perif; regi2c_saradc.h's SAR ADC block */
#define I2C_BIAS           0x6A /* i2c_bias; regi2c_bias.h I2C_BIAS 0x6A, whose host there is 0 */
#define I2C_TXRF           0x6B /* i2c_txrf */
#define I2C_HOST_MST       1    /* the host libphy reads the master's blocks at */
#define I2C_HOST_SELF      0    /* and the host it reads the others at */

/*
 * The RC calibration's three fields on i2c_txrf, host 1, through rom_i2c_writeReg_Mask. No header names
 * them; i2c_rc_set_reg writes the same three registers with its caller's values, and i2c_rc_cal_set writes
 * them with 9, its first argument and its second, so the names stay by register and the bit ranges are the
 * mask's own arguments. What each value selects is not known.
 */
#define I2C_TXRF_RC_CAL_0B      0x0B /* bits 5..4 */
#define I2C_TXRF_RC_CAL_0B_MSB  5
#define I2C_TXRF_RC_CAL_0B_LSB  4
#define I2C_TXRF_RC_CAL_08      0x08 /* bits 7..3 */
#define I2C_TXRF_RC_CAL_08_MSB  7
#define I2C_TXRF_RC_CAL_08_LSB  3
#define I2C_TXRF_RC_CAL_0D      0x0D /* bits 5..2 */
#define I2C_TXRF_RC_CAL_0D_MSB  5
#define I2C_TXRF_RC_CAL_0D_LSB  2

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
        if (((int (*)(uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_READ_REG])(I2C_RFPLL_SDM, I2C_HOST_MST, 0) ==
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
    ((void (*)(uint8_t, uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_REG])(I2C_BBTOP, I2C_HOST_MST, 2, 6);
}

/*
 * The ROM's rom_pwdet_sar2_init, which rom_en_pwdet calls through the table, without its last store:
 * the SAR ADC's capacitance again, at 0x600B1C54 in LP_WDT's page, where ESP-IDF's headers list no register.
 */
static void pwdet_sar2_init(void)
{
    REG(FECTRL_PWDET_CONF1) |= 0x00003000u;
    REG(FECTRL_PWDET_CONF1) &= ~0x00000200u;
    REG(FECTRL_PWDET_CONF4) = 0x0000016Au;
}

/*
 * The factory MAC address as the eFuse holds it, last byte first, which the root task read for the driver.
 * Kept a call, not inlined: libphy's own eFuse read is one, and the off-board control replaces both with a
 * stub, which a call the compiler folded away would escape.
 */
__attribute__((noinline)) void esp_phy_efuse_get_mac(uint8_t *mac)
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
#define FECOEX_FREQ_SET_CONF    (MODEM_BASE + 0xC0u)
#define FECOEX_FREQ_SET_STATUS  (MODEM_BASE + 0xCCu)
#define FECOEX_FREQ_SET_ENABLE  0x04000000u /* the setting's enable, its bit 26 */
#define FECOEX_FREQ_SET_BUSY    (1u << 8)   /* clear once the setting has settled */
#define FECOEX_FREQ_SET_WAIT_US 0x3e8u      /* of the MAC's microsecond counter, the wait's bound */
#define FECTRL_TXRX_OFF         (MODEM_BASE + 0x910u)
#define FECTRL_TXRX_FORCE_MASK  0xfffff0ffu /* bits 8 to 11, the forcing's, which each step keeps off */

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

/* The ROM's rom_i2c_readReg_Mask, block, host, register, msb, lsb, through its table's slot. */
static uint8_t rom_i2c_read_masked(uint8_t block, uint8_t host, uint8_t reg, uint8_t msb, uint8_t lsb)
{
    return (uint8_t)((int (*)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_READ_MASK])(
        block, host, reg, msb, lsb);
}

/* libphy's force_txrx_off: the front end's TX and RX forced off, its bits 8 to 11, in two steps with a 1 us wait. */
void force_txrx_off(uint32_t on)
{
    uint32_t v = REG(FECTRL_TXRX_OFF) & FECTRL_TXRX_FORCE_MASK;

    if (on != 0u) {
        REG(FECTRL_TXRX_OFF) = v | 0x800u;
        ets_delay_us(1);
        REG(FECTRL_TXRX_OFF) = (REG(FECTRL_TXRX_OFF) & FECTRL_TXRX_FORCE_MASK) | 0xa00u;
    } else {
        REG(FECTRL_TXRX_OFF) = v | 0x200u;
        ets_delay_us(1);
        REG(FECTRL_TXRX_OFF) = REG(FECTRL_TXRX_OFF) & FECTRL_TXRX_FORCE_MASK;
    }
    ets_delay_us(1);
}

/* libphy's phy_dis_hw_set_freq_reg: the frequency setting's enable, then a 2 us wait. */
static void phy_dis_hw_set_freq_reg(void)
{
    REG(FECOEX_FREQ_SET_CONF) |= FECOEX_FREQ_SET_ENABLE;
    ets_delay_us(2);
}

/*
 * libphy's phy_wait_freq_set_busy_new [0x42001ef8]: wait, up to 0x3e8 of the MAC's microseconds,
 * for the frequency setting to settle; past that the ROM resets the frequency module, as the library does,
 * whose print of the timeout is left out.
 */
uint32_t phy_wait_freq_set_busy_new(void)
{
    uint32_t start = REG(MAC_TSF_CLOCK);
    uint32_t waited = 0;

    while ((REG(FECOEX_FREQ_SET_STATUS) & FECOEX_FREQ_SET_BUSY) == 0u) {
        waited = 1;
        if (REG(MAC_TSF_CLOCK) - start > FECOEX_FREQ_SET_WAIT_US) {
            freq_module_resetn();
            break;
        }
    }
    return waited;
}

/* libphy's phy_dis_hw_set_freq_1: the wait for the frequency setting, then its enable. */
static void phy_dis_hw_set_freq_1(void)
{
    phy_wait_freq_set_busy_new();
    phy_dis_hw_set_freq_reg();
}

/* libphy's bias_reg_reset: three masked writes to the analog bias block 0x6a, host 1. */
static void bias_reg_reset(void)
{
    rom_i2c_write_masked(I2C_BIAS, I2C_HOST_MST, 0, 3, 0, 7);
    rom_i2c_write_masked(I2C_BIAS, I2C_HOST_MST, 1, 3, 0, 7);
    rom_i2c_write_masked(I2C_BIAS, I2C_HOST_MST, 0, 7, 4, 7);
}

/*
 * The temperature sensor's read and its calibration, the driver's own, [0x4200831c]..[0x420084d6]:
 * the sensor's DAC code names one of five temperature ranges, each with a DAC code and an offset for the ROM,
 * the sensor's own code is turned into a temperature, and a temperature outside its range's bounds corrects
 * the DAC. ram_tsens_temp_read_new answers the temperature, phy_get_tsens_value is its other name,
 * tsens_temp_read_new keeps it in phy_param, and get_temp_init_new hands it to the calibration blocks that
 * start from it. phy_set_tsens_range, also the library's, is left out: nothing calls it.
 */

/* libphy's tsens_dac_to_index: the sensor's DAC code names one of the five entries its calibration writes,
 * and a code the library does not know names none. */
uint32_t tsens_dac_to_index(uint32_t dac)
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
uint32_t tsens_dac_cal_new(int32_t temp, uint32_t index)
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
    rom_i2c_write_masked(I2C_PERIF, I2C_HOST_SELF, 6, 3, 0, (uint8_t)dac);
    return dac;
}

/*
 * libphy's ram_tsens_temp_read_new: the sensor's block 0x69, host 0, register 6 read through the ROM's table
 * for its DAC code, that code's index kept in phy_param, the sensor's code turned to a temperature by the ROM's
 * code_to_temp with the entry's offset, the DAC corrected where the temperature lies outside its entry's range,
 * and the temperature answered. Its phy_get_tsens_value is the library's other name for it.
 */
int32_t ram_tsens_temp_read_new(void)
{
    uint32_t dac =
        (uint32_t)((int (*)(uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_READ_REG])(I2C_PERIF, I2C_HOST_SELF, 6) & 0xfu;
    uint32_t index = tsens_dac_to_index(dac);
    uint32_t code;
    int32_t temp;

    phy_param[PHY_PARAM_TSENS_INDEX] = (uint8_t)index;
    code = tsens_code_read();
    temp = code_to_temp(code, index < 5u ? tsens_attribute[index].offset : 0);
    tsens_dac_cal_new(temp, index);
    return temp;
}

/* libphy's phy_get_tsens_value, a weak name for ram_tsens_temp_read_new. */
int32_t phy_get_tsens_value(void)
{
    return ram_tsens_temp_read_new();
}

/* libphy's tsens_temp_read_new: the temperature read and kept in phy_param's temp_code, a halfword. */
void tsens_temp_read_new(void)
{
    int32_t temp = phy_get_tsens_value();

    phy_param[PHY_PARAM_TEMP_CODE] = (uint8_t)temp;
    phy_param[PHY_PARAM_TEMP_CODE + 1u] = (uint8_t)((uint32_t)temp >> 8);
}

/*
 * libphy's get_temp_init_new: the temperature read, then the two calibration blocks whose temperature it starts.
 * The transmit calibration's, where the caller asks: temp_code_init and its own kept temperature both take the
 * read temperature. The RFPLL capacitance tracking's, where the caller asks: its last temperature takes the read
 * one. The transmit power tracking's last temperature, phy_param 0x04, always takes temp_code_init.
 * register_chipv7_phy asks for both while parameter flag bits 5 and 20 are clear, and so starts the blocks it
 * has not run; the flag's bit 20 is read there alone and no step of the library sets it.
 */
void get_temp_init_new(uint32_t cap_temp_init, uint32_t txcal_temp_init)
{
    int16_t temp;

    tsens_temp_read_new();
    if (txcal_temp_init != 0u) {
        temp = (int16_t)((uint16_t)phy_param[PHY_PARAM_TEMP_CODE] |
                         ((uint16_t)phy_param[PHY_PARAM_TEMP_CODE + 1u] << 8));
        phy_param[PHY_PARAM_TEMP_CODE_INIT] = (uint8_t)temp;
        phy_param[PHY_PARAM_TEMP_CODE_INIT + 1u] = (uint8_t)((uint16_t)temp >> 8);
        phy_param[PHY_PARAM_TXCAL_TEMP] = (uint8_t)temp;
        phy_param[PHY_PARAM_TXCAL_TEMP + 1u] = (uint8_t)((uint16_t)temp >> 8);
    }
    phy_param[PHY_PARAM_TRACK_TEMP_AT] = phy_param[PHY_PARAM_TEMP_CODE_INIT];
    phy_param[PHY_PARAM_TRACK_TEMP_AT + 1u] = phy_param[PHY_PARAM_TEMP_CODE_INIT + 1u];
    if (cap_temp_init != 0u) {
        phy_param[PHY_PARAM_CAP_TEMP] = phy_param[PHY_PARAM_TEMP_CODE];
        phy_param[PHY_PARAM_CAP_TEMP + 1u] = phy_param[PHY_PARAM_TEMP_CODE + 1u];
    }
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
/* The baseband's word 0x702C, one address with several fields: set_rx_comp_new writes its low byte below,
 * phy_force_rx_gain_trig and set_rx_gain_table its higher bits; libphy also calls it AGC_2 there. One name. */
#define AGC_2 (MODEM_BASE + 0x702Cu)
#define AGC_RX_COMP1 (MODEM_BASE + 0x70A0u)

void set_rx_comp_new(void)
{
    REG(AGC_2) = (REG(AGC_2) & ~0xffu) | 0xe9u;
    REG(AGC_RX_COMP1) = (REG(AGC_RX_COMP1) & 0x00ffffffu) | 0xe9000000u;
}

/* libphy's write_gain_mem_new [0x42005004]: three words of the front end's gain memory, then its fourth. */
#define FECTRL_GAIN_MEM0     (MODEM_BASE + 0x8CCu)
#define FECTRL_GAIN_MEM1     (MODEM_BASE + 0x8D0u)
#define FECTRL_GAIN_MEM2     (MODEM_BASE + 0x8D4u)
#define FECTRL_GAIN_MEM_CONF (MODEM_BASE + 0x8C8u)

void write_gain_mem_new(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    REG(FECTRL_GAIN_MEM0) = a;
    REG(FECTRL_GAIN_MEM1) = b;
    REG(FECTRL_GAIN_MEM2) = c;
    REG(FECTRL_GAIN_MEM_CONF) = (REG(FECTRL_GAIN_MEM_CONF) & 0xfff00000u) | ((d << 11) + 0x80000u);
}

/*
 * libphy's phy_i2c_master_mem_txcap [0x42002e5e]:
 * the analog I2C's block 0x6b, host 1, register 2 read through the ROM's table, its byte above the constant.
 */
#define I2C_ANA_MST_TXCAP (MODEM_BASE + 0xFC04u)

void phy_i2c_master_mem_txcap(void)
{
    uint32_t v =
        (uint32_t)((int (*)(uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_READ_REG])(I2C_TXRF, I2C_HOST_MST, 2);

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
    temp = (int16_t)((uint16_t)phy_param[PHY_PARAM_TEMP_CODE] |
                     ((uint16_t)phy_param[PHY_PARAM_TEMP_CODE + 1u] << 8));
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
    rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 7, 3, 0, (uint8_t)data);
}

/*
 * The transmit gain chain of chip_v7_set_chan_misc, the driver's own:
 * wifi_get_tx_tab builds the per-subband gains, wifi_get_tx_gain_new walks them, and get_tx_gain_value_new
 * is the search itself. The two tables are libphy's rodata at 0x4200adbc and 0x4200addc, and the middle
 * buffer is the library's own for wifi_get_tx_gain_new: 0x80 at each end of sixteen words.
 */
#define PHY_PARAM_MOST_TPW     0x06 /* the byte phy_set_most_tpw keeps */
#define PHY_PARAM_TX_GAIN_B    0x08

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
    wifi_get_tx_gain_new(chan, &phy_param[PHY_PARAM_WIFI_PWCTRL_ATTEN], (int)(int8_t)phy_param[PHY_PARAM_WIFI_CAL_POWER],
                         (int)(int8_t)(uint8_t)(phy_param[PHY_PARAM_WIFI_CORRECT_POWER] - phy_param[PHY_PARAM_TX_GAIN_B]),
                         tx_gain_tab, tx_gain_mid, tx_gain_offset, out8, out9, out10, (uint8_t)power);
}

/* The channel's little-endian word of phy_param, which phy_set_most_tpw reads its channel from. */
static uint16_t phy_param16(uint32_t off)
{
    return (uint16_t)((uint16_t)phy_param[off] | ((uint16_t)phy_param[off + 1u] << 8));
}

static void phy_param16_set(uint32_t off, uint16_t v); /* phy_param's halfword store, defined below */

/* The front end's gain memory channel, and its table base in phy_param, which set_tx_gain_mem reads. */
#define FEDATA_GAIN_CHAN       (MODEM_BASE + 0x410u)
#define PHY_PARAM_TX_GAIN_OFF 0x07 /* nonzero leaves set_tx_gain_mem out */

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
    uint32_t base = ((p1 != 0u ? 0x20u : 0u) + (REG(FEDATA_GAIN_CHAN) >> 24)) & 0xffu;
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
        set_tx_gain_mem(0, 0x20u, out10, out9, out8, cfg, &phy_param[PHY_PARAM_TXIQ]);
    }
}

/*
 * libphy's phy_set_most_tpw [0x4200164a]: the byte kept in phy_param's 0x06, then the channel's transmit gain.
 */
#define PHY_PARAM_CHAN14_TPW 0x24

void phy_set_most_tpw(uint8_t tpw)
{
    phy_param[PHY_PARAM_MOST_TPW] = tpw;
    wifi_set_tx_gain((uint32_t)phy_param16(PHY_PARAM_CHANNEL), 0u); /* phy_param's channel, the halfword chip_v7_set_chan writes */
}

/*
 * libphy's chan14_mic_cfg [0x42000550]: channel 14's microphone configuration, the baseband's bits 13 and 14,
 * and the byte phy_param keeps with each: 0x24 for channel 14 and 0x06 otherwise.
 */
#define BBTX_CHAN14 (MODEM_BASE + 0x7400u) /* channel 14's bits 13-14; the library also names this word INIT_0 */

void chan14_mic_cfg(uint32_t on)
{
    if (on == 1u) {
        REG(BBTX_CHAN14) = (REG(BBTX_CHAN14) & ~0x6000u) | 0x2000u;
        phy_set_most_tpw((uint8_t)(int8_t)phy_param[PHY_PARAM_CHAN14_TPW]);
    } else {
        REG(BBTX_CHAN14) |= 0x6000u;
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
/* The ROM's channel into the analog front end, which the receive DC calibration sets. */
void chip_v7_set_chan_ana(uint32_t chan);

#define PHY_PARAM_RFRX_B 0x16e

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
            w2 = phy_param16(PHY_PARAM_RXIQ);
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
#define BRX_LOW_RATE0      (MODEM_BASE + 0x8060u)
#define BRX_LOW_RATE1      (MODEM_BASE + 0x807Cu)
#define BRX_LOW_RATE_BIT10 (1u << 10)
#define BRX_LOW_RATE_BIT11 (1u << 11)

void drv_phy_low_rate_disable(void)
{
    REG(BRX_LOW_RATE0) &= ~BRX_LOW_RATE_BIT10;
    REG(BRX_LOW_RATE0) &= ~BRX_LOW_RATE_BIT11;
    REG(BRX_LOW_RATE1) &= ~BRX_LOW_RATE_BIT11;
}

/*
 * The driver's own of libphy's power path, rate_to_index through phy_get_max_pwr, taken from its disassembly
 * and named for the driver; every byte is libphy's own arithmetic over phy_param, and none is the driver's invention.
 * Its result is held to the library's in one run before the driver's takes the library's place.
 */
#define PHY_PARAM_TX_CAP   0x6    /* the cap every rate's power is held to, signed */
#define PHY_PARAM_SUBBAND  0x50   /* above 9 the second byte is the first's own, not the table's */
#define PHY_PARAM_FCC_CHAN 0x66   /* 1 asks for the four band limits of rate_fcc_index below */
#define PHY_PARAM_FCC_A    0x70   /* rate_fcc_index's first table, read at chan-1, chan+0xd and chan+0x1b */
#define PHY_PARAM_FCC_B    0x67   /* its second, one more limit */

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
    if ((REG(BRX_LOW_RATE0) & BRX_LOW_RATE_BIT10) != 0u) {
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
                          &phy_param[PHY_PARAM_WIFI_INIT_POWER], phy_param[PHY_PARAM_FCC_CHAN],
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
#define AGC_11P (MODEM_BASE + 0x7030u)

static void phy_11p_set(uint32_t p1, uint32_t p2)
{
    phy_param[0x28] = (uint8_t)p1;
    phy_param[0x29] = (uint8_t)p2;
    if (p1 == 0u) {
        REG(MODEM_SYSCON_WIFI_BB_CFG) &= ~0x1cu;
        REG(AGC_11P) |= 0x20u;
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 4, phy_param[PHY_PARAM_RC_DOUT_WIFI]);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 5, phy_param[PHY_PARAM_RC_DOUT_WIFI]);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 6, phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u]);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 7, phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u]);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0xc, phy_param[PHY_PARAM_RC_DOUT_WIFI]);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0xd, phy_param[PHY_PARAM_RC_DOUT_WIFI]);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0xe, phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u]);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0xf, phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u]);
    } else {
        uint32_t k = p2 == 0u ? 4u : 5u;
        uint32_t div = p2 == 0u ? 100u : 0x32u;
        int32_t v = (int32_t)(((uint32_t)(phy_param[PHY_PARAM_RC_DOUT] + 0x38u) * 0x67u) / div) - 8;
        uint8_t d;

        REG(MODEM_SYSCON_WIFI_BB_CFG) = (REG(MODEM_SYSCON_WIFI_BB_CFG) & ~0x1cu) | (k << 2);
        REG(AGC_11P) &= ~0x20u;
        if (v > 0x3f) {
            v = 0x3f;
        }
        d = (uint8_t)v;
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 4, d);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 5, d);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 6, d);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 7, d);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0xc, d);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0xd, d);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0xe, d);
        rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0xf, d);
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
    phy_param[PHY_PARAM_CHANNEL_SECOND] = (uint8_t)second;
    phy_param[0x11e] = second != 0u ? 1u : 0u;
    phy_param[PHY_PARAM_CHANNEL] = (uint8_t)ch;
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
#define I2C_MST_CLK0 (MODEM_LPCON_BASE + 0x824u)
#define I2C_MST_CLK1 (MODEM_LPCON_BASE + 0x828u)
#define I2C_MST_CLK2 (MODEM_LPCON_BASE + 0x82Cu)

/*
 * libphy's i2c_clk_sel [0x42002678]: each of the three words takes the selector's bits, two read-modify-writes
 * each as the library writes it, bits 6 to 10 from the selector's bits 2 to 6 and bits 0 to 5 from its 1 to 6.
 */
void i2c_clk_sel(uint32_t sel)
{
    uint32_t hi = (sel << 4) & 0x7c0u;
    uint32_t lo = (sel >> 1) & 0x3fu;

    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t reg = I2C_MST_CLK0 + i * 4u;

        REG(reg) = (REG(reg) & ~0x7c0u) | hi;
        REG(reg) = (REG(reg) & ~0x3fu) | lo;
    }
}

/* The register i2c_bbpll_set keeps the byte it reads of block I2C_BBPLL. */
#define PHY_PARAM_BBPLL_BYTE  0x4a

/*
 * libphy's i2c_bbpll_set [0x42002ae4]: powering the baseband PLL up takes the byte of block 0x66, host 0,
 * register 4 above bit 2 and keeps it in phy_param as it answers; powering it down writes that byte back.
 */
void i2c_bbpll_set(int on)
{
    if (on != 0) {
        rom_i2c_write_masked(I2C_BBPLL, I2C_HOST_SELF, 4, 3, 2, 2);
        phy_param[PHY_PARAM_BBPLL_BYTE] = rom_i2c_read(I2C_BBPLL, I2C_HOST_SELF, 4);
    } else {
        rom_i2c_write(I2C_BBPLL, I2C_HOST_SELF, 4, phy_param[PHY_PARAM_BBPLL_BYTE]);
    }
}

/* The front end's and the baseband's words the library's reg_set_new writes, unnamed in any header. */
#define FECTRL_REG_SET0     (MODEM_BASE + 0x8F8u)
#define FEDATA_REG_SET1     (MODEM_BASE + 0x44Cu)
#define FEDATA_REG_SET2     (MODEM_BASE + 0x46Cu)
#define FEDATA_REG_SET3     (MODEM_BASE + 0x450u)
#define BBTX_REG_SET (MODEM_BASE + 0x7428u)

/* libphy's reg_set_new [0x420030ea]: five read-modify-writes bringing the front end and the baseband up. */
void reg_set_new(void)
{
    REG(FECTRL_REG_SET0) = (REG(FECTRL_REG_SET0) & ~0xffu) | 4u;
    REG(FEDATA_REG_SET1) |= 0x08000000u;
    REG(FEDATA_REG_SET2) &= ~0x20000000u;
    REG(FEDATA_REG_SET3) |= 0x80008000u;
    REG(BBTX_REG_SET) = (REG(BBTX_REG_SET) & ~0x3f00u) | 0x1500u;
}

/* The frequency module's own two words the library's freq_reg_init writes, unnamed in any header. */
#define FECOEX_FREQ_SET_CONF2 (MODEM_BASE + 0xC8u)
#define FECOEX_FREQ_SET_CONF4 (MODEM_BASE + 0xD4u)

/* phy_param's byte the library's freq_reg_init reads for its default. */
#define PHY_PARAM_FREQ_SEL 0x19b

/*
 * libphy's freq_reg_init [0x42001c62]: the frequency setting's word cleared and its hold bit set,
 * then the two parameters at bits 17 to 24, with phy_param 0x19b naming the default pair, then the
 * module's own three words -- a 2-bit field, a 0x19800249 and a 0x25824e50, which is the setting's status word.
 */
void freq_reg_init(uint32_t param1, uint32_t param2)
{
    REG(FECOEX_FREQ_SET_CONF) &= 0x5bffbfffu;
    REG(FECOEX_FREQ_SET_CONF) |= 0x02000000u;
    if (phy_param[PHY_PARAM_FREQ_SEL] != 0u) {
        param1 = 0u;
        param2 = 2u;
    }
    REG(FECOEX_FREQ_SET_CONF) = (REG(FECOEX_FREQ_SET_CONF) & 0xfe01ffffu) |
                            ((((param2 << 4) | param1) << 17) & 0x01fe0000u);
    REG(FECOEX_FREQ_SET_CONF4) = (REG(FECOEX_FREQ_SET_CONF4) & 0xfffff87fu) | 0x300u;
    REG(FECOEX_FREQ_SET_CONF2) = 0x19800249u;
    REG(FECOEX_FREQ_SET_STATUS) = 0x25824e50u;
}

/*
 * libphy's bias_reg_set [0x42002cc2], whose argument it does not read: the analog bias block 0x6a, host 1,
 * three masked writes, 0xf to register 0 and 0xc to registers 1 and 0's high nibble.
 */
void bias_reg_set(int on)
{
    (void)on;
    rom_i2c_write_masked(I2C_BIAS, I2C_HOST_MST, 0, 3, 0, 0x0f);
    rom_i2c_write_masked(I2C_BIAS, I2C_HOST_MST, 1, 3, 0, 0x0c);
    rom_i2c_write_masked(I2C_BIAS, I2C_HOST_MST, 0, 7, 4, 0x0c);
}

/* The baseband's noise-floor word, which the library's noise_floor_auto_set_new sets. */
#define AGC_NOISE_FLOOR (MODEM_BASE + 0x7018u)

/* libphy's noise_floor_auto_set_new [0x42004f46]: two bits of the baseband's noise-floor word, set in order. */
void noise_floor_auto_set_new(void)
{
    REG(AGC_NOISE_FLOOR) |= 0x00800000u;
    REG(AGC_NOISE_FLOOR) |= 0x10000000u;
}

/* The front end's Bluetooth filter word, which the library's bt_filter_reg_new sets. */
#define FECTRL_BT_FILTER (MODEM_BASE + 0x900u)

/* libphy's bt_filter_reg_new [0x42004f60]: three read-modify-writes of the front end's filter word. */
void bt_filter_reg_new(void)
{
    REG(FECTRL_BT_FILTER) |= 0x02000000u;
    REG(FECTRL_BT_FILTER) &= 0xffbfffffu;
    REG(FECTRL_BT_FILTER) &= 0xfe7fffffu;
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

/*
 * The pbus's six force words in FECTRL, 0x8E0 to 0x8F4; no header names them and only the library's
 * save_pbus_reg and write_pbus_mem touch them, which read and write them without saying what they mean.
 */
#define FECTRL_PBUS_FORCE0 (MODEM_BASE + 0x8E0u)
#define FECTRL_PBUS_FORCE1 (MODEM_BASE + 0x8E4u)
#define FECTRL_PBUS_FORCE2 (MODEM_BASE + 0x8E8u)
#define FECTRL_PBUS_FORCE3 (MODEM_BASE + 0x8ECu)
#define FECTRL_PBUS_FORCE4 (MODEM_BASE + 0x8F0u)
#define FECTRL_PBUS_FORCE5 (MODEM_BASE + 0x8F4u)

/* phy_param's copy of them, which the library's save_pbus_reg keeps; no label names the six words. */
#define PHY_PARAM_PBUS_FORCE 0x30

static void phy_param32_set(uint32_t off, uint32_t v); /* phy_param's word store, defined below */
static uint32_t phy_param32(uint32_t off);             /* phy_param's word load, defined below */

/*
 * libphy's save_pbus_reg [0x42004516]: the six force words copied into phy_param, word for word, in the
 * order write_pbus_mem programs them.
 */
void save_pbus_reg(void)
{
    phy_param32_set(PHY_PARAM_PBUS_FORCE + 0x00u, REG(FECTRL_PBUS_FORCE0));
    phy_param32_set(PHY_PARAM_PBUS_FORCE + 0x04u, REG(FECTRL_PBUS_FORCE1));
    phy_param32_set(PHY_PARAM_PBUS_FORCE + 0x08u, REG(FECTRL_PBUS_FORCE2));
    phy_param32_set(PHY_PARAM_PBUS_FORCE + 0x0cu, REG(FECTRL_PBUS_FORCE3));
    phy_param32_set(PHY_PARAM_PBUS_FORCE + 0x10u, REG(FECTRL_PBUS_FORCE4));
    phy_param32_set(PHY_PARAM_PBUS_FORCE + 0x14u, REG(FECTRL_PBUS_FORCE5));
}

/*
 * libphy's write_pbus_mem [0x42004548]: twelve word arrays written into the front end's gain memory, one word at
 * a time through FECTRL_GAIN_MEM0 0x8cc under a countdown in FECTRL_GAIN_MEM_CONF 0x8c8, and, once per array,
 * the range of entries it covers forced into one half of one of the six pbus words 0x8e0 to 0x8f4. The arrays'
 * lengths are 8, 5, 8, 5, 3, 1 twice over; the first and second array of each word take its low and high half.
 * The library's own is the only source for any of it, so the names stay by register.
 */
void write_pbus_mem(const uint32_t *p0, const uint32_t *p1, const uint32_t *p2, const uint32_t *p3,
                    const uint32_t *p4, const uint32_t *p5, const uint32_t *p6, const uint32_t *p7,
                    const uint32_t *p8, const uint32_t *p9, const uint32_t *p10, const uint32_t *p11)
{
    const uint32_t *buf[12] = { p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11 };
    static const uint8_t count[12] = { 8, 5, 8, 5, 3, 1, 8, 5, 8, 5, 3, 1 };
    static const uint32_t force[12] = {
        FECTRL_PBUS_FORCE0, FECTRL_PBUS_FORCE0, FECTRL_PBUS_FORCE1, FECTRL_PBUS_FORCE1,
        FECTRL_PBUS_FORCE2, FECTRL_PBUS_FORCE2, FECTRL_PBUS_FORCE3, FECTRL_PBUS_FORCE3,
        FECTRL_PBUS_FORCE4, FECTRL_PBUS_FORCE4, FECTRL_PBUS_FORCE5, FECTRL_PBUS_FORCE5,
    };
    static const uint8_t half[12] = { 0, 16, 0, 16, 0, 16, 0, 16, 0, 16, 0, 16 };
    uint32_t at = 0;

    for (uint32_t i = 0; i < 12u; i++) {
        uint32_t n = count[i];
        uint32_t field = (((at + n - 1u) << 8) | at) & 0xffffu;
        uint32_t fill = (at + 0x200u) << 11;

        REG(force[i]) = (REG(force[i]) & ~(0xffffu << half[i])) | (field << half[i]);
        for (uint32_t k = 0; k < n; k++) {
            REG(FECTRL_GAIN_MEM0) = buf[i][k];
            REG(FECTRL_GAIN_MEM_CONF) = (REG(FECTRL_GAIN_MEM_CONF) & 0xffe007ffu) | (fill & 0x1ff800u);
            fill += 0x800u;
        }
        at += n;
    }
}

/*
 * libphy's own tables, from its rodata, its .LANCHOR0 at 0x4200acb8, which its set_pbus_mem copies onto its
 * stack; each word is two 16-bit pbus entries. No header names them, so the names stay by where the library
 * reads them.
 */
static const uint32_t pbus_tab1[5] = { 0x000401ffu, 0x000801ffu, 0x001401ffu, 0x001801ffu, 0x004801ffu };
static const uint32_t pbus_tab2[8] = { 0x000501ffu, 0x0014f9ffu, 0x004801ffu, 0x004417ffu,
                                       0x00f00000u, 0x00f10000u, 0x00f20000u, 0x00f40000u };
static const uint32_t pbus_tab3[5] = { 0x000401ffu, 0x004801ffu, 0x001401ffu, 0x001801ffu, 0x004401ffu };
static const uint32_t pbus_tab4[3] = { 0x0014f9ffu, 0x005781ffu, 0x00f30000u };

/*
 * libphy's set_pbus_mem [0x420046b2]: the pbus's twelve word arrays, built from the library's tables and the two
 * phy_param bytes 2 and 0x14, handed to write_pbus_mem and then saved. Six arrays are handed twice, the second
 * half copied from the first; two of the copies have a word put back by hand -- 0x1717ff where the first has
 * 0x1713ff, and the second's 0x14f9ff and 0x4801ff replaced from phy_param[0x14] -- and the last pair's third
 * array alone differs, the library's table 4 beside 0x14fdff, 0x5781ff and 0x0f30000. Its caller is the
 * library's bb_init, which this file's own bb_init will take over.
 */
void set_pbus_mem(void)
{
    uint32_t front[8] = { ((uint32_t)phy_param[2] << 9) | 0x801ffu, 0x4401ffu, 0x1713ffu, 0x4803ffu,
                          0x5401ffu, 0x0487ffu, 0x0f50000u, 0x0f60000u };
    uint32_t front_again[8] = { ((uint32_t)phy_param[2] << 9) | 0x801ffu, 0x4401ffu, 0x1717ffu, 0x4803ffu,
                                0x5401ffu, 0x0487ffu, 0x0f50000u, 0x0f60000u };
    uint32_t back_again[8] = { 0x000501ffu, 0x0014fdffu, ((uint32_t)phy_param[0x14] << 12) | 0x4801ffu,
                               0x004417ffu, 0x00f00000u, 0x00f10000u, 0x00f20000u, 0x00f40000u };
    uint32_t tail[3] = { 0x0014fdffu, 0x005781ffu, 0x00f30000u };
    uint32_t single[1] = { 0x5401ffu };

    write_pbus_mem(front, pbus_tab1, pbus_tab2, pbus_tab3, pbus_tab4, single, front_again, pbus_tab1,
                   back_again, pbus_tab3, tail, single);
    save_pbus_reg();
}

/* The analog I2C master's parallel-write word in MODEM_LPCON, which the library's phy_i2c_init2 sets. */
#define I2C_MST_MST0 (MODEM_LPCON_BASE + 0x820u)

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
    uint8_t e5[7] = { 0x68, 0x28, phy_param[PHY_PARAM_I2C_RFPLL_0F], 0x44, 0x26, 0x26, 0x26 };
    uint32_t mask;

    ((void (*)(void))phy_get_romfuncs()[ROMFUNC_I2C_ENTER_CRITICAL])();
    mask = ((uint32_t (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_I2C_MST0_MASK])(0x6bu);
    REG(I2C_MST_MST0) = (REG(I2C_MST_MST0) & 0xfffe000fu) | (mask & 0xfff0u);
    i2c_paral_write_num(e0, e1, e2, e3, e4, e5, 7u, 0u);
    REG(I2C_MST_MST0) = (REG(I2C_MST_MST0) & 0xfffe000fu) | 0x1f00u;
    ((void (*)(void))phy_get_romfuncs()[ROMFUNC_I2C_EXIT_CRITICAL])();
}

/*
 * libphy's filter_dcap_set [0x420026e0]: eighteen writes of the analog block 0x67, host 1, from the
 * calibration's bytes in phy_param -- 0xed at 0x14, 0x15, 0x18, 0x19; 0xee at 0x16, 0x17; 0xf0 at 0x1c
 * to 0x1f, its top bit set at 0x1e; 0xe9 at 4, 5, 0xc, 0xd; 0xea at 6, 7, 0xe, 0xf.
 */
void filter_dcap_set(void)
{
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x14, phy_param[PHY_PARAM_RC_DOUT_BT]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x15, phy_param[PHY_PARAM_RC_DOUT_BT]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x16, phy_param[PHY_PARAM_RC_DOUT_BT + 1u]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x17, phy_param[PHY_PARAM_RC_DOUT_BT + 1u]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x18, phy_param[PHY_PARAM_RC_DOUT_BT]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x19, phy_param[PHY_PARAM_RC_DOUT_BT]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x1c, phy_param[PHY_PARAM_RC_DOUT_BT + 3u]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x1d, phy_param[PHY_PARAM_RC_DOUT_BT + 3u]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x1e, (uint8_t)(phy_param[PHY_PARAM_RC_DOUT_BT + 3u] | 0x40u));
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x1f, phy_param[PHY_PARAM_RC_DOUT_BT + 3u]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x04, phy_param[PHY_PARAM_RC_DOUT_WIFI]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x05, phy_param[PHY_PARAM_RC_DOUT_WIFI]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x06, phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x07, phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x0c, phy_param[PHY_PARAM_RC_DOUT_WIFI]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x0d, phy_param[PHY_PARAM_RC_DOUT_WIFI]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x0e, phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u]);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 0x0f, phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u]);
}

/* The ROM's frequency-memory lookups the library's frequency chain runs. */
void freq_i2c_num_addr(void *buf, uint32_t count);
uint32_t get_freq_mem_param(uint32_t index);

/* The frequency module's memory-write word, which the library's freq_i2c_mem_write_new writes. */
#define FECOEX_FREQ_SET_MEM (MODEM_BASE + 0xD0u)

/*
 * libphy's freq_i2c_mem_write_new [0x420021ba]: the sub-address at the frequency setting's bits 4 to 11,
 * then the data and the length at 0x600a00d0, the setting's own bit 15 pulsed.
 */
void freq_i2c_mem_write_new(uint32_t addr, uint32_t data, uint32_t len)
{
    REG(FECOEX_FREQ_SET_CONF) = (REG(FECOEX_FREQ_SET_CONF) & 0xffffc00fu) | ((addr << 4) & 0x3ff0u);
    REG(FECOEX_FREQ_SET_MEM) = (len << 24) | data;
    REG(FECOEX_FREQ_SET_CONF) |= 0x8000u;
    REG(FECOEX_FREQ_SET_CONF) &= ~0x8000u;
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

    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 0x0b, 6, 6, 1);
    s6 = rom_i2c_read(I2C_RFPLL, I2C_HOST_MST, 0x0b);
    uint32_t v2 = rom_i2c_read(I2C_RFPLL_SDM, I2C_HOST_MST, 0);
    uint32_t v3 = rom_i2c_read(I2C_BBTOP, I2C_HOST_MST, 3);

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
#define AGC_11B_0 (MODEM_BASE + 0x7044u)
#define AGC_11B_1 (MODEM_BASE + 0x7124u)
#define AGC_11B_2 (MODEM_BASE + 0x7104u)
#define BRX_11B_3 (MODEM_BASE + 0x8004u)

/*
 * libphy's rx_11b_opt [0x42004aa4]: the two long-range 11b settings of the baseband and the front end,
 * five read-modify-writes each way, with the last two shared: the baseband's 0x7104 clearing its high bits
 * and setting 0x1c8 either way.
 */
void rx_11b_opt(uint32_t on)
{
    if (on != 0u) {
        REG(AGC_11B_0) |= 0x003f0000u;
        REG(AGC_11B_0) = (REG(AGC_11B_0) & 0xffffc0ffu) | 0x2100u;
        REG(AGC_11B_1) = (REG(AGC_11B_1) & 0xffff03ffu) | 0x8400u;
        REG(AGC_11B_1) = (REG(AGC_11B_1) & ~0x0fu) | 0x3u;
        REG(BRX_11B_3) = (REG(BRX_11B_3) & 0xffff0fffu) | 0x9000u;
    } else {
        REG(AGC_11B_0) = (REG(AGC_11B_0) & 0xffc0ffffu) | 0x003e0000u;
        REG(AGC_11B_0) = (REG(AGC_11B_0) & 0xffffc0ffu) | 0x1800u;
        REG(AGC_11B_1) = (REG(AGC_11B_1) & 0xffff03ffu) | 0x6000u;
        REG(AGC_11B_1) = (REG(AGC_11B_1) & ~0x0fu) | 0x4u;
        REG(BRX_11B_3) = (REG(BRX_11B_3) & 0xffff0fffu) | 0x6000u;
    }
    REG(AGC_11B_2) = (REG(AGC_11B_2) & 0xfffffe00u) | 0x1c8u;
}

/* The front end's and the baseband's words the library's tx_paon_set_new writes. */
#define BB_PAON_0 (MODEM_BASE + 0x7C00u)
#define BB_PAON_1 (MODEM_BASE + 0x7C6Cu)
#define BB_PAON_2 (MODEM_BASE + 0x7C30u)
#define FECTRL_REG_SET4    (MODEM_BASE + 0x8FCu)

/* libphy's tx_paon_set_new [0x42004b68]: the transmit PA's settings in six read-modify-writes. */
void tx_paon_set_new(void)
{
    REG(BB_PAON_0) = (REG(BB_PAON_0) & 0xffe007ffu) | 0x0000a000u;
    REG(FECTRL_REG_SET0) = (REG(FECTRL_REG_SET0) & 0xffff00ffu) | 0x00005000u;
    REG(BB_PAON_1) = 0x0140c81eu;
    REG(BB_PAON_2) = (REG(BB_PAON_2) & ~0x3ffu) | 0x1eu;
    REG(FECTRL_REG_SET4) = (REG(FECTRL_REG_SET4) & 0x0000ffffu) | 0x0a0e0000u;
    REG(FECTRL_REG_SET4) = (REG(FECTRL_REG_SET4) & 0xffff00ffu) | 0x0000c800u;
}

/* The baseband's and the front end's words the library's agc_reg_init_new writes. */
#define AGC_0 (MODEM_BASE + 0x713Cu)
#define AGC_1 (MODEM_BASE + 0x7094u)
#define AGC_3 (MODEM_BASE + 0x7128u)
#define AGC_4 (MODEM_BASE + 0x7068u)
#define FECTRL_AGC_0      (MODEM_BASE + 0x958u)

/*
 * libphy's agc_reg_init_new [0x42004c04]: the automatic gain control's settings from the two calibration
 * bytes, eight read-modify-writes, the last word 0x404 outright.
 */
void agc_reg_init_new(uint32_t param1, uint32_t param2)
{
    uint32_t n = param1 - 1u;

    REG(AGC_0) = (REG(AGC_0) & 0xfe03ffffu) | ((n << 18) & 0x01fc0000u);
    REG(AGC_1) = (REG(AGC_1) & ~0x1fcu) | ((n << 2) & 0x1fcu);
    REG(AGC_2) = (REG(AGC_2) & 0xffff80ffu) | ((param1 << 8) & 0x7f00u);
    REG(FECTRL_AGC_0) = (REG(FECTRL_AGC_0) & 0xfffff00fu) | (param1 << 4);
    REG(FECTRL_AGC_0) = (REG(FECTRL_AGC_0) & 0xfff00fffu) | (((param2 + 0x50u) << 12) & 0x000ff000u);
    REG(AGC_2) = (REG(AGC_2) & 0x00ffffffu) | 0x32000000u;
    REG(AGC_2) |= 0x00800000u;
    REG(AGC_2) &= 0xff7fffffu;
    REG(AGC_3) = (REG(AGC_3) & 0x00ffffffu) | 0xd2000000u;
    REG(AGC_4) = 0x404u;
}

/* The baseband's and the front end's words the library's bb_reg_init_new writes. */
#define NRX_INIT_1  (MODEM_BASE + 0x7848u)
#define NRX_INIT_2  (MODEM_BASE + 0x7808u)
#define NRX_INIT_3  (MODEM_BASE + 0x78DCu)
#define NRX_INIT_4  (MODEM_BASE + 0x78E4u)
#define NRX_INIT_5  (MODEM_BASE + 0x790Cu)
#define BB_INIT_6  (MODEM_BASE + 0x7CA8u)
#define NRX_INIT_7  (MODEM_BASE + 0x7980u)
#define NRX_INIT_8  (MODEM_BASE + 0x7890u)
#define NRX_INIT_9  (MODEM_BASE + 0x7A28u)
#define BBTX_INIT_10 (MODEM_BASE + 0x7424u)
#define BB_INIT_11 (MODEM_BASE + 0x7CD0u)
#define BBTX_INIT_12 (MODEM_BASE + 0x7438u)

/* libphy's btbb_wifi_bb_cfg2 [0x42004cd0]: two nibbles of the baseband's 0x7cd0 set. */
void btbb_wifi_bb_cfg2(void)
{
    REG(BB_INIT_11) |= 0x000f000bu;
}

/*
 * libphy's bb_reg_init_new [0x42004ce6]: the baseband's and the front end's bring-up, thirteen words,
 * then the transmit and receive filter bits of the frequency setting, ending in btbb_wifi_bb_cfg2 above.
 */
void bb_reg_init_new(void)
{
    REG(BBTX_CHAN14) |= 0x6000u;
    REG(NRX_INIT_1) = (REG(NRX_INIT_1) & 0xff000000u) | 0x000433afu;
    REG(NRX_INIT_1) = (REG(NRX_INIT_1) & 0xe0ffffffu) | 0x17000000u;
    REG(NRX_INIT_2) = (REG(NRX_INIT_2) & 0xffffc07fu) | 0x3000u;
    REG(NRX_INIT_3) = (REG(NRX_INIT_3) & 0xffffc07fu) | 0x100u;
    REG(NRX_INIT_4) = REG(NRX_INIT_4) & 0xffbfffffu;
    REG(BB_PAON_2) = REG(BB_PAON_2) & 0xfff00fffu;
    REG(NRX_INIT_5) = REG(NRX_INIT_5) & 0xfffff7ffu;
    REG(BB_INIT_6) |= 0x00100000u;
    REG(NRX_INIT_7) = REG(NRX_INIT_7) & 0xfdffffffu;
    REG(NRX_INIT_8) = REG(NRX_INIT_8) & 0xfdffffffu;
    REG(NRX_INIT_8) |= 0x01000000u;
    REG(NRX_INIT_9) = REG(NRX_INIT_9) & 0xffbfffffu;
    REG(BBTX_INIT_10) = (REG(BBTX_INIT_10) & 0xe01fffffu) | 0x01000000u;
    REG(BB_INIT_11) |= 0x000f000fu;
    REG(BB_PAON_0) |= 0x200u;
    REG(MODEM_SYSCON_WIFI_BB_CFG) |= 0x800u;
    REG(BBTX_INIT_12) = REG(BBTX_INIT_12) & ~0xc0u;
    REG(BBTX_INIT_12) = REG(BBTX_INIT_12) & ~0x100u;
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
    REG(FECOEX_FREQ_SET_CONF) = (REG(FECOEX_FREQ_SET_CONF) & 0xe7ffffffu) | 0x10000000u;
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
    write_chan_freq(phy_param[PHY_PARAM_CHANNEL]);
    set_pbus_reg();
    phy_reg_init();
    set_chan_reg(1u);
    set_rx_comp_new();
    enable_agc();
    phy_wait_freq_set_busy_new();
    phy_en_hw_set_freq();
    phy_bbpll_cal(0);
    force_txrx_off(0);
    phy_param[PHY_PARAM_RF_OFF] = 0;
    phy_param[PHY_PARAM_TSENS_XPD] = 0;
    REG(FECOEX_FREQ_SET_CONF) &= 0xe7ffffffu;
    REG(MODEM_SYSCON_CLK_CONF1) = clk_conf1;
    if (phy_param[PHY_PARAM_WIFI_BB_ENABLE] != 0u) {
        phy_wifi_enable_set(0);
    }
}

/*
 * libphy's phy_i2c_init1 [0x4200286a]: the analog I2C's blocks the bring-up sets, one register at a time
 * through the ROM's writeReg -- the transmit block 0x6b's registers 2 to 8, the RF PLL 0x62's registers 0, 4,
 * 0x0f and 0x0b, register 0x0f taking the pump's setting rf_init has just read from it -- and the baseband
 * top 0x67's register 2 as its tail. What each value selects is not known.
 */
void phy_i2c_init1(void)
{
    rom_i2c_write(I2C_TXRF, I2C_HOST_MST, 2, 0x72);
    rom_i2c_write(I2C_TXRF, I2C_HOST_MST, 3, 0x3b);
    rom_i2c_write(I2C_TXRF, I2C_HOST_MST, 4, 0xa4);
    rom_i2c_write(I2C_TXRF, I2C_HOST_MST, 5, 0x02);
    rom_i2c_write(I2C_TXRF, I2C_HOST_MST, 6, 0x88);
    rom_i2c_write(I2C_TXRF, I2C_HOST_MST, 7, 0xb8);
    rom_i2c_write(I2C_TXRF, I2C_HOST_MST, 8, 0x81);
    rom_i2c_write(I2C_RFPLL, I2C_HOST_MST, 0x00, 0x68);
    rom_i2c_write(I2C_RFPLL, I2C_HOST_MST, 0x04, 0x28);
    rom_i2c_write(I2C_RFPLL, I2C_HOST_MST, 0x0f, phy_param[PHY_PARAM_I2C_RFPLL_0F]);
    rom_i2c_write(I2C_RFPLL, I2C_HOST_MST, 0x0b, 0x44);
    rom_i2c_write(I2C_BBTOP, I2C_HOST_MST, 2, 0x26);
}

/*
 * libphy's phy_i2c_master_cmd_mem_init [0x42002e8a]: the analog I2C master's command memory, thirty words
 * from 0xfc00, each a command's register pair and its halfword. Eight are the plain defaults; the rest take
 * the RF PLL's charge pump byte from phy_param 0x196 and the RC calibration's bytes 0xe9, 0xea, 0xed, 0xee
 * and 0xf0, the one with 0x40 beside them or-ed in. The word at 0xfc04 is the one
 * phy_i2c_master_mem_txcap writes elsewhere. What the command words say is not known.
 */
#define I2C_ANA_MST_CMD0 (MODEM_BASE + 0xFC00u)

void phy_i2c_master_cmd_mem_init(void)
{
    REG(I2C_ANA_MST_CMD0 + 0x00u) = 0x00060267u;
    REG(I2C_ANA_MST_CMD0 + 0x04u) = 0x0072026bu;
    REG(I2C_ANA_MST_CMD0 + 0x08u) = 0x003b036bu;
    REG(I2C_ANA_MST_CMD0 + 0x0cu) = 0x00a4046bu;
    REG(I2C_ANA_MST_CMD0 + 0x10u) = 0x0002056bu;
    REG(I2C_ANA_MST_CMD0 + 0x14u) = 0x0088066bu;
    REG(I2C_ANA_MST_CMD0 + 0x18u) = 0x00b8076bu;
    REG(I2C_ANA_MST_CMD0 + 0x1cu) = 0x0081086bu;
    REG(I2C_ANA_MST_CMD0 + 0x20u) = 0x00680062u;
    REG(I2C_ANA_MST_CMD0 + 0x24u) = 0x00280462u;
    REG(I2C_ANA_MST_CMD0 + 0x28u) = ((uint32_t)phy_param[PHY_PARAM_I2C_RFPLL_0F] << 16) | 0x0f62u;
    REG(I2C_ANA_MST_CMD0 + 0x2cu) = 0x00260267u;
    REG(I2C_ANA_MST_CMD0 + 0x30u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_WIFI] << 16) | 0x0467u;
    REG(I2C_ANA_MST_CMD0 + 0x34u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_WIFI] << 16) | 0x0567u;
    REG(I2C_ANA_MST_CMD0 + 0x38u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u] << 16) | 0x0667u;
    REG(I2C_ANA_MST_CMD0 + 0x3cu) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u] << 16) | 0x0767u;
    REG(I2C_ANA_MST_CMD0 + 0x40u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_WIFI] << 16) | 0x0c67u;
    REG(I2C_ANA_MST_CMD0 + 0x44u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_WIFI] << 16) | 0x0d67u;
    REG(I2C_ANA_MST_CMD0 + 0x48u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u] << 16) | 0x0e67u;
    REG(I2C_ANA_MST_CMD0 + 0x4cu) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u] << 16) | 0x0f67u;
    REG(I2C_ANA_MST_CMD0 + 0x50u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT] << 16) | 0x1467u;
    REG(I2C_ANA_MST_CMD0 + 0x54u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT] << 16) | 0x1567u;
    REG(I2C_ANA_MST_CMD0 + 0x58u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT + 1u] << 16) | 0x1667u;
    REG(I2C_ANA_MST_CMD0 + 0x5cu) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT + 1u] << 16) | 0x1767u;
    REG(I2C_ANA_MST_CMD0 + 0x60u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT] << 16) | 0x1867u;
    REG(I2C_ANA_MST_CMD0 + 0x64u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT] << 16) | 0x1967u;
    REG(I2C_ANA_MST_CMD0 + 0x68u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT + 3u] << 16) | 0x1c67u;
    REG(I2C_ANA_MST_CMD0 + 0x6cu) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT + 3u] << 16) | 0x1d67u;
    REG(I2C_ANA_MST_CMD0 + 0x70u) = (((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT + 3u] | 0x40u) << 16) | 0x1e67u;
    REG(I2C_ANA_MST_CMD0 + 0x74u) = ((uint32_t)phy_param[PHY_PARAM_RC_DOUT_BT + 3u] << 16) | 0x1f67u;
}

/*
 * libphy's rf_init [0x42003560]: the analog bring-up, which the library's register_chipv7_phy calls between
 * its calibration-data check and its bb_init. It powers the baseband PLL and the analog bias up, opens the
 * analog I2C and clears the pbus, sets the I2C clock and the baseband PLL, brings the I2C master, the power
 * detector, the front end and the baseband registers up, starts the temperature sensor, sets the transmit
 * power's bandgap, runs the RC calibration, sets the filter's DCAP, reads the RF PLL's charge pump word into
 * phy_param 0x196, opens the analog I2C's blocks, calibrates the charge pump, programs the I2C master's
 * command memory, starts the SAR ADC's code where the block's register 4 asks, and ends by tuning the
 * frequency module with its two arguments 2 and 4, which the library leaves in the registers for freq_reg_init.
 */
void rf_init(void)
{
    phy_bbpll_cal(1);
    bias_reg_set(1);
    open_i2c_xpd_new(1);
    pbus_clear_reg();
    i2c_clk_sel(8u);
    i2c_bbpll_set(1);
    i2cmst_reg_init();
    pwdet_reg_init_new();
    fe_reg_init();
    reg_set_new();
    tsens_read_init_new();
    tx_pwctrl_bg_init();
    i2c_rc_cal_set(3u, 1u);
    rc_cal_init();
    filter_dcap_set();
    phy_param[PHY_PARAM_I2C_RFPLL_0F] = rom_i2c_read(I2C_RFPLL, I2C_HOST_MST, 0x0f);
    phy_i2c_init1();
    rfpll_chgp_cal();
    phy_i2c_master_cmd_mem_init();
    if (rom_i2c_read_masked(I2C_PERIF, I2C_HOST_SELF, 4, 3, 0) == 0u) {
        i2c_sar2_init_code(0x578u);
    }
    set_chan_freq_hw_init(2u, 4u);
}

/*
 * libphy's bb_init [0x42003662]: the baseband bring-up, which the library's register_chipv7_phy calls right
 * after rf_init, before the calibration's temperature init. It raises the SAR ADC's power detector drive and
 * the frequency setting's bit 28, and, while param_flag's bit 3 says the baseband calibration has not run,
 * runs the transmit DC and IQ calibrations, the power detector's and the transmit capacitor's, another
 * temperature read, the transmit power and the transmit DC power detector's calibrations, and sets bit 3;
 * then, always, Bluetooth's gain, the pbus program, another temperature read, the receive IQ calibration,
 * the receive table, the gain table for 0x985, the front end's registers, channel 11, and takes the drive
 * and the setting's bit 28 down again, clearing Wi-Fi's baseband where phy_param 0x19e asks.
 * The leaves the driver's own are called directly; the library's remain its own until their packages.
 */
void bb_init(void)
{
    REG(APB_SARADC_CTRL) |= APB_SARADC2_PWDET_DRV;
    REG(FECOEX_FREQ_SET_CONF) = (REG(FECOEX_FREQ_SET_CONF) & 0xe7ffffffu) | 0x10000000u;
    if ((phy_param32(PHY_PARAM_PARAM_FLAG) & 0x8u) == 0u) {
        txdc_cal_init_new((uint16_t *)&phy_param[PHY_PARAM_TXDC], 0xfu, 0u, 0u);
        pwdet_code_cal();
        tx_cap_init_loop();
        tsens_temp_read_new();
        tx_pwctrl_init_new(0u);
        txdc_cal_pwdet_init(1u, 0u);
        txiq_cal_init_new();
        phy_param32_set(PHY_PARAM_PARAM_FLAG, phy_param32(PHY_PARAM_PARAM_FLAG) | 0x8u);
    }
    bt_tx_gain_init();
    set_pbus_mem();
    tsens_temp_read_new();
    rxiq_cal_init(0u, &phy_param[PHY_PARAM_PARAM_FLAG], 0u);
    phy_rx_table_init();
    rfrx_sat_rst(0u);
    phy_check_rx_sat();
    set_rx_gain_table(0x985u, 0u);
    rfrx_sat_rst(1u);
    phy_reg_init();
    chip_v7_set_chan(0xbu, 0u);
    REG(FECOEX_FREQ_SET_CONF) &= 0xe7ffffffu;
    REG(APB_SARADC_CTRL) &= ~APB_SARADC2_PWDET_DRV;
    if (phy_param[PHY_PARAM_WIFI_BB_ENABLE] != 0u) {
        phy_wifi_enable_set(0);
    }
}

/* The frequency setting's trigger and its answered word, which read_rf_freq_mem_new uses, unnamed in any header. */
#define FECOEX_FREQ_SET_TRIG (MODEM_BASE + 0xC4u)
#define FECOEX_FREQ_SET_DATA (MODEM_BASE + 0xE4u)

/*
 * libphy's read_rf_freq_mem_new [0x42002174]: the sub-address at the setting's bits 4 to 11 and a two-bit
 * field at its own, then its trigger pulsed and the word it answered read.
 */
uint32_t read_rf_freq_mem_new(uint32_t addr, uint32_t field)
{
    REG(FECOEX_FREQ_SET_CONF) = (REG(FECOEX_FREQ_SET_CONF) & 0xffffc00fu) | ((addr << 4) & 0x3ff0u);
    REG(FECOEX_FREQ_SET_CONF4) = (REG(FECOEX_FREQ_SET_CONF4) & ~0x3u) | (field & 0x3u);
    REG(FECOEX_FREQ_SET_TRIG) |= 0x1u;
    REG(FECOEX_FREQ_SET_TRIG) &= ~0x1u;
    return REG(FECOEX_FREQ_SET_DATA);
}

/* The ROM's get_freq_mem_addr: the block, host, index and flag of a frequency memory word, answering its address. */
uint32_t get_freq_mem_addr(uint32_t block, uint32_t host, uint32_t index, uint32_t flag);

/*
 * libphy's pll_cap_mem_update_new [0x4200238c]: the 0x55 capacitance words of the frequency memory's block,
 * each read back through read_rf_freq_mem_new and corrected by the argument delta, written again;
 * then the frequency setting's sub-band word from the current channel's frequency less 2400 MHz.
 */
void pll_cap_mem_update_new(int32_t delta)
{
    uint32_t ret = get_freq_mem_param(2u);
    uint32_t high = (ret >> 16) & 0xffu;
    uint32_t mid = (ret >> 8) & 0xffu;

    for (uint32_t k = 0; k < 0x55u; k++) {
        uint32_t addr = get_freq_mem_addr(high, mid, k, 0u);
        uint32_t v = read_rf_freq_mem_new(addr, 2u);
        uint32_t t = ((((v >> 4) & 0x100u) | (v & 0xffu)) + (uint32_t)delta) & 0xffffu;
        uint32_t w = (t & 0xffu) | (v & 0xef00u) | ((uint32_t)((int32_t)(int16_t)t >> 8) << 12);

        freq_i2c_mem_write_new(addr, w, 3u);
    }
    uint32_t freq = chan_to_freq(phy_param16(PHY_PARAM_CHANNEL));
    uint32_t sub = (uint32_t)(int32_t)(int16_t)(0xfffff6a0u + freq);

    REG(FECOEX_FREQ_SET_CONF) = (REG(FECOEX_FREQ_SET_CONF) & 0xffffc00fu) | ((sub << 7) & 0x3ff0u);
}

/*
 * libphy's wr_rf_freq_mem_new [0x4200227e]: the frequency memory's words for one index, two of them, each
 * written by freq_i2c_mem_write_new at the address the ROM's get_freq_mem_addr answers, the second with its
 * flag word 3. The block and host come from the ROM's get_freq_mem_param(2), as pll_cap_mem_update_new's do.
 */
void wr_rf_freq_mem_new(uint32_t index, const uint32_t *words)
{
    uint32_t ret = get_freq_mem_param(2u);
    uint32_t high = (ret >> 16) & 0xffu;
    uint32_t mid = (ret >> 8) & 0xffu;

    freq_i2c_mem_write_new(get_freq_mem_addr(high, mid, index, 0u), words[0], 7u);
    freq_i2c_mem_write_new(get_freq_mem_addr(high, mid, index, 3u), words[1], 7u);
}

/*
 * libphy's rfpll_chgp_cal [0x42005a30]: the RF PLL's charge pump, calibrated through the analog block 0x62,
 * host 1. Register 0x0f's bits 6 and 5 are cleared and then its bit 5 set; then, up to 0x64 times with a 20 us
 * wait, register 0x0e's bit 7 is read until it rises. Register 0x0e's bits 4..0, the measurement, is turned into
 * the pump's setting -- times 7, over 6, plus 9, held at 0x1f -- written to register 0x0f's bits 4..0 with its
 * bit 6 set, and the register read back into phy_param 0x196. The library's print on the last round is left out;
 * only the library's rf_init calls it.
 */
void rfpll_chgp_cal(void)
{
    int32_t chgp;
    uint32_t value;

    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 0x0f, 6, 6, 0);
    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 0x0f, 5, 5, 0);
    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 0x0f, 5, 5, 1);

    for (uint32_t round = 0; round != 0x64u; round++) {
        ets_delay_us(0x14);
        if (rom_i2c_read_masked(I2C_RFPLL, I2C_HOST_MST, 0x0e, 7, 7) != 0u) {
            break;
        }
    }

    chgp = (int32_t)rom_i2c_read_masked(I2C_RFPLL, I2C_HOST_MST, 0x0e, 4, 0) * 7 / 6;
    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 0x0f, 6, 6, 1);
    chgp += 9;
    value = (uint8_t)chgp;
    if (value > 0x1fu) {
        value = 0x1fu;
    }
    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 0x0f, 4, 0, (uint8_t)value);
    phy_param[PHY_PARAM_I2C_RFPLL_0F] = rom_i2c_read(I2C_RFPLL, I2C_HOST_MST, 0x0f);
}

/*
 * libphy's phy_rfpll_cap_correct_new [0x420057b2]: the capacitance's own calibration, and, when it moved,
 * the memory rewritten through pll_cap_mem_update_new; its argument is only the library's print, left out.
 */
int32_t phy_rfpll_cap_correct_new(uint32_t verbose)
{
    int32_t delta = phy_rfpll_cap_init_cal_new();

    (void)verbose;
    if (delta != 0) {
        pll_cap_mem_update_new(delta);
    }
    return delta;
}

/* The ROM's two capacitance words of the RF PLL: its locked value written and read back. */
int32_t read_pll_cap(void);
void write_pll_cap(int32_t cap);
/* The ROM's abs_temp: the magnitude of a temperature difference. */
int32_t abs_temp(int32_t temp);
/* The ROM's set_rfpll_freq: the crystal selector, a frequency in MHz, the caller's parameter and a five-byte
 * out buffer, which it fills from the PLL's own setting; it is what set_chan_freq_hw_init's tree retunes with. */
void set_rfpll_freq(uint32_t xtal, uint32_t freq, uint32_t param, uint8_t *out);
/* The ROM's get_data_sat, which holds a value between a span and a bias (its order is index, span, bias). */
int32_t get_data_sat(int32_t index, int32_t span, int32_t bias);

/*
 * libphy's get_rf_freq_init_new [0x4200246c]: the frequency memory filled for the channels the receiver will
 * tune to. It runs once, marked by param_flag's bit 5, which sends it straight back afterwards. The PLL's
 * capacitance is written to 0xc8 and the analog block 0x62's register 2 armed -- its bit 7 cleared, the
 * frequency set through the ROM's set_rfpll_freq, register 6's bits 3..0 copied into register 2's, and its bit
 * 7 set. Two more settings, at 0x960 and 0x9a0 MHz, are read back as the capacitance's two ends, and their
 * difference steps a running value: for each of the caller's indices the library's ram_rfpll_set_freq (the
 * table's slot 12) sets the frequency, the running value turns into a baseband word through the ROM's
 * get_data_sat, and the out buffer and that word are packed into the memory's two words by
 * wr_rf_freq_mem_new. At the end param_flag's bit 5 marks it done. The library's own prints are left out.
 */
void get_rf_freq_init_new(uint32_t count, uint32_t param)
{
    uint8_t out[5];
    int32_t cap1, cap2, delta, sum, base;
    uint32_t s3;

    if ((phy_param32(PHY_PARAM_PARAM_FLAG) & 0x20u) != 0u) {
        return;
    }
    write_pll_cap(0xc8);
    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 2, 7, 7, 0);

    set_rfpll_freq(phy_param[PHY_PARAM_XTAL], 0x985u, param, out);
    s3 = rom_i2c_read_masked(I2C_RFPLL, I2C_HOST_MST, 6, 3, 0);
    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 2, 3, 0, (uint8_t)s3);
    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 2, 7, 7, 1);

    set_rfpll_freq(phy_param[PHY_PARAM_XTAL], 0x960u, param, out);
    cap1 = (int16_t)read_pll_cap();
    set_rfpll_freq(phy_param[PHY_PARAM_XTAL], 0x9a0u, param, out);
    cap2 = (int16_t)read_pll_cap();
    delta = cap2 - cap1;
    base = cap1;
    sum = 0;

    for (uint32_t i = 0; i != count; i++) {
        int32_t res, packed;
        uint32_t words[2];

        ((void (*)(uint32_t, uint32_t, uint32_t, uint8_t *))phy_get_romfuncs()[ROMFUNC_RAM_RFPLL_SET_FREQ])(
            i + 0x960u, phy_param[PHY_PARAM_XTAL], param, out);
        res = get_data_sat((int16_t)(base + sum / 0x40), 0x1ff, 0);
        packed = (uint32_t)(uint8_t)((((int16_t)res >> 8) << 4) | (int32_t)s3 | 0x80);
        words[0] = ((uint32_t)out[0] << 16) | ((uint32_t)packed << 8) | ((uint32_t)res & 0xffu);
        words[1] = ((uint32_t)out[3] << 16) | ((uint32_t)out[2] << 8) | (uint32_t)out[1];
        wr_rf_freq_mem_new((uint8_t)i, words);
        sum += delta;
    }
    phy_param32_set(PHY_PARAM_PARAM_FLAG, phy_param32(PHY_PARAM_PARAM_FLAG) | 0x20u);
}

/*
 * libphy's set_chan_freq_hw_init [0x42002628]: the frequency module's words, the frequency memory's build and
 * its write. The library declares it void and hands freq_reg_init the caller's a0 and a1 unchanged, which its
 * rf_init leaves as 2 and 4; its two parameters here are those, so a caller that set them is reproduced. The
 * frequency memory's count is 0x55 and its parameter 0. The write is a tail call.
 */
void set_chan_freq_hw_init(uint32_t param1, uint32_t param2)
{
    freq_reg_init(param1, param2);
    get_rf_freq_init_new(0x55u, 0u);
    freq_i2c_data_write_new();
}

/*
 * libphy's phy_rfpll_cap_init_cal_new [0x420056ac]: the RF PLL's capacitance searched from its locked value,
 * ten steps below and ten above, each written and its lock status read from the analog block 0x62, host 1,
 * register 0xc; the settings that lock twice in a run are the calibration, and their mean is written back.
 * The mean less the locked value is answered, as a signed halfword.
 */
int32_t phy_rfpll_cap_init_cal_new(void)
{
    int32_t cap = read_pll_cap();
    uint32_t sum = 0, count = 0, lock = 0;
    int pass = 0;
    int32_t mean;

    rom_i2c_write_masked(I2C_RFPLL, I2C_HOST_MST, 0x0b, 6, 6, 1);
restart:
    for (uint32_t step = 0; step < 10u; step++) {
        uint32_t cap_v = (uint32_t)((pass == 0 ? cap - (int32_t)step : cap + 1 + (int32_t)step)) & 0xffffu;
        uint32_t status;

        write_pll_cap((int16_t)cap_v);
        ets_delay_us(5);
        status = ((uint32_t)rom_i2c_read(I2C_RFPLL, I2C_HOST_MST, 0x0c) >> 2) & 3u;
        if (status == 0u) {
            sum = (sum + cap_v) & 0xffffu;
            count = (count + 1u) & 0xffu;
        } else if (pass == 0 && status == 1u) {
            if (++lock == 2u) {
                break;
            }
            lock = 1;
        } else if (pass == 1 && status == 2u) {
            if (++lock == 2u) {
                goto finish;
            }
            lock = 1;
        }
    }
    if (pass == 0) {
        pass = 1;
        lock = 0;
        goto restart;
    }
finish:
    mean = cap;
    if (count != 0u) {
        mean = (int32_t)(sum / count);
    }
    write_pll_cap((int16_t)mean);
    return (int16_t)(mean - cap);
}

/* libphy's phy_en_hw_set_freq_new [0x42001ee2]: the frequency setting's bits 27 and 28 cleared. */
void phy_en_hw_set_freq_new(void)
{
    REG(FECOEX_FREQ_SET_CONF) &= 0xe7ffffffu;
}

/*
 * libphy's phy_dis_hw_set_freq_new [0x42001f48]: the frequency setting's bit 28 set over bits 27 and 28
 * cleared, a 2 us wait, the wait for it to settle, and 0x14 us more where it did not settle at once.
 */
void phy_dis_hw_set_freq_new(void)
{
    REG(FECOEX_FREQ_SET_CONF) = (REG(FECOEX_FREQ_SET_CONF) & 0xe7ffffffu) | 0x10000000u;
    ets_delay_us(2);
    if (phy_wait_freq_set_busy_new() != 0u) {
        ets_delay_us(0x14);
    }
}

/* phy_param's bytes and halfwords the capacitance tracking keeps. */
#define PHY_PARAM_TEMP        0x09
#define PHY_PARAM_CAP_BUSY    0x19C
#define PHY_PARAM_TEMP_STEP_EN 0x1A5
#define PHY_PARAM_TEMP_STEP   0x1A6

/*
 * libphy's phy_rfpll_cap_track_new [0x42007b22]: while the temperature has moved more than its step since the
 * last correction, and no correction is under way, the frequency setting is held, the capacitance calibrated,
 * the temperature kept, and the setting released. Its argument is only the library's print, left out.
 */
void phy_rfpll_cap_track_new(uint32_t verbose)
{
    int32_t step = (phy_param[PHY_PARAM_TEMP_STEP_EN] & 1u) != 0u ? (int32_t)phy_param[PHY_PARAM_TEMP_STEP] : 0xf;
    int16_t temp = (int16_t)phy_param16(0x00u);
    int16_t last = (int16_t)phy_param16(PHY_PARAM_CAP_TEMP);

    (void)verbose;
    if (abs_temp((int32_t)temp - (int32_t)last) < step) {
        return;
    }
    if (phy_param[PHY_PARAM_CAP_BUSY] != 0u) {
        return;
    }
    phy_param[PHY_PARAM_CAP_BUSY] = 1;
    phy_dis_hw_set_freq_new();
    phy_rfpll_cap_correct_new(phy_param[PHY_PARAM_TEMP]);
    phy_param[PHY_PARAM_CAP_TEMP] = (uint8_t)temp;
    phy_param[PHY_PARAM_CAP_TEMP + 1u] = (uint8_t)((uint16_t)temp >> 8);
    phy_en_hw_set_freq_new();
    phy_param[PHY_PARAM_CAP_BUSY] = 0;
}

/*
 * libphy's ram_temp_to_power [0x4200846e]: the temperature's movement over the tracked one, a quarter of it
 * for Bluetooth above it, a fifth below it, and for Wi-Fi a sixth above and a fourth below, all in a signed byte.
 */
int32_t ram_temp_to_power(int32_t temp, int32_t base, uint32_t bt)
{
    int32_t d = (int16_t)(temp - base);

    if (bt != 0u) {
        return d > 0 ? (int8_t)(d >> 2) : (int8_t)(d / 5);
    }
    return d > 0 ? (int8_t)(d / 6) : (int8_t)(d / 4);
}

/* phy_param's bytes the transmit power tracking keeps. */
#define PHY_PARAM_TRACK_POWER  0x122
#define PHY_PARAM_TRACK_FLAG   0x1A0

/* The library's Bluetooth transmit gain, still its own here; a later package takes it over. */
void bt_set_tx_gain(uint32_t a0);

/*
 * libphy's txpwr_cal_track [0x42007c46]: the transmit power the temperature asks for, from the sensor's
 * halfword and the tracked one, held between the ROM's bounds, moved by ram_temp_to_power where it has
 * moved enough, and written into phy_param and the front end with phy_bbpll_cal around it -- Bluetooth's
 * gain or Wi-Fi's; the library's print is left out.
 */
void txpwr_cal_track(uint32_t en_bt, uint32_t threshold, uint32_t verbose)
{
    int16_t temp = (int16_t)phy_param16(0x00u);
    int16_t last = (int16_t)phy_param16(PHY_PARAM_TEMP_CODE_INIT);
    int32_t step = abs_temp((int32_t)temp - (int32_t)last) > 7 ? 4 : 2;
    int32_t power;
    int8_t cur;

    (void)verbose;
    if (phy_param[PHY_PARAM_TRACK_FLAG] != 0u) {
        step = 10;
    }
    power = (int16_t)get_data_sat((int32_t)temp, en_bt != 0u ? 0x69 : 0x50, -0x3c);
    if (abs_temp(power - (int16_t)phy_param16(PHY_PARAM_TRACK_TEMP_AT)) >= step) {
        power = ram_temp_to_power(power, last, en_bt);
    } else {
        power = (int8_t)phy_param[PHY_PARAM_TRACK_POWER];
    }
    if (threshold == 0u) {
        return;
    }
    cur = en_bt != 0u ? (int8_t)phy_param[PHY_PARAM_BT_CORRECT_POWER] : (int8_t)phy_param[PHY_PARAM_WIFI_CORRECT_POWER];
    if ((int32_t)cur == power) {
        return;
    }
    phy_bbpll_cal(1);
    phy_param[PHY_PARAM_TRACK_POWER] = (uint8_t)power;
    phy_param[PHY_PARAM_TRACK_TEMP_AT] = (uint8_t)temp;
    phy_param[PHY_PARAM_TRACK_TEMP_AT + 1u] = (uint8_t)((uint16_t)temp >> 8);
    if (en_bt != 0u) {
        phy_param[PHY_PARAM_BT_CORRECT_POWER] = (uint8_t)power;
        bt_set_tx_gain(0);
    } else {
        phy_param[PHY_PARAM_WIFI_CORRECT_POWER] = (uint8_t)power;
        wifi_set_tx_gain(phy_param16(PHY_PARAM_CHANNEL), 0u);
    }
    phy_bbpll_cal(0);
}

/* libphy's bt_track_tx_power [0x42007f72]: the tracking with Bluetooth's own temperature step. */
void bt_track_tx_power(uint32_t threshold, uint32_t verbose)
{
    txpwr_cal_track(1u, threshold, verbose);
}

/* libphy's wifi_track_tx_power [0x42007f80]: the tracking for Wi-Fi, whose step is the caller's. */
void wifi_track_tx_power(uint32_t threshold, uint32_t verbose)
{
    txpwr_cal_track(0u, threshold, verbose);
}

/* phy_param's byte the transmit I2C tracking keeps: which of the three ranges it last set. */
#define PHY_PARAM_TX_I2C 0x4D

/*
 * libphy's tx_i2c_track [0x42007d88]: the analog block 0x6b, host 1, set for the temperature's range --
 * 1 above freezing's of the sensor, 2 above 0x36, 0 between them -- four masked writes each, with the
 * seventh register's value asking whether the channel is above 12, and the range kept in phy_param 0x4d.
 */
void tx_i2c_track(void)
{
    if ((int16_t)phy_param16(0x00u) <= 0 && phy_param[PHY_PARAM_TX_I2C] != 1u) {
        rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 3, 3, 0, 0xa);
        rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 3, 7, 4, 0x3);
        rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 7, 3, 0, 0x5);
        rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 4, 7, 4, 0x6);
        phy_param[PHY_PARAM_TX_I2C] = 1;
    }
    if ((int16_t)phy_param16(0x00u) > 0x36 && phy_param[PHY_PARAM_TX_I2C] != 2u) {
        rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 3, 3, 0, 0xb);
        rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 3, 7, 4, 0x4);
        rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 7, 3, 0, phy_param16(PHY_PARAM_CHANNEL) > 0xcu ? 0x5 : 0x7);
        rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 4, 7, 4, 0xa);
        phy_param[PHY_PARAM_TX_I2C] = 2;
    }
    if ((uint16_t)((int16_t)phy_param16(0x00u) - 1) > 0x35u || phy_param[PHY_PARAM_TX_I2C] == 0u) {
        return;
    }
    rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 3, 3, 0, 0xb);
    rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 3, 7, 4, 0x3);
    rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 7, 3, 0, phy_param16(PHY_PARAM_CHANNEL) > 0xcu ? 0x5 : 0x8);
    rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, 4, 7, 4, 0xa);
    phy_param[PHY_PARAM_TX_I2C] = 0;
}

/* The receive gain table's groups of six: the gain each of the seven entries stands for, past its sixth step. */
static const uint16_t rx_gain_group[7] = { 0, 0x20, 0x30, 0x38, 0x3c, 0x3e, 0x3f };

/*
 * libphy's gen_rx_gain_table_new [0x42007140]: the rows of a receive-gain table, one per step of the gain
 * code the caller asks for, each the row's own gain shifted left twelve, the group's gain at the step's
 * sixth, and the step within that sixth in the low bits -- the three added, not orred, and the group's
 * gain truncated to sixteen bits after its own shift. The code starts at the second table's first byte
 * and rises by one per row until it passes the caller's limit or the table's 0x55 rows are full, which it
 * answers; while the two tables' bytes at the current index add to it, the index moves past that run and
 * the code takes the second table's byte there. Its print of each row and its tail is left out.
 */
uint32_t gen_rx_gain_table_new(uint32_t *out, uint8_t limit, const uint16_t *gains, const int8_t *a,
                               const int8_t *b, uint8_t count, uint32_t verbose)
{
    int32_t last = (int32_t)count - 1;
    uint8_t cur = (uint8_t)b[0];
    uint8_t i = 0;
    uint8_t j = 0;

    (void)verbose;
    for (;;) {
        if ((int32_t)a[j] + (int32_t)b[j] == (int32_t)cur && (int32_t)j < last) {
            do {
                j = (uint8_t)(j + 1u);
            } while (a[j] == 0 && (int32_t)j < last);
            cur = (uint8_t)b[j];
        }
        out[i] = ((uint32_t)gains[j] << 12) +
                 ((((uint32_t)rx_gain_group[(uint32_t)cur / 6u] << 4) & 0xffffu) + (uint32_t)(cur % 6u));
        if (limit < cur) {
            return i;
        }
        cur = (uint8_t)(cur + 1u);
        i = (uint8_t)(i + 1u);
        if (i == 0x55u) {
            return 0x55u;
        }
    }
}

/*
 * The receive gain table's DC offset, which set_rx_gain_table runs before it writes its rows: the analog
 * and pbus leaves of libphy's set_rx_gain_cal_dc_new and the search under it, the driver's own here.
 * Each is held to the library's off the board, by phy-ab.py; build.mk weakens the library's set_rx_gain_cal_dc_new
 * and phy_force_rx_gain_trig, the two the library calls from elsewhere too.
 */

/* The ROM's table slots this subtree drives the front end and the pbus through, past the ones already named. */
#define ROMFUNC_BB_BSS_CBW40       4  /* rom_bb_bss_cbw40: a0, whether the baseband's channel is 40 MHz wide */
#define ROMFUNC_PBUS_RD           30  /* rom_pbus_rd: a pbus address and a place in its word, answers the bits */
#define ROMFUNC_PBUS_SET_RXGAIN   31  /* rom_pbus_set_rxgain: a0, the receiver's gain into the pbus */
#define ROMFUNC_PBUS_XPD_RX_OFF   32  /* rom_pbus_xpd_rx_off: the receiver powered down */
#define ROMFUNC_PBUS_XPD_TX_OFF   34  /* rom_pbus_xpd_tx_off: the transmitter powered down */
#define ROMFUNC_START_TX_TONE_STEP 38 /* rom_start_tx_tone_step: a0 to a5, a tone at a frequency, step and scale */
#define ROMFUNC_STOP_TX_TONE       39 /* rom_stop_tx_tone: the tone started above stopped */
#define ROMFUNC_IQ_EST_DISABLE    41  /* rom_iq_est_disable: the IQ estimate's accumulation stopped */

/* The modem's IQ estimate, its DC accumulators and receive-gain SAR, and the trigger that starts them. */
#define FEDATA_RX_IQ_EST0      (MODEM_BASE + 0x470u)
#define FEDATA_RX_IQ_EST1      (MODEM_BASE + 0x474u)
#define FEDATA_IQ_EST_I        (MODEM_BASE + 0x488u) /* the estimate's in-phase accumulator */
#define FEDATA_IQ_EST_Q        (MODEM_BASE + 0x48Cu) /* its quadrature accumulator */
#define FEDATA_IQ_EST_MAG      (MODEM_BASE + 0x490u) /* and its magnitude */
#define FEDATA_RX_GAIN_SAR     (MODEM_BASE + 0x4A0u)
#define FECTRL_RX_GAIN_TRIG_EN (MODEM_BASE + 0x904u) /* bit 0: the trigger is the hardware's */
#define AGC_GAIN_SAR      (MODEM_BASE + 0x708Cu)

/* The ROM's linear_to_db: a linear power's decibels. */
int linear_to_db(int32_t value, int32_t offset);

/* The ROM's pbus readers and forcers, which the library reaches through its table, as its slots take them. */
static uint32_t rom_pbus_rd(uint32_t addr, uint32_t shift)
{
    return ((uint32_t (*)(uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_RD])(addr, shift);
}

static void rom_pbus_force_test(uint32_t a0, uint32_t a1, uint32_t a2)
{
    ((void (*)(uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_FORCE_TEST])(a0, a1, a2);
}

static void rom_pbus_set_rxgain(uint32_t gain)
{
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_SET_RXGAIN])(gain);
}

static void rom_pbus_xpd_rx_off(void)
{
    ((void (*)(void))phy_get_romfuncs()[ROMFUNC_PBUS_XPD_RX_OFF])();
}

static void rom_pbus_xpd_tx_off(void)
{
    ((void (*)(void))phy_get_romfuncs()[ROMFUNC_PBUS_XPD_TX_OFF])();
}

static void rom_iq_est_disable(void)
{
    ((void (*)(void))phy_get_romfuncs()[ROMFUNC_IQ_EST_DISABLE])();
}

/* phy_param's words the receive DC calibration keeps: the trigger's count, a halfword, and its flag. */
#define PHY_PARAM_IQ_EST_COUNT 0x1A2

/*
 * libphy's phy_force_rx_gain_trig [0x42005b5a]: where the modem lets the hardware trigger the receive gain,
 * the baseband's word's hold below bit 18 set to 0x46000, then its bit 11 pulsed with a 1 us wait.
 */
void phy_force_rx_gain_trig(void)
{
    if ((REG(FECTRL_RX_GAIN_TRIG_EN) & 1u) == 0u) {
        return;
    }
    REG(AGC_2) = (REG(AGC_2) & 0xffffffu) | 0x46000000u;
    REG(AGC_2) |= 0x800000u;
    ets_delay_us(1);
    REG(AGC_2) &= ~0x800000u;
}

/*
 * libphy's ram_iq_est_enable [0x42005bac]: the IQ estimate's two words armed, its count from the argument's
 * three low bits, the trigger started, and the count of the SAR readings taken until the modem answers
 * past 0x45, kept in phy_param where the first reading counted; its first argument is unused.
 */
void ram_iq_est_enable(uint32_t a0, uint32_t n)
{
    uint32_t v;
    uint16_t count = 0;
    uint32_t wrote = 0;

    (void)a0;
    v = REG(FEDATA_RX_IQ_EST0);
    REG(FEDATA_RX_IQ_EST0) = (v & 0xf3ffffffu) | 0x4000000u;
    v = REG(FEDATA_RX_IQ_EST1);
    REG(FEDATA_RX_IQ_EST1) = (v & 0xffe7ffffu) | 0x100000u;
    v = REG(FEDATA_RX_IQ_EST1);
    REG(FEDATA_RX_IQ_EST1) = ((n << 2) & 0x1fffcu) | (v & 0xfffe0003u);
    REG(FEDATA_RX_IQ_EST1) = REG(FEDATA_RX_IQ_EST1) | 1u;
    phy_param[PHY_PARAM_IQ_EST_COUNT] = 0;
    phy_param[PHY_PARAM_IQ_EST_COUNT + 1u] = 0;
    ets_delay_us(1);
    REG(FEDATA_RX_IQ_EST1) = REG(FEDATA_RX_IQ_EST1) | 2u;
    phy_force_rx_gain_trig();
    count = phy_param16(PHY_PARAM_IQ_EST_COUNT);
    for (;;) {
        if ((REG(FEDATA_RX_GAIN_SAR) & 0x10000u) != 0u) {
            if (wrote != 0u) {
                phy_param[PHY_PARAM_IQ_EST_COUNT] = (uint8_t)count;
                phy_param[PHY_PARAM_IQ_EST_COUNT + 1u] = (uint8_t)(count >> 8);
            }
            return;
        }
        if (((REG(AGC_GAIN_SAR) >> 12) & 0x7fu) <= 0x45u) {
            count = (uint16_t)(count + 1u);
            wrote = 1;
        }
    }
}

/*
 * libphy's dc_iq_est_new [0x42005c82]: the estimate taken, its accumulators shifted -- by 4 where the fourth
 * argument is set, 6 otherwise -- and divided by one more than the second argument, their squares' sum
 * taken from the magnitude's eight times, held at zero, turned to decibels and rounded, and the estimate
 * disabled. The third argument takes the two accumulators and the decibels.
 */
void dc_iq_est_new(uint32_t a0, uint32_t a1, int32_t out[3], uint32_t a3)
{
    uint32_t shift = a3 != 0u ? 4u : 6u;
    int32_t div = (int32_t)a1 + 1;
    int32_t i, q, sq, v;

    ram_iq_est_enable(a0, a1);
    i = ((int32_t)REG(FEDATA_IQ_EST_I) >> shift) / div;
    out[0] = i;
    q = ((int32_t)REG(FEDATA_IQ_EST_Q) >> shift) / div;
    out[1] = q;
    /* The squares wrap as the machine's words do, and the shifts are the machine's own. */
    sq = (int32_t)((uint32_t)i * (uint32_t)i + (uint32_t)q * (uint32_t)q);
    if (a3 != 0u) {
        sq >>= 4;
    }
    v = (int32_t)((uint32_t)((int32_t)REG(FEDATA_IQ_EST_MAG) / div) * 8u - (uint32_t)sq);
    if (v < 0) {
        v = 0;
    }
    out[2] = (int32_t)(((uint32_t)linear_to_db(v, 0) + 8u) >> 4);
    rom_iq_est_disable();
}

/*
 * libphy's rxdc_est_min_new [0x4200641a]: the estimate repeated until it settles, the decibels of its best
 * kept in the third argument with the two accumulators, at most eight times; it gives up with 0x38 where
 * the decibels stay within 0x23 to 0x2f past two rounds. The second argument is unused.
 */
void rxdc_est_min_new(uint32_t a0, uint32_t a1, int32_t out[3], uint32_t a3)
{
    int32_t est[3] = { 0, 0, 0 };
    int32_t best = 0x64;
    uint32_t pass = 0;

    (void)a1;
    for (;;) {
        dc_iq_est_new(1u, a0, est, a3);
        if (est[2] < best && (phy_param16(PHY_PARAM_IQ_EST_COUNT) == 0u || phy_param[PHY_PARAM_RX_SAT] == 1u)) {
            out[2] = est[2];
            best = est[2];
            out[0] = est[0];
            out[1] = est[1];
        }
        if (best > 0x2f) {
            /* fall through to another round */
        } else if (pass >= 2u || best <= 0x23) {
            return;
        }
        pass = (pass + 1u) & 0xffu;
        if (pass == 8u) {
            out[2] = 0x38;
            return;
        }
    }
}

/* What a DC word is held to: nothing below, 0x1ff above. */
static int16_t rx_dc_clamp(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 0x1ff) {
        return 0x1ff;
    }
    return (int16_t)v;
}

/*
 * libphy's pbus_rx_dco_cal_1step_new [0x420064b6]: the receive DC offset's one step. The pbus is forced
 * at the caller's two words, the offset estimated, and the two words moved by the estimate's low bits,
 * up to the second argument's bound rounds; the first argument is the caller's mode through the estimates
 * and their clamp, the third the divisor, the fourth's two halfwords are the offset in and out, and the
 * fifth takes one once the round settles. Six pbus readings the library prints are taken but their print
 * is left out.
 */
void pbus_rx_dco_cal_1step_new(uint32_t a0, uint32_t mode, uint32_t div, int16_t dc[2], uint8_t *flag)
{
    int32_t e0[3] = { 0, 0, 0 }, e1[3] = { 0, 0, 0 }, e2[3] = { 0, 0, 0 };
    uint32_t bits = (uint32_t)(uint8_t)rom_pbus_rd(1u, 2u);
    uint32_t pop = 0;
    uint32_t s3, s4, s10, bound, outer = 0;
    int16_t s6 = dc[0], s9 = dc[1];
    int16_t s6_0 = dc[0], s9_0 = dc[1];
    uint32_t chase = (phy_param16(0x10u) >> 3) & 1u;
    int32_t s11 = 0;
    int32_t v1, v2;

    for (uint32_t k = 0; k < 6u; k++) {
        pop = (pop + ((bits >> k) & 1u)) & 0xffu;
    }
    if (mode == 1u) {
        s3 = (pop >= 2u ? pop : 2u) - 1u;
        bound = 8u;
    } else {
        s3 = a0 != 0u ? 0x18u : 4u;
        bound = 0xcu;
    }
    *flag = 0;
    s10 = mode == 1u ? 2u : 1u;

loop:
    rom_pbus_force_test(2u, s10, (uint16_t)s6);
    dc[0] = s6;
    dc[1] = s9;
    rom_pbus_force_test(3u, s10, (uint16_t)dc[1]);
    if (mode != 1u) {
        rom_pbus_force_test(1u, 2u, 0u);
        ets_delay_us(0xa);
        rxdc_est_min_new(div, 1u, e1, 1u);
        rom_pbus_force_test(1u, 2u, 0x20u);
        ets_delay_us(0xa);
        rxdc_est_min_new(div, 1u, e2, 1u);
        e0[0] = e2[0] - e1[0];
        e0[1] = e2[1] - e1[1];
        s11 = e2[2];
        if (e1[2] > s11) {
            s11 = e1[2];
        }
        s4 = a0 != 0u ? 5u : 2u;
    } else {
        ets_delay_us(0xa);
        rxdc_est_min_new(div, 1u, e0, 0u);
        s4 = ((pop >= 2u ? pop : 2u) - 2u) & 0xffu;
        s11 = e0[2];
    }
    v1 = abs_temp(e0[0]) >= (int32_t)s3 ? (int16_t)(e0[0] >> s4) : 0;
    v2 = abs_temp(e0[1]) >= (int32_t)s3 ? (int16_t)(e0[1] >> s4) : 0;
    if (v1 == 0) {
        if (abs_temp(e1[0]) > 0x31) {
            v1 = (int16_t)(e1[0] >> s4);
        } else if (e0[0] > 0) {
            v1 = 1;
        } else {
            v1 = e0[0] != 0 ? -1 : 0;
        }
    }
    if (v2 == 0) {
        if (abs_temp(e1[1]) > 0x31) {
            v2 = (int16_t)(e1[1] >> s4);
        } else if (e0[1] > 0) {
            v2 = 1;
        } else {
            v2 = e0[1] != 0 ? -1 : 0;
        }
    }
    if (mode != 1u) {
        if (s11 > 0x2c) {
            v1 = 0;
            v2 = 0;
        }
        if (mode == 2u) {
            v1 = get_data_sat(v1, 5, -5);
            v2 = get_data_sat(v2, 5, -5);
        }
    }
    if (chase != 0u) {
        (void)rom_pbus_rd(0u, 1u);
        (void)rom_pbus_rd(1u, 2u);
        (void)rom_pbus_rd(2u, 1u);
        (void)rom_pbus_rd(3u, 1u);
        (void)rom_pbus_rd(2u, 2u);
        (void)rom_pbus_rd(3u, 2u);
    }
    if ((int32_t)s3 >= abs_temp(e0[0]) && (int32_t)s3 >= abs_temp(e0[1]) && s11 <= 0x2d) {
        *flag = 1;
        goto finish;
    }
    if ((int32_t)s3 < abs_temp(e0[0])) {
        s6 = (int16_t)(s6 - v1);
    }
    if ((int32_t)s3 < abs_temp(e0[1])) {
        s9 = (int16_t)(s9 - v2);
    }
    s6 = rx_dc_clamp(s6);
    s9 = rx_dc_clamp(s9);
    outer = (uint8_t)(outer + 1u);
    if ((uint8_t)outer < bound) {
        goto loop;
    }
    if (*flag != 0u || mode == 1u) {
        goto finish;
    }
    dc[0] = s6_0;
    dc[1] = s9_0;
finish:
    dc[0] = rx_dc_clamp(dc[0]);
    dc[1] = rx_dc_clamp(dc[1]);
    rom_pbus_force_test(2u, s10, (uint16_t)dc[0]);
    rom_pbus_force_test(3u, s10, (uint16_t)dc[1]);
}

/* libphy's rx_pbus_reset [0x4200484e]: the receiver forced through the ROM's three test windows. */
void rx_pbus_reset(uint32_t on)
{
    rom_pbus_force_test(1u, 1u, 0u);
    rom_pbus_force_test(1u, 1u, on != 0u ? 0x1cbu : 0x1c9u);
    rom_pbus_force_test(1u, 1u, on != 0u ? 0x18bu : 0x189u);
}

/* The receive DC calibration's tables, its read of the DC offset out of phy_param, and libphy's own sizes. */
void set_rx_gain_cal_dc_new(uint32_t mode, uint32_t flag, int16_t *pairs, int16_t *out_a, int16_t *out_b);

/*
 * libphy's set_rx_gain_cal_dc_new [0x42006d38]: the receive gain table's DC offset search, over the mode's
 * channels or a single 0xe, the pbus and the analog I2C set up, and one step of pbus_rx_dco_cal_1step_new
 * for each of the mode's steps; the strongest pbus readings among them, 0x100 each where the mode asks,
 * and the settled offsets are written into the first and fourth tables' pairs and the third's, which the
 * caller's gains table then reads. A flag set takes the alternate first pass, whose second channel's
 * offsets it answers in the fifth argument. The library's print is left out.
 */
void set_rx_gain_cal_dc_new(uint32_t mode, uint32_t flag, int16_t *pairs, int16_t *out_a, int16_t *out_b)
{
    static const uint16_t sel_tab[8] = { 0x40, 0x41, 0x43, 0x6b, 0x77, 0x79, 0x7b, 0x7f };
    static const uint16_t code_tab[7] = { 0x41, 0x43, 0x6b, 0x77, 0x79, 0x7b, 0x7f };
    static const uint16_t chan_tab[7] = { 0x1, 0x3, 0x5, 0x7, 0x9, 0xb, 0xd };
    int16_t dc0[2] = { 0x100, 0x100 };
    int16_t dc1[2] = { 0x100, 0x100 };
    uint8_t done = 0;
    uint32_t inner = mode == 1u ? 1u : (flag == 0u ? 8u : 7u);
    uint32_t outer = mode == 1u ? 7u : 1u;
    uint32_t pbus_a1 = mode == 1u ? 2u : 0u;
    int16_t *dst_b = out_b;

    for (uint32_t o = 0; o < outer; o++) {
        chip_v7_set_chan_ana(mode != 0u ? chan_tab[o % 7u] : 0xeu);
        pbus_debugmode();
        rom_pbus_xpd_tx_off();
        rom_pbus_xpd_rx_off();
        ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_XPD_RX_ON])(0u);
        set_rxclk_en(1u);
        set_txclk_en(1u);
        if (flag != 0u) {
            rom_i2c_write_masked(I2C_BBTOP, I2C_HOST_MST, 3, 2, 2, 0);
        } else if (mode == 0u) {
            if (o == 0u) {
                rom_pbus_set_rxgain(0x43000u);
                rx_pbus_reset(0u);
                rom_pbus_force_test(2u, 2u, 0x100u);
                rom_pbus_force_test(3u, 2u, 0x100u);
                pbus_rx_dco_cal_1step_new(0u, 0u, 0x800u, dc0, &done);
                pbus_rx_dco_cal_1step_new(0u, 1u, 0x800u, dc1, &done);
                out_a[0] = dc1[0];
                out_a[1] = dc1[1];
            }
        } else {
            dc0[0] = pairs[4];
            dc0[1] = pairs[5];
        }
        for (uint32_t k = 0; k < inner; k++) {
            uint32_t code = flag != 0u ? code_tab[k] : (mode != 0u ? 0x7fu : sel_tab[k]);

            rom_pbus_set_rxgain(code << 12);
            rx_pbus_reset(flag);
            if (flag != 0u) {
                rom_pbus_force_test(2u, 2u, 0x100u);
                rom_pbus_force_test(3u, 2u, 0x100u);
            } else {
                rom_pbus_force_test(2u, 2u, (uint16_t)out_a[0]);
                rom_pbus_force_test(3u, 2u, (uint16_t)out_a[1]);
            }
            pbus_rx_dco_cal_1step_new(flag, pbus_a1, 0x800u, dc0, &done);
            if (mode != 0u) {
                dst_b[0] = dc0[0];
                dst_b[1] = dc0[1];
            } else {
                pairs[k * 2u] = dc0[0];
                pairs[k * 2u + 1u] = dc0[1];
            }
        }
        if (flag != 0u) {
            rom_i2c_write_masked(I2C_BBTOP, I2C_HOST_MST, 3, 2, 2, 1);
        }
        set_rxclk_en(0u);
        set_txclk_en(0u);
        rom_pbus_xpd_rx_off();
        pbus_workmode();
        dst_b += 2;
    }
}

/* The ROM's receive-gain adjustments set_rx_gain_table makes after the rows: a baseband gain offset and the
 * receiver's IQ correction. */
void bt_gain_offset(uint32_t offset);
void iq_corr_enable(void);

/* phy_param's word and halfwords the gain table keeps, and the two rows its tables are built into. */
#define PHY_PARAM_RX_N1   0x121
#define PHY_PARAM_RX_N2   0x120
#define PHY_PARAM_RX_DC_T 0x198  /* the temperature the rows were built at */

static uint32_t phy_param32(uint32_t off)
{
    uint32_t v;

    __builtin_memcpy(&v, phy_param + off, sizeof(v));
    return v;
}

static void phy_param32_set(uint32_t off, uint32_t v)
{
    __builtin_memcpy(phy_param + off, &v, sizeof(v));
}

/*
 * libphy's set_rx_gain_table [0x420077b8]: the receive gain table's rows built and written. Unless
 * phy_param 0xa4's bit 9 says they are done, and unless its bit 7 says the DC calibration is, the
 * offset is calibrated for both tables; then two gen_rx_gain_table_new runs build the rows -- one to
 * 0x21, one to 0x1c -- each written by wr_rx_gain_mem with the opposite table's DC offsets, the
 * counts kept in phy_param, and the temperature the rows were built at remembered. The baseband's
 * gain offset and the receiver's IQ correction are set either way. The library's prints are left out.
 */
void set_rx_gain_table(uint32_t p1, uint32_t p2)
{
    static const uint16_t gains1[8] = { 0x40, 0x41, 0x43, 0x6b, 0x77, 0x79, 0x7b, 0x7f };
    static const uint16_t gains2[7] = { 0x41, 0x43, 0x6b, 0x77, 0x79, 0x7b, 0x7f };
    static const int8_t a1[8] = { 0x0b, 0x0a, 0x06, 0x06, 0x06, 0x06, 0x08, 0x00 };
    static const int8_t b1[8] = { 0x03, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08 };
    static const int8_t a2[7] = { 0x0b, 0x0e, 0x06, 0x05, 0x06, 0x08, 0x00 };
    static const int8_t b2[7] = { 0x00, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08 };
    uint32_t out[0x55];
    uint32_t a4;
    uint32_t verbose;
    uint32_t n;

    (void)p1;
    (void)p2;
    a4 = phy_param32(PHY_PARAM_PARAM_FLAG);
    if ((a4 & 0x200u) == 0u) {
        uint32_t cal = phy_param16(0x10u);

        if ((a4 & 0x80u) == 0u) {
            set_rx_gain_cal_dc_new(0u, 1u, (int16_t *)(void *)(phy_param + 0x132u),
                                   (int16_t *)(void *)(phy_param + 0x132u), (int16_t *)(void *)(phy_param + 0x132u));
            set_rx_gain_cal_dc_new(0u, 0u, (int16_t *)(void *)(phy_param + 0x14eu),
                                   (int16_t *)(void *)(phy_param + 0x16eu), (int16_t *)(void *)(phy_param + 0x172u));
            set_rx_gain_cal_dc_new(1u, 0u, (int16_t *)(void *)(phy_param + 0x14eu),
                                   (int16_t *)(void *)(phy_param + 0x16eu), (int16_t *)(void *)(phy_param + 0x172u));
            phy_param32_set(PHY_PARAM_PARAM_FLAG, phy_param32(PHY_PARAM_PARAM_FLAG) | 0x80u);
        }
        verbose = (cal >> 8) & 1u;
        n = gen_rx_gain_table_new(out, 0x21u, gains1, a1, b1, 8u, verbose);
        if (n > 0x4fu) {
            n = 0x4fu;
        }
        phy_param[PHY_PARAM_RX_N1] = (uint8_t)n;
        wr_rx_gain_mem(0u, (uint8_t)(phy_param[PHY_PARAM_RX_N1] + 1u), out,
                       (const uint16_t *)(const void *)(phy_param + 0x14eu),
                       (const uint16_t *)(const void *)(phy_param + 0x16eu),
                       (const uint16_t *)(const void *)(phy_param + 0x172u), 0xeu, 0u);
        n = gen_rx_gain_table_new(out, 0x1cu, gains2, a2, b2, 7u, verbose);
        if (n > 0x4fu) {
            n = 0x4fu;
        }
        phy_param[PHY_PARAM_RX_N2] = (uint8_t)n;
        wr_rx_gain_mem(1u, (uint8_t)(phy_param[PHY_PARAM_RX_N2] + 1u), out,
                       (const uint16_t *)(const void *)(phy_param + 0x132u),
                       (const uint16_t *)(const void *)(phy_param + 0x16eu),
                       (const uint16_t *)(const void *)(phy_param + 0x132u), 0xeu, 0u);
        phy_param32_set(PHY_PARAM_PARAM_FLAG, phy_param32(PHY_PARAM_PARAM_FLAG) | 0x200u);
        phy_param[PHY_PARAM_RX_DC_T] = (uint8_t)phy_param16(0x00u);
        phy_param[PHY_PARAM_RX_DC_T + 1u] = (uint8_t)(phy_param16(0x00u) >> 8);
    }
    REG(AGC_2) = (((uint32_t)phy_param[PHY_PARAM_RX_N1] << 8) & 0x7f00u) |
                                (REG(AGC_2) & 0xffff80ffu);
    n = phy_param[PHY_PARAM_RX_N1];
    if (n > 0x4cu) {
        n = 0x4cu;
    }
    REG(MODEM_BASE + 0x713Cu) = ((n & 0xffu) << 18) | (REG(MODEM_BASE + 0x713Cu) & 0xfe03ffffu);
    bt_gain_offset(0x50u);
    iq_corr_enable();
}

/*
 * The transmitter's DC offset calibration, which the library's second tracking block and its bring-up both run:
 * the power detector's readings of a tone at several offsets of the front end's DCO word, and the search that
 * settles them. The leaves are the ROM's, called as the library's does.
 */
uint32_t get_tone_sar_dout(uint32_t a0);
uint32_t index_to_txbbgain(uint32_t index);
void pbus_set_dco(void *dco);

/* The ROM's own, reached through the table as libphy's does: the power detector's clocks and the transmitter's. */
#define ROMFUNC_EN_PWDET        7 /* rom_en_pwdet: the power detector's clocks turned on */
#define ROMFUNC_PBUS_XPD_TX_ON 35 /* rom_pbus_xpd_tx_on: the transmitter powered, its channel and a value */

static void rom_en_pwdet(uint32_t a0)
{
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_EN_PWDET])(a0);
}

static void rom_pbus_xpd_tx_on(uint32_t a0, uint32_t a1)
{
    ((void (*)(uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_XPD_TX_ON])(a0, a1);
}

static void rom_start_tx_tone_step(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))
     phy_get_romfuncs()[ROMFUNC_START_TX_TONE_STEP])(a0, a1, a2, a3, a4, a5);
}

/* phy_param's halfwords the calibration keeps: the two pairs it settles, and the four it starts them from. */
#define PHY_PARAM_TXDC_SRC2  0xB0
#define PHY_PARAM_TXDC_SRC3  0xB2

static void phy_param16_set(uint32_t off, uint16_t v)
{
    phy_param[off] = (uint8_t)v;
    phy_param[off + 1u] = (uint8_t)(v >> 8);
}

/*
 * The receive table and the transmit channel's offset, the driver's own this step (package 6): the channel's
 * frequency offset, the transmit channel's two offset bits and the receive table's own initialisation.
 * Each is held to the library's with phy-ab.py, and build.mk then weakens the library's in its copy.
 */

/* phy_param's frequency correction, which chip_v7_set_chan_offset builds and chip_v7_set_chan and the
 * ROM's phy_freq_correct and set_channel_rfpll_freq take; no label names it. */
#define PHY_PARAM_FREQ_OFFSET 0x20
/* The two bytes the library asks before it builds the offset: the first says whether the second's signed
 * byte is added in eighths. No label names either, and the meaning of both is not known. */
#define PHY_PARAM_CHAN_OFFSET_EN 0xA1
#define PHY_PARAM_CHAN_OFFSET    0xA2

/*
 * libphy's chip_v7_set_chan_offset [0x4200597e]: the channel's frequency offset built from the channel
 * itself -- the channel plus two, its low two bits dropped -- and, where phy_param 0xa1 asks, phy_param
 * 0xa2's signed byte added in eighths; then the frequency corrected, the AGC taken down, the RF PLL retuned
 * with the offset and the current channel, and the AGC put back. The library runs it inside the ROM's
 * critical section, the pair of empty returns [0x40002e1e, 0x40002e22], so this leaves that out; its only
 * caller is the library's register_chipv7_phy, until package 10 takes that over.
 */
void chip_v7_set_chan_offset(uint32_t chan)
{
    int32_t offset = (int16_t)((chan + 2u) & 0xfffffffcu);

    phy_param16_set(PHY_PARAM_FREQ_OFFSET, (uint16_t)offset);
    if (phy_param[PHY_PARAM_CHAN_OFFSET_EN] != 0u) {
        offset += (int8_t)phy_param[PHY_PARAM_CHAN_OFFSET] * 8;
        phy_param16_set(PHY_PARAM_FREQ_OFFSET, (uint16_t)offset);
    }
    offset = (int16_t)phy_param16(PHY_PARAM_FREQ_OFFSET);
    phy_freq_correct(1u, offset);
    disable_agc();
    set_channel_rfpll_freq(phy_param16(PHY_PARAM_CHANNEL), phy_param[PHY_PARAM_XTAL], offset);
    enable_agc();
}

/*
 * libphy's mac_tx_chan_offset_new [0x4200502e]: the transmit channel's two offset bits in the transmit
 * minimum power's word, MAC_TX_MIN_PWR, which the MAC's own start also writes -- 2 for channel 2, 1 for
 * channel 3 and 0 for every other channel, the word's other bits kept. Its ROM counterpart
 * mac_tx_chan_offset [0x40004dbe] also sets bit 0 for channel 2, which is how the library's own differs.
 */
void mac_tx_chan_offset_new(uint32_t chan)
{
    if (chan == 2u) {
        REG(MAC_TX_MIN_PWR) = (REG(MAC_TX_MIN_PWR) & ~0x3u) | 0x2u;
    } else if (chan == 3u) {
        REG(MAC_TX_MIN_PWR) = (REG(MAC_TX_MIN_PWR) & ~0x3u) | 0x1u;
    } else {
        REG(MAC_TX_MIN_PWR) = REG(MAC_TX_MIN_PWR) & ~0x3u;
    }
}

/*
 * libphy's phy_rx_table_init [0x42007760]: phy_param's receive band halfword 0x4f4f, then the front end's
 * gain memory written 0x4f times with one entry's four words, then the front end's registers and the AGC.
 */
void phy_rx_table_init(void)
{
    phy_param16_set(PHY_PARAM_RX_N2, 0x4f4fu);
    for (uint32_t i = 0; i < 0x4fu; i++) {
        write_gain_mem_new(0x40200000u, 0x10080u, 0u, i);
    }
    phy_reg_init();
    enable_agc();
}

/*
 * The receive IQ's mismatch and the saturation checks, the driver's own this step (package 7a). Each is
 * held to the library's with phy-ab.py, and build.mk then weakens the library's in its copy.
 */

/* The estimate's four words rxiq_get_mis_new reads, beside the two ram_iq_est_enable arms; no label
 * names them and what each holds is not known. */
#define FEDATA_RX_IQ_EST2 (MODEM_BASE + 0x478u)
#define FEDATA_RX_IQ_EST3 (MODEM_BASE + 0x47Cu)
#define FEDATA_RX_IQ_EST4 (MODEM_BASE + 0x480u)
#define FEDATA_RX_IQ_EST5 (MODEM_BASE + 0x484u)

/* The baseband's word rfrx_sat_rst moves, which no label names; what it holds is not known. */
#define AGC_RFRX_SAT (MODEM_BASE + 0x705Cu)

/* The ROM's rom_iq_est_enable, the table's slot 40, which the two readers below reach through it; its
 * first argument is not read. */
#define ROMFUNC_IQ_EST_ENABLE 40

static void rom_iq_est_enable(uint32_t a0, uint32_t n)
{
    ((void (*)(uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_IQ_EST_ENABLE])(a0, n);
}

/* The ROM's ram_pbus_xpd_rx_on, the table's slot 33, which phy_check_rx_sat reaches through it. */
void ram_pbus_xpd_rx_on(uint32_t arg);

/*
 * libphy's rfrx_sat_rst [0x4200543c]: AGC_4 written 0x404 outright, then AGC_RFRX_SAT's bits 31, 30, 28,
 * 24 and 19 set where the argument is set and cleared where it is clear, its bits 0 to 18 cleared, and
 * bit 11 -- where the argument is set -- or bit 10 raised.
 */
void rfrx_sat_rst(uint32_t on)
{
    uint32_t v;

    REG(AGC_4) = 0x404u;
    v = REG(AGC_RFRX_SAT);
    if (on != 0u) {
        REG(AGC_RFRX_SAT) = v | 0xd1080000u;
        v = REG(AGC_RFRX_SAT);
        REG(AGC_RFRX_SAT) = (v & 0xfff80000u) | 0x800u;
    } else {
        REG(AGC_RFRX_SAT) = v & 0x2ef7ffffu;
        v = REG(AGC_RFRX_SAT);
        REG(AGC_RFRX_SAT) = (v & 0xfff80000u) | 0x400u;
    }
}

/*
 * libphy's phy_check_rx_sat [0x420070b4]: the pbus's debug mode, the receiver powered and its DCO pair
 * 0x01000100, 0x01000100 handed to the ROM -- the four halfwords 0x0100 -- the receive gain's trigger
 * raised, five microseconds, then a hundred readings of the receive gain's SAR, the ones at 0x45 and below
 * counted; where any counted, phy_param's rx_sat is set. The pbus's work mode is put back.
 */
void phy_check_rx_sat(void)
{
    uint32_t dco[2] = { 0x01000100u, 0x01000100u };
    uint16_t count = 0;

    pbus_debugmode();
    ram_pbus_xpd_rx_on(0u);
    pbus_set_dco(dco);
    phy_force_rx_gain_trig();
    ets_delay_us(5);
    for (uint16_t i = 0; i < 0x64u; i++) {
        if (((REG(AGC_GAIN_SAR) >> 12) & 0x7fu) <= 0x45u) {
            count = (uint16_t)(count + 1u);
        }
    }
    if (count != 0u) {
        phy_param[PHY_PARAM_RX_SAT] = 1u;
    }
    pbus_workmode();
}

/*
 * libphy's dc_iq_est_high [0x42005d18]: the IQ estimate armed through the ROM, its two accumulators
 * shifted six and divided by one more than the second argument, the magnitude's quotient eight times less
 * their squares' sum held at zero, turned to decibels and raised by eight into the third argument's third
 * word; the estimate then disabled. The first argument reaches only the ROM's arming, which does not read
 * it.
 */
void dc_iq_est_high(uint32_t a0, uint32_t a1, int32_t out[3])
{
    int32_t div = (int32_t)a1 + 1;
    int32_t i, q, sq, v;

    rom_iq_est_enable(a0, a1);
    i = ((int32_t)REG(FEDATA_IQ_EST_I) >> 6) / div;
    out[0] = i;
    q = ((int32_t)REG(FEDATA_IQ_EST_Q) >> 6) / div;
    out[1] = q;
    /* The squares wrap as the machine's words do. */
    sq = (int32_t)((uint32_t)i * (uint32_t)i + (uint32_t)q * (uint32_t)q);
    v = (int32_t)((uint32_t)((int32_t)REG(FEDATA_IQ_EST_MAG) / div) * 8u - (uint32_t)sq);
    if (v < 0) {
        v = 0;
    }
    out[2] = linear_to_db(v, 0) + 8;
    rom_iq_est_disable();
}

/*
 * libphy's rxiq_get_mis_new [0x42005d98]: the receive IQ mismatch from the estimate's four words. The
 * estimate is armed through the ROM with the count one shifted by the first argument's gain, the four
 * words shifted by the gain less two, and the two pairs' sum and difference crossed into a 64-bit divisor
 * -- the second pair's squares' sum, one where that is zero -- and two 64-bit numerators, each shifted,
 * divided and its low byte halved after one is added; the two bytes are the caller's. The third argument
 * asks for the library's print, which is left out.
 */
void rxiq_get_mis_new(uint32_t gain, uint8_t out[2], uint32_t verbose)
{
    int32_t w0, w1, w2, w3;
    int32_t x1, y1, x2, y2;
    uint32_t sh = (gain - 2u) & 31u;
    int64_t num, den;
    int8_t r0, r1;

    rom_iq_est_enable(1u, (1u << gain) & 0xffffu);
    w0 = (int32_t)REG(FEDATA_RX_IQ_EST2);
    w1 = (int32_t)REG(FEDATA_RX_IQ_EST5);
    w2 = (int32_t)REG(FEDATA_RX_IQ_EST4);
    w3 = (int32_t)REG(FEDATA_RX_IQ_EST3);
    w0 >>= sh;
    w1 >>= sh;
    w2 >>= sh;
    w3 >>= sh;
    x1 = w0 - w1;
    y1 = w2 + w3;
    x2 = w0 + w1;
    y2 = w3 - w2;
    den = (int64_t)x2 * x2 + (int64_t)y2 * y2;
    if (den == 0) {
        den = 1;
    }
    num = ((int64_t)x2 * x1 - (int64_t)y2 * y1) << 9;
    r0 = (int8_t)(int32_t)(num / den);
    num = ((int64_t)x2 * y1 + (int64_t)y2 * x1) << 10;
    r1 = (int8_t)(int32_t)(num / den);
    out[0] = (uint8_t)((r0 + 1) >> 1);
    out[1] = (uint8_t)((r1 + 1) >> 1);
    (void)verbose;
    rom_iq_est_disable();
}

/*
 * libphy's rxiq_cover_mg_mp_new [0x42005ef6]: the receive IQ mismatch's correction, over two rounds. Each
 * round hands the ROM's rxiq_set_reg the running pair -- the first the high correction, the second the low --
 * takes its answer, asks rxiq_get_mis_new for the estimate's mismatch, and folds the answer and the two bytes
 * into the pair; the second round's pair is held to the ROM's get_data_sat bounds, 0x1f and 0x3f, handed back
 * to rxiq_set_reg, and left in the caller's two bytes. The fourth argument asks rxiq_get_mis_new for the
 * library's print, which is left out.
 */

/* The ROM's rxiq_set_reg: one byte of the receive IQ correction into the front end's FEDATA_REG_SET3 word --
 * the high byte's field where the second argument is set, the low byte's otherwise -- its value held to 0x1f
 * or 0x3f and answered. */
int32_t rxiq_set_reg(int32_t value, int32_t high);

void rxiq_cover_mg_mp_new(uint32_t gain, uint8_t *out1, uint8_t *out2, uint32_t verbose)
{
    uint8_t buf[2];
    int32_t v1 = 0, v2 = 0, hi, lo;
    uint32_t round = 2u;

    for (;;) {
        hi = rxiq_set_reg(v1, 1);
        v2 = rxiq_set_reg(v2, 0);
        rxiq_get_mis_new(gain, buf, verbose);
        v1 = (int8_t)((int8_t)buf[0] + hi);
        v2 = (int8_t)(v2 - (int8_t)buf[1]);
        if (round != 1u) {
            round = 1u;
            continue;
        }
        hi = get_data_sat(v1, 0x1f, -0x1f);
        lo = get_data_sat(v2, 0x3f, -0x3f);
        rxiq_set_reg(hi, 1);
        rxiq_set_reg(lo, 0);
        *out1 = (uint8_t)hi;
        *out2 = (uint8_t)lo;
        break;
    }
}

/*
 * libphy's rfcal_rxiq_new [0x42005fba]: the front end's FEDATA_REG_SET3 word has bit 29 raised and bit 30
 * lowered, the ROM's tone started through the table -- 1, the caller's second and third arguments and zeros --
 * the mismatch correction taken from rxiq_cover_mg_mp_new with the first argument, the tone stopped, and the
 * pair written to the caller's two bytes. The last argument is rxiq_cover_mg_mp_new's verbose one; the
 * caller's second and third reach the tone alone.
 */
void rfcal_rxiq_new(uint32_t a0, uint32_t a1, uint32_t a2, uint8_t *out, uint32_t verbose)
{
    uint8_t buf[2];

    REG(FEDATA_REG_SET3) |= 0x20000000u;
    REG(FEDATA_REG_SET3) &= ~0x40000000u;
    ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_START_TX_TONE_STEP])(
        1u, a1, a2, 0u, 0u, 0u);
    rxiq_cover_mg_mp_new(a0, &buf[0], &buf[1], verbose);
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_STOP_TX_TONE])(1u);
    out[0] = buf[0];
    out[1] = buf[1];
}

/*
 * libphy's get_rfcal_rxiq_data_new [0x42006042]: the mismatch correction read up to four times through
 * rfcal_rxiq_new, its high and low bytes accumulated as signed halves, stopping early once a reading's two
 * bytes are within one of the last -- the last two then averaged -- else the four summed and divided by four,
 * two added first. The pair is held to the ROM's get_data_sat bounds (0x1f and 0x3f) and packed high byte
 * first into the answer's halfword. The third argument asks rfcal_rxiq_new for the library's print, which is
 * left out.
 */
int32_t get_rfcal_rxiq_data_new(int32_t a0, int32_t a1, uint32_t verbose)
{
    uint8_t buf[2];
    int32_t sum1 = 0, sum2 = 0, last1 = 0, last2 = 0, n = 0, v1 = 0, v2 = 0;
    int32_t p1, p2;

    for (;;) {
        rfcal_rxiq_new(0xeu, (uint32_t)a0, (uint32_t)a1, buf, verbose);
        if (n != 0 && abs_temp(last1 - (int8_t)buf[0]) <= 1 && abs_temp(last2 - (int8_t)buf[1]) <= 1) {
            v1 = (int8_t)((last1 + (int8_t)buf[0] + 1) >> 1);
            v2 = (last2 + (int8_t)buf[1] + 1) >> 1;
            break;
        }
        last1 = (int8_t)buf[0];
        last2 = (int8_t)buf[1];
        n++;
        sum1 = (int16_t)(sum1 + last1);
        sum2 = (int16_t)(sum2 + last2);
        if (n == 4) {
            v1 = (int8_t)((sum1 + 2) >> 2);
            v2 = (sum2 + 2) >> 2;
            break;
        }
    }
    p1 = get_data_sat(v1, 0x1f, -0x1f);
    p2 = get_data_sat(v2, 0x3f, -0x3f);
    return (int32_t)(uint16_t)(((int8_t)p1 << 8) | (uint8_t)p2);
}

/*
 * libphy's txdc_cal_pwdet [0x420084d8]: the transmit DC offset of the caller's halfword pair, one element at a
 * time. Each element is first bracketed by the power detector's reading of a tone with the DCO word above and
 * below it, the offsets stepping down by five until the two readings meet within 0x14; the offsets are then
 * swept in a run of up to 0x32 steps, the least reading kept, the run starting again from the other side, and
 * the reading after the first run's last step marks where the sweep stops; the run is smoothed to its neighbours
 * and the settled words averaged with the bracketing offsets. The second element starts from the pair the
 * caller wrote in. The library's prints of every step are left out. The first argument's buffer takes the pair.
 */
void txdc_cal_pwdet(uint16_t *dc, uint32_t verbose)
{
    uint16_t local[110];
    uint8_t codes[2];
    uint16_t *p = dc;
    uint32_t hi, lo, pass, grp, n, i, k, r, m;

    (void)verbose;
    local[0] = dc[0];
    local[1] = dc[1];
    local[2] = 0x100;
    local[3] = 0x100;

    i = 0;
    for (;;) {
        /* The bracketing: a reading with the DCO word raised, one lowered, until they meet. */
        pass = 2u;
        hi = 0xau;
        lo = 0xau;
        for (;;) {
            uint32_t a, b;

            local[i] = (uint16_t)(p[0] + (int16_t)hi);
            pbus_set_dco(local);
            ets_delay_us(0xau);
            a = get_tone_sar_dout(2u);
            local[i] = (uint16_t)(p[0] - (int16_t)lo);
            pbus_set_dco(local);
            ets_delay_us(0xau);
            b = get_tone_sar_dout(2u);
            if (b + 0x14u < a) {
                hi = (hi - 5u) & 0xffu;
            } else {
                if (b <= a + 0x14u) {
                    break;
                }
                lo = (lo - 5u) & 0xffu;
            }
            if (pass == 1u) {
                break;
            }
            pass = 1u;
        }

        /* The sweep: the offsets of one side then the other, each step's reading kept. */
        {
            uint8_t *c = codes;

            grp = 0u;
            n = 0u;
            for (;;) {
                k = 0u;
                m = 0xffffu;
                for (;;) {
                    int16_t off = (int16_t)((int16_t)hi + (int16_t)k);

                    if (grp != 0u) {
                        off = (int16_t)(-((int16_t)lo + (int16_t)k));
                    }
                    local[i] = (uint16_t)(p[0] + off);
                    pbus_set_dco(local);
                    ets_delay_us(0xau);
                    r = get_tone_sar_dout(2u);
                    local[n + 4u] = (uint16_t)r;
                    n = (n + 1u) & 0xffu;
                    if (r < m) {
                        m = r;
                    }
                    *c = (uint8_t)k;
                    if (!(k < 6u || r <= m + 0x1eu)) {
                        break;
                    }
                    k++;
                    if (k == 0x32u) {
                        break;
                    }
                }
                c++;
                if (grp != 0u) {
                    break;
                }
                grp = 1u;
            }
        }

        /* Each reading held to its neighbours, but those flanking the first run's last step to the run itself. */
        {
            uint32_t t6 = codes[0];

            for (k = 0u; k < n; k++) {
                if (k == 0u || t6 + 1u == k) {
                    if (local[k + 5u] < local[k + 4u]) {
                        local[k + 4u] = local[k + 5u];
                    }
                } else if (local[k + 4u] < local[k + 3u]) {
                    local[k + 4u] = local[k + 3u];
                }
            }

            /* The two offsets the element settles to, moved by the smoothed run and averaged. */
            {
                uint32_t r0 = local[4];
                uint32_t rt = local[t6 + 5u];
                uint32_t t3 = (uint32_t)abs_temp((int32_t)(r0 - rt));
                uint32_t t4;
                int32_t hv, lv;

                if ((t3 & 0xffffu) < 0x14u) {
                    t3 = 0x14u;
                }
                t3 &= 0xffu;
                t4 = (r0 < rt ? r0 : rt) & 0xffffu;
                hv = (int16_t)((int32_t)p[0] + (int16_t)hi);
                lv = (int16_t)((int32_t)p[0] - (int16_t)lo);
                for (k = 0u; k < n; k++) {
                    if ((uint32_t)local[k + 4u] <= t3 + t4) {
                        if (t6 < k) {
                            lv = (int16_t)((int32_t)p[0] + (int32_t)(t6 + 1u) - (int32_t)lo - (int32_t)k);
                        } else {
                            hv = (int16_t)((int32_t)p[0] + (int32_t)hi + (int32_t)k);
                        }
                    }
                }
                local[i] = (uint16_t)(((hv + lv + 1) / 2) + 1);
            }
        }

        p++;
        if (i == 1u) {
            dc[0] = local[0];
            dc[1] = local[1];
            pbus_set_dco(local);
            return;
        }
        i = 1u;
    }
}

/*
 * libphy's txdc_cal_pwdet_init [0x4200884a]: both pairs of the transmit DC offset settled and kept in phy_param.
 * The power detector and the pbus are brought up, and each pair is calibrated twice: the first over four runs,
 * their average kept, the second in one run. The first argument is written over before it is read, so what the
 * library's callers pass there does nothing; the second, clear, leaves the power detector's two words as they
 * were found on its way out. The library's prints are left out.
 */
void txdc_cal_pwdet_init(uint32_t a0, uint32_t a1)
{
    uint32_t prev0, prev3;
    uint32_t flag;
    uint16_t dc[2];

    (void)a0;
    phy_param16_set(PHY_PARAM_TXDC_CAL_PWDET, phy_param16(PHY_PARAM_TXDC));
    phy_param16_set((PHY_PARAM_TXDC_CAL_PWDET + 2u), phy_param16((PHY_PARAM_TXDC + 2u)));
    phy_param16_set((PHY_PARAM_TXDC_CAL_PWDET + 4u), phy_param16(PHY_PARAM_TXDC_SRC2));
    phy_param16_set((PHY_PARAM_TXDC_CAL_PWDET + 6u), phy_param16(PHY_PARAM_TXDC_SRC3));
    flag = (phy_param16(0x10u) >> 4) & 1u;

    prev3 = REG(FECTRL_PWDET_CONF3);
    prev0 = REG(FECTRL_PWDET_CONF0);
    REG(FECTRL_PWDET_CONF3) = (REG(FECTRL_PWDET_CONF3) & 0xffffff00u) | 0xf0u;
    REG(FECTRL_PWDET_CONF0) = (REG(FECTRL_PWDET_CONF0) & 0xfffff00fu) | 0x780u;

    set_txclk_en(1u);
    rom_en_pwdet(1u);
    pbus_debugmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_off();
    rom_start_tx_tone_step(1u, 0x200u, 0x78u, 0u, 0u, 0u);
    ets_delay_us(1u);
    rom_pbus_xpd_tx_on(0x3fu, 0u);

    for (uint32_t i = 0u; i < 2u; i++) {
        dc[0] = phy_param16(PHY_PARAM_TXDC_CAL_PWDET + i * 4u);
        dc[1] = phy_param16((PHY_PARAM_TXDC_CAL_PWDET + 2u) + i * 4u);
        rom_pbus_force_test(1u, 2u, index_to_txbbgain(i));
        if (i == 0u) {
            uint32_t acc_i = 0u, acc_q = 0u;

            for (uint32_t k = 0u; k < 4u; k++) {
                txdc_cal_pwdet(dc, flag);
                acc_i = (acc_i + dc[0]) & 0xffffu;
                acc_q = (acc_q + dc[1]) & 0xffffu;
            }
            phy_param16_set(PHY_PARAM_TXDC_CAL_PWDET, (uint16_t)(((int32_t)acc_i + 2) >> 2));
            phy_param16_set((PHY_PARAM_TXDC_CAL_PWDET + 2u), (uint16_t)(((int32_t)acc_q + 2) >> 2));
        } else {
            txdc_cal_pwdet(dc, flag);
            phy_param16_set((PHY_PARAM_TXDC_CAL_PWDET + 4u), dc[0]);
            phy_param16_set((PHY_PARAM_TXDC_CAL_PWDET + 6u), dc[1]);
        }
    }

    if (a1 == 0u) {
        uint16_t dco[4] = { 0x100, 0x100, 0x100, 0x100 };

        pbus_set_dco(dco);
        rom_pbus_xpd_tx_off();
        rom_start_tx_tone_step(0u, 0x80u, 0x78u, 0u, 0u, 0u);
        pbus_workmode();
        set_txclk_en(0u);
        REG(FECTRL_PWDET_CONF3) = (REG(FECTRL_PWDET_CONF3) & 0xffffff00u) | (prev3 & 0xffu);
        REG(FECTRL_PWDET_CONF0) = (prev0 & 0xff0u) | (REG(FECTRL_PWDET_CONF0) & 0xfffff00fu);
    }
}

/*
 * The front end's transmit DC calibration word, which txdc_cal_new steps a pair of offsets by. No header names
 * it; its meaning is not known beyond what the loop does with it, and each bit below says that.
 */
#define FEDATA_TXDC_CTRL        (MODEM_BASE + 0x418u)
#define FEDATA_TXDC_CTRL_START0 (1u << 0)  /* the loop clears it then sets it with the next, before each wait */
#define FEDATA_TXDC_CTRL_START1 (1u << 1)  /* set before each wait as well; both meanings are not known */
#define FEDATA_TXDC_CTRL_READY  (1u << 22) /* the wait ends when it is set */
#define FEDATA_TXDC_CTRL_STEP2  (1u << 29) /* set: the window-2 offset steps down, else up */
#define FEDATA_TXDC_CTRL_STEP3  (1u << 28) /* set: the window-3 offset steps down, else up */

/* The pbus windows the two offsets are forced into, which pbus_set_dco uses as (2,1), (3,1), (2,2), (3,2). */
#define TXDC_WINDOW2 2u
#define TXDC_WINDOW3 3u

static uint32_t fe_txdc_step(uint32_t value, uint32_t step, uint32_t down)
{
    int32_t v = down != 0u ? (int16_t)value - (int16_t)step : (int16_t)value + (int16_t)step;

    if (v < 0) {
        v = 0;
    }
    if (v > 0x1ff) {
        v = 0x1ff;
    }
    return (uint32_t)v;
}

/*
 * libphy's txdc_cal_new [0x42008a6a]: one pair of the front end's transmit DC offsets, settled by a twelve-step
 * bisection. A tone is started, then each step forces the two offsets into the front end's windows 3 and 2 and
 * waits for its calibration word's ready bit; each offset then moves by the step's size -- down where the word's
 * own decision bit says so, up otherwise -- held between 0 and 0x1ff. The step halves, plus one, down to one,
 * and the last four steps' offsets are summed and averaged; the pair is written both into the caller's buffer,
 * which pbus_set_dco takes, and to the front end, and the tone is stopped. The word's bits are the loop's own.
 */
void txdc_cal_new(uint16_t dco[4])
{
    uint32_t step = 0x7cu;
    uint32_t w3 = 0x100u; /* the offset forced into window 3, and its window-2 partner */
    uint32_t w2 = 0x100u;
    int32_t sum3 = 0, sum2 = 0;

    rom_pbus_force_test(TXDC_WINDOW2, 2u, 0x100u);
    rom_pbus_force_test(TXDC_WINDOW3, 2u, 0x100u);
    dco[2] = 0x100;
    dco[3] = 0x100;
    set_txclk_en(1u);
    rom_start_tx_tone_step(1u, 0x258u, 0x78u, 0u, 0u, 0u);

    for (uint32_t i = 0u; i < 12u; i++) {
        uint32_t w3_was = w3;
        uint32_t reg2, reg3;

        rom_pbus_force_test(TXDC_WINDOW3, 1u, w3);
        rom_pbus_force_test(TXDC_WINDOW2, 1u, w2);
        ets_delay_us(2u);
        REG(FEDATA_TXDC_CTRL) = REG(FEDATA_TXDC_CTRL) | FEDATA_TXDC_CTRL_START1;
        REG(FEDATA_TXDC_CTRL) = REG(FEDATA_TXDC_CTRL) & ~FEDATA_TXDC_CTRL_START0;
        REG(FEDATA_TXDC_CTRL) = REG(FEDATA_TXDC_CTRL) | FEDATA_TXDC_CTRL_START0;
        do {
            reg2 = REG(FEDATA_TXDC_CTRL);
        } while ((reg2 & FEDATA_TXDC_CTRL_READY) == 0u);
        /* The library reads the word twice more, once per offset's decision bit; the first read's value is
         * the wait's, the two below are the two it uses. */
        reg2 = REG(FEDATA_TXDC_CTRL);
        reg3 = REG(FEDATA_TXDC_CTRL);
        w2 = fe_txdc_step(w2, step, reg2 & FEDATA_TXDC_CTRL_STEP2);
        w3 = fe_txdc_step(w3_was, step, reg3 & FEDATA_TXDC_CTRL_STEP3);
        step = step == 2u ? 1u : (step >> 1) + 1u;
        if (i > 7u) {
            sum3 = (int16_t)(sum3 + (int32_t)w3);
            sum2 = (int16_t)(sum2 + (int32_t)w2);
        }
    }

    {
        uint32_t v3 = (uint32_t)((sum3 + 2) >> 2);
        uint32_t v2 = (uint32_t)((sum2 + 2) >> 2);

        rom_pbus_force_test(TXDC_WINDOW3, 1u, v3 & 0xffffu);
        rom_pbus_force_test(TXDC_WINDOW2, 1u, v2 & 0xffffu);
        dco[0] = (uint16_t)v2;
        dco[1] = (uint16_t)v3;
        REG(FEDATA_TXDC_CTRL) = REG(FEDATA_TXDC_CTRL) & ~FEDATA_TXDC_CTRL_START1;
        REG(FEDATA_TXDC_CTRL) = REG(FEDATA_TXDC_CTRL) & ~FEDATA_TXDC_CTRL_START0;
    }
    rom_start_tx_tone_step(0u, 0x258u, 0x78u, 0u, 0u, 0u);
}

/*
 * The transmitter's IQ correction, which the library's bring-up and its second tracking block both run through
 * txiq_cal_init_new: over seven rounds a tone's power is measured with the correction pair raised and lowered,
 * each round's mismatch byte pair accumulated, and the pair the caller keeps settled. The leaves are the ROM's,
 * called as the library's does. Each is held to the library's off the board, by phy-ab.py; build.mk weakens the
 * library's three.
 */

/* The ROM's own leaves, which the driver cannot weaken and calls as the library's does. */
int32_t txiq_set_reg(int32_t value, int32_t high);
void txiq_get_mis_pwr(int32_t first, int32_t atten, int32_t second, int16_t *out1, int16_t *out2);
uint32_t get_power_atten(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4);
void txcal_work_mode(void);

/* The ROM's table slots this subtree reaches, past the ones already named. */
#define ROMFUNC_LOOPBACK_MODE_EN    3  /* rom_loopback_mode_en: the loopback's transmitter and receiver clocks */
#define ROMFUNC_TXCAL_DEBUG_MODE   37  /* rom_txcal_debuge_mode: the transmitter's calibration debug mode */

static void rom_loopback_mode_en(uint32_t on)
{
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_LOOPBACK_MODE_EN])(on);
}

static void rom_txcal_debuge_mode(void)
{
    ((void (*)(void))phy_get_romfuncs()[ROMFUNC_TXCAL_DEBUG_MODE])();
}

/* The transmit tone generator's first word, which the ROM's ram_start_tx_tone_step and ram_stop_tx_tone write and
 * txiq_get_mis_pwr writes its measurement into; no header names it, and the ROM's own functions are the source. */
#define FEDATA_TX_TONE0 (MODEM_BASE + 0x420u)

/* The two bits of the front end's word FEDATA_REG_SET3 this calibration moves; rfcal_txiq_new's loop is the only
 * source, whose role is all that is said of them. */
#define FEDATA_REG_SET3_TXCAL     (1u << 13) /* raised before the calibration runs and left raised */
#define FEDATA_REG_SET3_TXIQ_DONE (1u << 14) /* lowered while it runs and raised at its end */

/* phy_param's two bytes the TXIQ calibration starts from; no label names either, so the meaning is not known. */
#define PHY_PARAM_TXIQ_TEMP  0x18 /* read as a signed byte, taken as the calibration's temperature */
#define PHY_PARAM_TXIQ_ATTEN 0x2B /* read as an unsigned byte, taken as 0x40 minus it */

/*
 * libphy's txiq_cover_new [0x42008e3a]: the transmitter's IQ correction pair, found over seven rounds.
 * Each round measures both sides of the pair -- the first argument's low byte less 0x0c, held at zero below it,
 * is the attenuation -- and writes their mismatch into the caller's two bytes; the first three rounds move the
 * pair down by them, the next four gather them, and the seventh moves the pair down by the four rounds' average
 * as well. The pair is then written into the caller's bytes. The library's own caller passes clear flags for the
 * last two arguments, so its prints never run; they, and the round buffer they walk, are left out.
 */
void txiq_cover_new(int32_t param1, int32_t param2, int8_t *out, uint32_t summary, uint32_t verbose)
{
    int16_t out1, out2;
    int32_t v1 = 0, v2 = 0;     /* the pair's two bytes, moved down round by round */
    int32_t acc1 = 0, acc2 = 0; /* the last four rounds' mismatch bytes, kept in signed bytes */
    int32_t atten = (int32_t)(int8_t)(uint8_t)(param1 - 0xc);

    (void)summary;
    (void)verbose;
    if (atten < 0) {
        atten = 0;
    }
    atten &= 0xff;

    for (uint32_t i = 0u; i < 7u; i++) {
        int32_t min, d, sum, byte0, byte1;
        uint32_t u0, u1;

        v1 = txiq_set_reg(v1, 1);
        v2 = txiq_set_reg(v2, 0);
        txiq_get_mis_pwr(1, atten, param2, &out1, &out2);
        min = (int32_t)out1 < (int32_t)out2 ? (int32_t)out1 : (int32_t)out2;
        if (min == 0) {
            min = 1;
        }
        d = (int32_t)(((uint32_t)((int32_t)out2 - (int32_t)out1)) << 11);
        byte0 = (int8_t)((d / min + 0x10) >> 5);
        out[0] = (int8_t)byte0;

        txiq_get_mis_pwr(0, param1, param2, &out1, &out2);
        sum = (int16_t)((int32_t)out1 + (int32_t)out2);
        if (sum == 0) {
            sum = 1;
        }
        d = (int32_t)(((uint32_t)((int32_t)out1 - (int32_t)out2)) << 12);
        byte1 = (int8_t)((d / sum + 8) >> 4);
        out[1] = (int8_t)byte1;

        u0 = (uint8_t)out[0];
        u1 = (uint8_t)out[1];
        if (i < 3u) {
            v1 = (int8_t)(v1 - (int32_t)u0);
            v2 = (int8_t)(v2 - (int32_t)u1);
        } else {
            acc1 = (int8_t)(acc1 + (int32_t)u0);
            acc2 = (int8_t)(acc2 + (int32_t)u1);
            if (i == 6u) {
                v1 = (int8_t)(v1 - ((acc1 + 2) >> 2));
                v2 = (int8_t)(v2 - ((acc2 + 2) >> 2));
            }
        }
    }

    (void)txiq_set_reg(v1, 1);
    (void)txiq_set_reg(v2, 0);
    out[0] = (int8_t)v1;
    out[1] = (int8_t)v2;
}

/*
 * libphy's rfcal_txiq_new [0x42009066]: one side of the transmitter's IQ correction, measured and kept in the
 * caller's halfword. The front end's word FEDATA_REG_SET3 has its TXIQ bit raised and its done bit lowered, then
 * the transmitter is put into the mode the last argument names -- 1 reads a pbus register and forces its low bit
 * with the tone, 2 turns the loopback on and settles the transmit DC pair, anything else only forces the transmit
 * DC pair -- and the tone is measured at the attenuation the fourth argument names and the temperature the fifth
 * names, over the same correction the library's does; the pair that answers is held to its limits and packed into
 * the caller's halfword. The tone generator's word is kept and put back, and in mode 2 the loopback is turned off
 * again and the done bit raised. The library's prints are left out.
 */
void rfcal_txiq_new(uint32_t param1, uint16_t *dc, uint16_t *out, uint32_t atten, uint32_t temp, uint32_t mode)
{
    int8_t cal[2];
    uint32_t saved, att;
    int32_t a, b;

    (void)param1; /* the library's first argument its own rom_txcal_debuge_mode ignores too */
    REG(FEDATA_REG_SET3) |= FEDATA_REG_SET3_TXCAL;
    REG(FEDATA_REG_SET3) &= ~FEDATA_REG_SET3_TXIQ_DONE;
    rom_txcal_debuge_mode();
    rom_pbus_force_test(1u, 2u, 0u);
    if (mode == 1u) {
        uint32_t v = rom_pbus_rd(1u, 1u);

        rom_pbus_force_test(1u, 1u, (v | 2u) & 0xffffu);
        pbus_set_dco(dc);
    } else if (mode == 2u) {
        rom_loopback_mode_en(1u);
        txdc_cal_new(dc);
    } else {
        pbus_set_dco(dc);
    }

    saved = REG(FEDATA_TX_TONE0);
    att = get_power_atten((int32_t)(int16_t)atten, (int32_t)temp,
                          (int32_t)(uint8_t)(0x40u - phy_param[PHY_PARAM_TXIQ_ATTEN]),
                          (int32_t)phy_param16(0x0eu), 0);
    txiq_cover_new((int32_t)(att & 0xffu), (int32_t)(int16_t)atten, cal, 0u, 0u);
    txcal_work_mode();
    a = get_data_sat((int32_t)cal[0], 0x1f, -0x1f);
    cal[0] = (int8_t)a;
    b = get_data_sat((int32_t)cal[1], 0x3f, -0x3f);
    cal[1] = (int8_t)b;
    *out = (uint16_t)((((uint32_t)(uint8_t)cal[0]) << 7) & 0x1f80u) | ((uint32_t)b & 0x7fu);
    REG(FEDATA_TX_TONE0) = saved;
    if (mode == 2u) {
        rom_loopback_mode_en(0u);
    }
    REG(FEDATA_REG_SET3) |= FEDATA_REG_SET3_TXIQ_DONE;
}

/*
 * libphy's txiq_cal_init_new [0x420091e0]: the transmitter's IQ correction, which the library's bring-up and its
 * second tracking block both call. Once its done bit, param_flag's bit 14, is set it does nothing; otherwise it
 * calibrates the forward pair from the temperature byte 0x18, then, at the temperature less 0x14 held between 0
 * and 0x78, the loopback pair, and marks the correction done.
 */
void txiq_cal_init_new(void)
{
    uint16_t dc[4];
    int16_t temp;
    int32_t att;

    if ((phy_param32(PHY_PARAM_PARAM_FLAG) & 0x4000u) != 0u) {
        return;
    }
    temp = (int16_t)(int8_t)phy_param[PHY_PARAM_TXIQ_TEMP];
    rfcal_txiq_new(0u, (uint16_t *)(void *)&phy_param[PHY_PARAM_TXDC],
                   (uint16_t *)(void *)&phy_param[PHY_PARAM_TXIQ], 0x80u,
                   (uint32_t)(int32_t)(int8_t)(temp < 0 ? 0 : temp), 0u);
    att = get_data_sat((int32_t)(int16_t)((temp < 0 ? 0 : (int32_t)temp) - 0x14), 0x78, 0);
    rfcal_txiq_new(0u, dc, (uint16_t *)(void *)&phy_param[PHY_PARAM_LB_TXIQ], 0x80u,
                   (uint32_t)(int32_t)(int8_t)att, 2u);
    phy_param32_set(PHY_PARAM_PARAM_FLAG, phy_param32(PHY_PARAM_PARAM_FLAG) | 0x4000u);
}

/*
 * libphy's phy_cal_param_track [0x42008008], the two blocks the library's tracking reruns once the chip has
 * drifted far enough from the temperature each was last run at. The leaves are the driver's own.
 */

/* phy_param's fields these two thresholds are kept in; no label names any of them. */
/* phy_param's debug override of the two steps, which phy_track_temp_debug writes: its bit 1 turns the byte
 * beside it into the step, else the block's own below. */
#define PHY_PARAM_TRACK_TEMP_DEBUG    0x1A5
#define PHY_PARAM_TRACK_TEMP_DEBUG_TH 0x1A6
#define TRACK_TEMP_DEBUG_ON 0x2u
#define TRACK_STEP_RXDC     0x1eu
#define TRACK_STEP_TXCAL    0x28u

/* The ROM's table slot the baseband's 40 MHz width is set through, which libphy fills with its own. */
static void rom_bb_bss_cbw40(uint32_t on)
{
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_BB_BSS_CBW40])(on);
}

/* The library's debug override of the two steps: its bit 1 turns the byte beside it into the step, else the
 * block's own. */
static uint32_t track_step(uint32_t fallback)
{
    return (phy_param[PHY_PARAM_TRACK_TEMP_DEBUG] & TRACK_TEMP_DEBUG_ON) != 0u
               ? (uint32_t)phy_param[PHY_PARAM_TRACK_TEMP_DEBUG_TH]
               : fallback;
}

/*
 * The first block, over the receive rows' temperature, at 0x1e or the override: it brings the frequency
 * setting down, forces TX and RX off, clears the pbus, wipes the rows' flags, rebuilds them and retunes the
 * channel, then puts the baseband's enable bit back as it was, lets TX and RX go and brings the setting up.
 * The second, over the transmit calibration's temperature at 0x28 or the override, does the same up to the
 * pbus, takes the baseband down, runs the IQ and transmit-DC calibrations and the transmit gains, puts its
 * temperature down, and puts the baseband back. The library's prints, under its first argument, are left out.
 */
void phy_cal_param_track(uint32_t verbose)
{
    uint32_t chan, second, was;

    (void)verbose;
    if (abs_temp((int32_t)(int16_t)phy_param16(PHY_PARAM_RX_DC_T) -
                 (int32_t)(int16_t)phy_param16(PHY_PARAM_TEMP_CODE)) >=
        (int32_t)track_step(TRACK_STEP_RXDC)) {
        phy_dis_hw_set_freq_new();
        force_txrx_off(1u);
        pbus_clear_reg();
        chan = phy_param16(PHY_PARAM_CHANNEL);
        second = (uint32_t)(int32_t)(int8_t)phy_param[PHY_PARAM_CHANNEL_SECOND];
        was = REG(MODEM_SYSCON_WIFI_BB_CFG);
        REG(MODEM_SYSCON_WIFI_BB_CFG) &= ~0x2u;
        phy_param32_set(PHY_PARAM_PARAM_FLAG, phy_param32(PHY_PARAM_PARAM_FLAG) & ~0x280u);
        set_rx_gain_table(0x985u, 0u);
        chip_v7_set_chan(chan, second);
        /* The library's shifts of the two just-saved words come to the same: the enable bit back as it was,
         * the rest from the word as it now is. */
        REG(MODEM_SYSCON_WIFI_BB_CFG) = ((REG(MODEM_SYSCON_WIFI_BB_CFG) | was) & 0x2u) |
                                        (REG(MODEM_SYSCON_WIFI_BB_CFG) & ~0x2u);
        force_txrx_off(0u);
        phy_en_hw_set_freq_new();
        phy_param16_set(PHY_PARAM_RX_DC_T, phy_param16(PHY_PARAM_TEMP_CODE));
    }

    if (abs_temp((int32_t)(int16_t)phy_param16(PHY_PARAM_TXCAL_TEMP) -
                 (int32_t)(int16_t)phy_param16(PHY_PARAM_TEMP_CODE)) >=
        (int32_t)track_step(TRACK_STEP_TXCAL)) {
        phy_dis_hw_set_freq_new();
        force_txrx_off(1u);
        pbus_clear_reg();
        was = REG(MODEM_SYSCON_WIFI_BB_CFG);
        chan = phy_param16(PHY_PARAM_CHANNEL);
        second = (uint32_t)(int32_t)(int8_t)phy_param[PHY_PARAM_CHANNEL_SECOND];
        REG(MODEM_SYSCON_WIFI_BB_CFG) &= ~0x2u;
        rom_bb_bss_cbw40(0u);
        txiq_cal_init_new();
        txdc_cal_pwdet_init(0u, 0u);
        wifi_set_tx_gain(chan, 0u);
        phy_param16_set(PHY_PARAM_TXCAL_TEMP, phy_param16(PHY_PARAM_TEMP_CODE));
        rom_bb_bss_cbw40(second);
        REG(MODEM_SYSCON_WIFI_BB_CFG) = ((REG(MODEM_SYSCON_WIFI_BB_CFG) | was) & 0x2u) |
                                        (REG(MODEM_SYSCON_WIFI_BB_CFG) & ~0x2u);
        force_txrx_off(0u);
        phy_en_hw_set_freq_new();
    }
}
/*
 * libphy's phy_param_track_tot [0x42008204]: the whole tracking, which the driver's drv_phy_track runs once a
 * second while the station is joined. Inside the ROM's critical section, and only while neither the RF-off flag
 * nor the sensor's power flag is set: the RFPLL capacitance where its flag is set, Bluetooth's transmit power
 * where the second argument says so, Wi-Fi's -- the I2C track and the transmit power, and the parameter
 * calibration where its skip flag is clear -- and the sensor's temperature read. The library's prints are left
 * out.
 */

/* The ROM's table's critical-section pair, empty on this chip but called as the library's does. */
#define ROMFUNC_ENTER_CRITICAL_PHY 0
#define ROMFUNC_EXIT_CRITICAL_PHY  1

static uint32_t rom_enter_critical_phy(void)
{
    return ((uint32_t (*)(void))phy_get_romfuncs()[ROMFUNC_ENTER_CRITICAL_PHY])();
}

static void rom_exit_critical_phy(uint32_t v)
{
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_EXIT_CRITICAL_PHY])(v);
}

/* phy_param's bytes the tracking gates itself by; no label names them, and the loops are their source. */
#define PHY_PARAM_CAP_TRACK  0x0A  /* where set, the RFPLL capacitance tracking runs; phy_force_freq_set sets it */
#define PHY_PARAM_BT_TRACK   0x0B  /* passed to Bluetooth's transmit-power tracking, whose zero means it does nothing */
#define PHY_PARAM_TRACK_SKIP 0x19A /* where set, the parameter calibration is left out */

void phy_param_track_tot(uint32_t en_wifi, uint32_t en_ble_154)
{
    uint32_t crit = rom_enter_critical_phy();

    if (phy_param[PHY_PARAM_RF_OFF] == 0u && phy_param[PHY_PARAM_TSENS_XPD] == 0u) {
        if (phy_param[PHY_PARAM_CAP_TRACK] != 0u) {
            phy_rfpll_cap_track_new(phy_param[PHY_PARAM_TEMP]);
        }
        if (en_ble_154 != 0u) {
            bt_track_tx_power(phy_param[PHY_PARAM_BT_TRACK], phy_param[PHY_PARAM_TEMP]);
        }
        if (en_wifi != 0u) {
            tx_i2c_track();
            wifi_track_tx_power(1u, phy_param[PHY_PARAM_TEMP]);
            if (phy_param[PHY_PARAM_TRACK_SKIP] == 0u) {
                phy_cal_param_track(phy_param[PHY_PARAM_TEMP]);
            }
        }
        tsens_temp_read_new();
    }
    rom_exit_critical_phy(crit);
}

/*
 * The RF calibration's saved blob, the driver's own. register_chipv7_phy carries one in as its cal_data: the
 * driver fills it on the first calibration and holds it across a restart. Its head is the version
 * (phy_get_rf_cal_version) and the factory MAC; from CAL_PHY_PARAM it holds phy_param[0..0x1A8) as
 * phy_rfcal_data_sub copies it, and its last word is the negated sum of every word before it, which
 * phy_rfcal_data_check writes or checks. The fields' names are phy_cal_print's labels (PHYREGS.md).
 */
#define CAL_VERSION         0x00  /* four bytes: the calibration's version, 0x159 */
#define CAL_MAC             0x04  /* the factory MAC and two bytes of nothing */
#define CAL_PHY_PARAM       0x0C  /* phy_param[0..0x1A8) */
#define CAL_SAR_DC          0x26  /* phy_param[PHY_PARAM_SAR_DC]'s halfword */
#define CAL_SAR_REF         0x28  /* phy_param[PHY_PARAM_SAR_REF]'s halfword */
#define CAL_CHECKSUM        0x1B4 /* the negated sum of the words before it */
#define CAL_END             0x1B8

/* phy_param's fields of the blob that are restored on their own; no header names them, phy_cal_print prints them. */
#define PHY_PARAM_SAR_DC     0x1A /* sar_dc_code */
#define PHY_PARAM_SAR_REF    0x1C /* sar_ref_code */
#define PHY_PARAM_BT_TXDC    0x104 /* BT_TXDC, 0x104, 0x106, 0x108, 0x10A */
#define PHY_PARAM_BT_TXIQ    0x102 /* BT_TXIQ */
#define PHY_PARAM_WIFI_ATTEN 0xF4 /* wifi_cal_atten, 0xF4..0xF6 */
#define PHY_PARAM_BT_ATTEN   0xF8 /* bt_cal_atten, 0xF8..0xFA */
#define PHY_PARAM_DCAP_STG   0xDC /* DCAP_STG1 and DCAP_STG2, interleaved at 0xDC..0xE1 */

uint32_t phy_byte_to_word(const uint8_t *p); /* the ROM's four bytes, little-endian, as a word */

/* libphy's phy_get_rf_cal_version [0x42000324]: the version the blob's head carries. */
uint32_t phy_get_rf_cal_version(void)
{
    return 0x159u;
}

/*
 * libphy's get_iq_value [0x4200692c]: one packed receive pair split into its two signed halves, the first
 * from bits 7 to 12 (six bits), the second from bits 0 to 6 (seven). It is the receive IQ package's, but the
 * blob's comparison loops below call it, so it is taken here with them.
 */
void get_iq_value(uint8_t out[2], uint16_t pair)
{
    int8_t i = (int8_t)(pair >> 7);
    int8_t q = (int8_t)pair;
    int8_t a = (int8_t)(i & 0x3f);
    int8_t b = (int8_t)(q & 0x7f);

    if ((i & 0x20) != 0) {
        a = (int8_t)(a - 0x40);
    }
    if ((q & 0x40) != 0) {
        b = (int8_t)(b - 0x80);
    }
    out[0] = (uint8_t)a;
    out[1] = (uint8_t)b;
}

/* libphy's phy_get_mac_addr [0x42003930]: the factory MAC in its ordinary order, the eFuse's reversal undone. */
void phy_get_mac_addr(uint8_t *mac)
{
    uint8_t buf[8] = { 0 };

    esp_phy_efuse_get_mac(buf);
    for (int i = 0; i < 6; i++) {
        mac[i] = buf[5 - i];
    }
}

/* libphy's phy_set_mac_data [0x42003980]: the blob's head, the version as a word and then the MAC and the two
 * bytes beside it, which the caller's stack holds as zero. */
void phy_set_mac_data(uint8_t *blob, uint32_t version)
{
    uint8_t buf[8] = { 0 };

    blob[CAL_VERSION] = (uint8_t)version;
    blob[CAL_VERSION + 1u] = (uint8_t)(version >> 8);
    blob[CAL_VERSION + 2u] = (uint8_t)(version >> 16);
    blob[CAL_VERSION + 3u] = (uint8_t)(version >> 24);
    phy_get_mac_addr(buf);
    for (int i = 0; i < 8; i++) {
        blob[CAL_MAC + i] = buf[i];
    }
}

/*
 * libphy's phy_rfcal_data_sub [0x420039d2]: phy_param's block of the blob a word at a time, the blob's copy
 * saved where save is set (blob <- phy_param) and restored where it is not (phy_param <- blob).
 */
void phy_rfcal_data_sub(uint8_t *blob, int save)
{
    for (uint32_t off = 0; off < 0x1A8u; off += 4u) {
        uint32_t w = phy_byte_to_word(save ? &phy_param[off] : &blob[CAL_PHY_PARAM + off]);

        if (save) {
            blob[CAL_PHY_PARAM + off] = (uint8_t)w;
            blob[CAL_PHY_PARAM + off + 1u] = (uint8_t)(w >> 8);
            blob[CAL_PHY_PARAM + off + 2u] = (uint8_t)(w >> 16);
            blob[CAL_PHY_PARAM + off + 3u] = (uint8_t)(w >> 24);
        } else {
            phy_param[off] = (uint8_t)w;
            phy_param[off + 1u] = (uint8_t)(w >> 8);
            phy_param[off + 2u] = (uint8_t)(w >> 16);
            phy_param[off + 3u] = (uint8_t)(w >> 24);
        }
    }
}

/* libphy's rf_cal_data_backup [0x42003e0c]: the calibration, saved. */
void rf_cal_data_backup(uint8_t *blob)
{
    phy_rfcal_data_sub(blob, 1);
}

/*
 * libphy's rf_cal_data_recovery [0x42003a36]: the calibrated block, restored, and the two SAR codes on their
 * own; where register_chipv7_phy asks for a re-parameterisation rather than a full calibration.
 */
void rf_cal_data_recovery(const uint8_t *blob)
{
    memcpy(&phy_param[PHY_PARAM_PARAM_FLAG], &blob[CAL_PHY_PARAM + PHY_PARAM_PARAM_FLAG], 0xF4u);
    phy_param16_set(PHY_PARAM_SAR_DC, (uint16_t)(blob[CAL_SAR_DC] | ((uint16_t)blob[CAL_SAR_DC + 1u] << 8)));
    phy_param16_set(PHY_PARAM_SAR_REF,
                    (uint16_t)(blob[CAL_SAR_REF] | ((uint16_t)blob[CAL_SAR_REF + 1u] << 8)));
}

/*
 * libphy's phy_rfcal_data_check [0x42003e22]: the blob's version and MAC first, then the words' negated sum;
 * checked against the blob's own where check is set (1 where it is stale), stored where it is not. The third
 * argument, the init data, is passed and left alone.
 */
uint32_t phy_rfcal_data_check(uint32_t check, uint8_t *blob, const uint8_t *init_data, uint32_t version)
{
    uint32_t sum = 0;

    (void)init_data;
    phy_set_mac_data(blob, version);
    for (uint32_t off = 0; off < CAL_CHECKSUM; off += 4u) {
        sum += phy_byte_to_word(&blob[off]);
    }
    sum = ~sum;
    if (check != 0u) {
        return (sum - phy_byte_to_word(&blob[CAL_CHECKSUM])) != 0u;
    }
    blob[CAL_CHECKSUM] = (uint8_t)sum;
    blob[CAL_CHECKSUM + 1u] = (uint8_t)(sum >> 8);
    blob[CAL_CHECKSUM + 2u] = (uint8_t)(sum >> 16);
    blob[CAL_CHECKSUM + 3u] = (uint8_t)(sum >> 24);
    return 0;
}

/* The layout phy_rfcal_data_check_value keeps in the caller's buffer, as its stores and comparisons walk it. */
#define RFCAL_BUF_TXDC       0x00 /* 20 halfwords: phy_param[PHY_PARAM_TXDC..+0x28) */
#define RFCAL_BUF_BT_TXDC    0x28 /* 12 halfwords: phy_param[PHY_PARAM_BT_TXDC..+0x18) */
#define RFCAL_BUF_IQ         0x40 /* 6 halfwords: phy_param[PHY_PARAM_TXIQ..+0x0C) */
#define RFCAL_BUF_BT_TXIQ    0x4C /* phy_param[PHY_PARAM_BT_TXIQ] */
#define RFCAL_BUF_WIFI_ATTEN 0x4E /* 3 bytes: phy_param[PHY_PARAM_WIFI_ATTEN..+3) */
#define RFCAL_BUF_BT_ATTEN   0x51 /* 3 bytes: phy_param[PHY_PARAM_BT_ATTEN..+3) */
#define RFCAL_BUF_DCAP       0x54 /* 9 bytes: phy_param[PHY_PARAM_DCAP_STG..+9) */
#define RFCAL_BUF_TXDC_PWDET 0x5E /* 4 halfwords: phy_param[PHY_PARAM_TXDC_CAL_PWDET..+8) */
#define RFCAL_BUF_END        0x66

/* The tracking's own threshold on a difference, and the ROM's magnitude: the comparison's decision. */
static int rfcal_changed(int32_t a, int32_t b)
{
    return abs_temp(a - b) > 4;
}

static uint16_t rfcal_buf16(const uint8_t *buf, uint32_t off)
{
    return (uint16_t)(buf[off] | ((uint16_t)buf[off + 1u] << 8));
}

/*
 * libphy's phy_rfcal_data_check_value [0x42003a76]: with mode zero, the blob's calibration is restored into
 * phy_param and its fields are copied into the caller's buffer; with mode set, that buffer is held against
 * phy_param field by field and the number that moved by more than four is returned. The caller's buffer is
 * 0x66 bytes, the layout above. This is how register_chipv7_phy decides whether a saved calibration still
 * stands; the IQ fields go through get_iq_value's unpacking first.
 */
uint32_t phy_rfcal_data_check_value(uint8_t *blob, uint8_t *buf, uint32_t mode)
{
    uint32_t count = 0;

    if (mode == 0u) {
        rf_cal_data_recovery(blob);
        for (uint32_t i = 0; i < 0x28u; i += 2u) {
            uint16_t v = phy_param16(PHY_PARAM_TXDC + i);

            buf[RFCAL_BUF_TXDC + i] = (uint8_t)v;
            buf[RFCAL_BUF_TXDC + i + 1u] = (uint8_t)(v >> 8);
        }
        for (uint32_t i = 0; i < 0x18u; i += 2u) {
            uint16_t v = phy_param16(PHY_PARAM_BT_TXDC + i);

            buf[RFCAL_BUF_BT_TXDC + i] = (uint8_t)v;
            buf[RFCAL_BUF_BT_TXDC + i + 1u] = (uint8_t)(v >> 8);
        }
        for (uint32_t i = 0; i < 0x0Cu; i += 2u) {
            uint16_t v = phy_param16(PHY_PARAM_TXIQ + i);

            buf[RFCAL_BUF_IQ + i] = (uint8_t)v;
            buf[RFCAL_BUF_IQ + i + 1u] = (uint8_t)(v >> 8);
        }
        uint16_t bt_txiq = phy_param16(PHY_PARAM_BT_TXIQ);

        buf[RFCAL_BUF_BT_TXIQ] = (uint8_t)bt_txiq;
        buf[RFCAL_BUF_BT_TXIQ + 1u] = (uint8_t)(bt_txiq >> 8);
        for (uint32_t i = 0; i < 3u; i++) {
            buf[RFCAL_BUF_WIFI_ATTEN + i] = phy_param[PHY_PARAM_WIFI_ATTEN + i];
            buf[RFCAL_BUF_BT_ATTEN + i] = phy_param[PHY_PARAM_BT_ATTEN + i];
        }
        for (uint32_t i = 0; i < 9u; i++) {
            buf[RFCAL_BUF_DCAP + i] = phy_param[PHY_PARAM_DCAP_STG + i];
        }
        for (uint32_t i = 0; i < 8u; i += 2u) {
            uint16_t v = phy_param16(PHY_PARAM_TXDC_CAL_PWDET + i);

            buf[RFCAL_BUF_TXDC_PWDET + i] = (uint8_t)v;
            buf[RFCAL_BUF_TXDC_PWDET + i + 1u] = (uint8_t)(v >> 8);
        }
        return 0;
    }

    for (uint32_t i = 0; i < 5u; i++) {
        count += (uint32_t)rfcal_changed((int32_t)rfcal_buf16(buf, RFCAL_BUF_TXDC + 8u * i),
                                         (int32_t)phy_param16(PHY_PARAM_TXDC + 8u * i));
        count += (uint32_t)rfcal_changed((int32_t)rfcal_buf16(buf, RFCAL_BUF_TXDC + 8u * i + 2u),
                                         (int32_t)phy_param16(PHY_PARAM_TXDC + 8u * i + 2u));
    }
    for (uint32_t i = 0; i < 4u; i++) {
        count += (uint32_t)rfcal_changed((int32_t)rfcal_buf16(buf, RFCAL_BUF_TXDC_PWDET + 2u * i),
                                         (int32_t)phy_param16(PHY_PARAM_TXDC_CAL_PWDET + 2u * i));
    }
    for (uint32_t i = 0; i < 3u; i++) {
        for (uint32_t j = 0; j < 2u; j++) {
            count += (uint32_t)rfcal_changed((int32_t)rfcal_buf16(buf, RFCAL_BUF_BT_TXDC + 8u * i + 2u * j),
                                             (int32_t)phy_param16(PHY_PARAM_BT_TXDC + 8u * i + 2u * j));
        }
    }
    for (uint32_t i = 0; i < 6u; i++) {
        uint8_t a[2], b[2];

        get_iq_value(a, rfcal_buf16(buf, RFCAL_BUF_IQ + 2u * i));
        get_iq_value(b, phy_param16(PHY_PARAM_TXIQ + 2u * i));
        count += (uint32_t)rfcal_changed((int32_t)(int8_t)a[0], (int32_t)(int8_t)b[0]);
        count += (uint32_t)rfcal_changed((int32_t)(int8_t)a[1], (int32_t)(int8_t)b[1]);
    }
    {
        uint8_t a[2], b[2];

        get_iq_value(a, rfcal_buf16(buf, RFCAL_BUF_BT_TXIQ));
        get_iq_value(b, phy_param16(PHY_PARAM_BT_TXIQ));
        count += (uint32_t)rfcal_changed((int32_t)(int8_t)a[0], (int32_t)(int8_t)b[0]);
        count += (uint32_t)rfcal_changed((int32_t)(int8_t)a[1], (int32_t)(int8_t)b[1]);
    }
    for (uint32_t i = 0; i < 3u; i++) {
        count += (uint32_t)rfcal_changed((int32_t)(int8_t)buf[RFCAL_BUF_WIFI_ATTEN + i],
                                         (int32_t)(int8_t)phy_param[PHY_PARAM_WIFI_ATTEN + i]);
        count += (uint32_t)rfcal_changed((int32_t)(int8_t)buf[RFCAL_BUF_BT_ATTEN + i],
                                         (int32_t)(int8_t)phy_param[PHY_PARAM_BT_ATTEN + i]);
    }
    for (uint32_t i = 0; i < 9u; i++) {
        count += (uint32_t)rfcal_changed((int32_t)buf[RFCAL_BUF_DCAP + i], (int32_t)phy_param[PHY_PARAM_DCAP_STG + i]);
    }
    return count;
}

/*
 * libphy's rc_cal_new [0x4200014a]: the analog RC oscillator's DAC read once (the ROM's get_rc_dout, which
 * settles the RC through i2c_txrf and answers its register 0x0E), turned into the receive chain's rc_dout
 * bytes -- one raw, two for Wi-Fi and four for Bluetooth, each held between the ROM's get_data_sat bounds.
 * Its three arguments are byte tables, the library's constants, which rc_cal_init passes on its stack. When
 * the run is done it sets param_flag's bit 23, and a second call returns at once; the gate is what the
 * library tests before the read. The divisions are signed and truncate, and each result is a signed halfword.
 */
void rc_cal_new(const uint8_t *spans, const uint8_t *divs, const uint8_t *coeffs)
{
    uint8_t dout;
    uint32_t base;
    int32_t s2;

    if ((phy_param32(PHY_PARAM_PARAM_FLAG) & 0x800000u) != 0u) {
        return;
    }
    dout = get_rc_dout();
    phy_param[PHY_PARAM_RC_DOUT] = dout;
    base = dout <= 0x28u ? dout : 0x28u; /* the library holds the read in a byte and caps it at 0x28 */
    base += 0x38u;
    s2 = (int32_t)(base * 0x52u);
    phy_param[PHY_PARAM_RC_DOUT_WIFI] =
        (uint8_t)get_data_sat((int32_t)(int16_t)(s2 / ((int32_t)divs[0] * 0xa) - 8), spans[0], 2);
    s2 = s2 / ((int32_t)divs[1] * 0xa) - 8;
    phy_param[PHY_PARAM_RC_DOUT_WIFI + 1u] = (uint8_t)get_data_sat((int32_t)(int16_t)s2, spans[1], 2);
    for (uint32_t i = 0; i < 4u; i++) {
        int32_t v = (int32_t)(base * 0x334u) / ((int32_t)coeffs[i] * 0x68) - 8;

        phy_param[PHY_PARAM_RC_DOUT_BT + i] = (uint8_t)get_data_sat((int32_t)(int16_t)v, spans[2u + (i & 1u)], 0);
    }
    phy_param32_set(PHY_PARAM_PARAM_FLAG, phy_param32(PHY_PARAM_PARAM_FLAG) | 0x800000u);
}

/*
 * libphy's rc_cal_init [0x4200352a]: rc_cal_new's three byte tables, the library's constants, which it
 * keeps on its stack; its words are 0x141E1428, 0x2814 and 0x20163224, little-endian.
 */
void rc_cal_init(void)
{
    static const uint8_t spans[4] = { 0x28, 0x14, 0x1e, 0x14 };
    static const uint8_t divs[2] = { 0x14, 0x28 };
    static const uint8_t coeffs[4] = { 0x24, 0x32, 0x16, 0x20 };

    rc_cal_new(spans, divs, coeffs);
}

/*
 * libphy's i2c_rc_cal_set [0x42002d1a]: the RC calibration's analog fields on i2c_txrf, host 1 -- 0x0B's
 * bits 4..5 from the first argument, 0x08's bits 3..7 from the second and 0x0D's bits 2..5 to 9 -- each
 * through the ROM's rom_i2c_writeReg_Mask, the third as the library tail-calls it.
 */
void i2c_rc_cal_set(uint32_t first, uint32_t second)
{
    rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, I2C_TXRF_RC_CAL_0B, I2C_TXRF_RC_CAL_0B_MSB, I2C_TXRF_RC_CAL_0B_LSB,
                         (uint8_t)first);
    rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, I2C_TXRF_RC_CAL_08, I2C_TXRF_RC_CAL_08_MSB, I2C_TXRF_RC_CAL_08_LSB,
                         (uint8_t)second);
    rom_i2c_write_masked(I2C_TXRF, I2C_HOST_MST, I2C_TXRF_RC_CAL_0D, I2C_TXRF_RC_CAL_0D_MSB, I2C_TXRF_RC_CAL_0D_LSB, 9u);
}

/*
 * The ROM's table of its PHY functions, which the ROM's own calls reach through `phy_get_romfuncs()`.
 * libphy's own `phy_get_romfunc_addr` [0x4200340c] hands the ROM the address of the library's `phy_param`
 * and fills twenty-three of the table's forty-four slots with the library's own functions, so that the ROM's
 * calls land in the library; it keeps the table's pointer in the library's `g_phyFuns`, which the library's
 * functions read. The driver's own `phy_get_romfunc_addr` does the same with the driver's `phy_param`, and
 * names the twenty-three functions that fill the slots -- the same set the library fills, at the same slots.
 * The driver's own twenty-three are below, and the slots point at those.
 *
 * The offsets are the table's, a slot every four bytes, as the library's `phy_get_romfunc_addr` stores them:
 * 0x08, 0x10, 0x14, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x68, 0x6c, 0x70, 0x84,
 * 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa8.
 */

/* The ROM's phy_param_addr: the address of the ROM code's `phy_param`, which its own functions read. */
void phy_param_addr(void *param);

/* The ROM's table of PHY functions, as `phy_get_romfuncs()` returns it; the library's functions read it too. */
extern romfunc *g_phyFuns;

/* The twenty-three the library fills the table's slots with, the driver's own; the definitions are below. */
int32_t ram_set_chan_cal_interp(const int8_t *tab, uint32_t chan);
uint32_t ram_bb_bss_cbw40(uint32_t on);
void ram_set_chan_freq_sw_start(void);
uint32_t ram_get_i2c_read_mask(uint8_t block);
uint32_t ram_get_i2c_mst0_mask(uint8_t block);
uint32_t ram_get_i2c_hostid(uint8_t block);
uint32_t ram_chip_i2c_readReg_org(uint32_t block, uint32_t mask, uint32_t host, uint32_t reg);
void ram_pbus_force_mode(uint32_t on);
uint32_t ram_pbus_rd_addr(uint32_t window, uint32_t second);
uint32_t ram_pbus_rd_shift(uint32_t window, uint32_t second);
void ram_pbus_xpd_rx_on(uint32_t arg);
void ram_pbus_xpd_tx_on(uint32_t arg, uint32_t second);
void ram_set_loopback_gain(uint32_t gain, uint32_t arg, uint32_t second);
void ram_txcal_debuge_mode(void);
void ram_pwdet_tone_start(void);
void ram_pwdet_wait_idle(void);
void ram_read_sar_dout(uint16_t *out);
void ram_start_tx_tone_step(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
void ram_stop_tx_tone(void);
void ram_rfpll_set_freq(uint32_t freq, uint32_t xtal, uint32_t param, uint8_t out[5]);
uint32_t ram_bb_gain_index(uint32_t gain);
void phy_i2c_enter_critical(void);
void phy_i2c_exit_critical(void);

/* The ROM's own the fillers run, and the library's ram_bb_bss_cbw40 calls, as the library runs them. */
int32_t rom_set_chan_cal_interp(const int8_t *tab, uint32_t chan);
void rom_set_chan_freq_sw_start(void);
uint32_t rom_chip_i2c_readReg_org(uint32_t block, uint32_t mask, uint32_t host, uint32_t reg);
void bb_bss_cbw40_dig(uint32_t on);
void cbw2040_cfg(uint32_t on);
void wifi_fbw_sel(uint32_t on);

/*
 * libphy's phy_get_romfunc_addr [0x4200340c]: the ROM is told the library's phy_param, the table's pointer is
 * kept, and the twenty-three slots above are filled, in the library's order. The library fills the ROM's own
 * table in place, so its functions and the ROM's alike reach the library's own; the driver does the same.
 */
void phy_get_romfunc_addr(void)
{
    phy_param_addr(phy_param);
    g_phyFuns = phy_get_romfuncs();
    g_phyFuns[0x08 / 4] = (romfunc)ram_set_chan_cal_interp;
    g_phyFuns[0x10 / 4] = (romfunc)ram_bb_bss_cbw40;
    g_phyFuns[0x14 / 4] = (romfunc)ram_set_chan_freq_sw_start;
    g_phyFuns[0x24 / 4] = (romfunc)ram_pwdet_tone_start;
    g_phyFuns[0x28 / 4] = (romfunc)ram_pwdet_wait_idle;
    g_phyFuns[0x2c / 4] = (romfunc)ram_read_sar_dout;
    g_phyFuns[0x30 / 4] = (romfunc)ram_rfpll_set_freq;
    g_phyFuns[0x34 / 4] = (romfunc)phy_i2c_enter_critical;
    g_phyFuns[0x38 / 4] = (romfunc)phy_i2c_exit_critical;
    g_phyFuns[0x3c / 4] = (romfunc)ram_get_i2c_read_mask;
    g_phyFuns[0x40 / 4] = (romfunc)ram_get_i2c_mst0_mask;
    g_phyFuns[0x44 / 4] = (romfunc)ram_get_i2c_hostid;
    g_phyFuns[0x48 / 4] = (romfunc)ram_chip_i2c_readReg_org;
    g_phyFuns[0x68 / 4] = (romfunc)ram_pbus_force_mode;
    g_phyFuns[0x6c / 4] = (romfunc)ram_pbus_rd_addr;
    g_phyFuns[0x70 / 4] = (romfunc)ram_pbus_rd_shift;
    g_phyFuns[0x84 / 4] = (romfunc)ram_pbus_xpd_rx_on;
    g_phyFuns[0x8c / 4] = (romfunc)ram_pbus_xpd_tx_on;
    g_phyFuns[0x90 / 4] = (romfunc)ram_set_loopback_gain;
    g_phyFuns[0x94 / 4] = (romfunc)ram_txcal_debuge_mode;
    g_phyFuns[0x98 / 4] = (romfunc)ram_start_tx_tone_step;
    g_phyFuns[0x9c / 4] = (romfunc)ram_stop_tx_tone;
    g_phyFuns[0xa8 / 4] = (romfunc)ram_bb_gain_index;
}

/*
 * The twenty-three slot fillers, the driver's own from this commit, taken over with their disassembly.
 * Each is the library's function at the address in its comment; the driver's phy_get_romfunc_addr above
 * puts them in the ROM's table, so the ROM's calls and the library's alike reach them. Three are the
 * ROM's own byte for byte -- ram_set_chan_cal_interp, ram_set_chan_freq_sw_start and
 * ram_chip_i2c_readReg_org -- and the driver's own calls the ROM's by name; the other twenty differ from
 * the ROM's, so their bodies are written out. The two critical sections are empty returns here, as the
 * library's are on this chip.
 */

/* Registers the fillers reach, no header naming them: named by register, meaning not known. */
#define FECTRL_FORCE_MODE    (MODEM_BASE + 0x90Cu) /* ram_pbus_force_mode's bit 27 and its bit 31 */
#define FECTRL_SAR_DOUT0     (MODEM_BASE + 0x824u) /* the SAR's four measurement words, 0x824 to 0x830 */
#define NRX_BSS_CBW40        (MODEM_BASE + 0x780Cu) /* ram_bb_bss_cbw40's bit 0, the band it sets */
#define FEDATA_TX_TONE_CTRL  (MODEM_BASE + 0x414u) /* the transmit tone's run bits: ram_start_tx_tone_step's 0 and 1 */
#define FEDATA_TX_TONE1      (MODEM_BASE + 0x424u) /* the tone generator's second word, as FEDATA_TX_TONE0's first */
#define FEDATA_TX_TONE_CONF  (MODEM_BASE + 0x42Cu) /* the tone's step and scale, ram_start_tx_tone_step's bits 0..3 */

/* phy_param fields the fillers keep, no label naming either. */
#define PHY_PARAM_PWDET_TONE 0x1A1 /* ram_pwdet_tone_start clears the tone's bit where it is set */
#define PHY_PARAM_STEP_01K   0x1A7 /* phy_set_step_01k writes it; ram_rfpll_set_freq divides the offset by ten */

/*
 * libphy's ram_set_chan_cal_interp [0x4200046e]: the calibration table's value at a channel, the two
 * entries the channel falls between interpolated by fives, and the table's third entry where the channel
 * lies past ten. Byte for byte the ROM's rom_set_chan_cal_interp [0x40002e2e], so the driver's own is it.
 */
int32_t ram_set_chan_cal_interp(const int8_t *tab, uint32_t chan)
{
    return rom_set_chan_cal_interp(tab, chan);
}

/*
 * libphy's ram_bb_bss_cbw40 [0x420004be]: the band the baseband's channel is set to -- the library's
 * mac_tx_chan_offset_new for the transmit gain, the ROM's bb_bss_cbw40_dig and wifi_fbw_sel and
 * cbw2040_cfg, and NRX_BSS_CBW40's bit 0 for the band itself. The library differs from the ROM's own
 * rom_bb_bss_cbw40 [0x40002ed8] in calling the library's mac_tx_chan_offset_new and in passing 1 to
 * wifi_fbw_sel where the ROM passes 0, so the body is written out: the call the library makes, and
 * wifi_fbw_sel always 1 once the channel is not 0.
 */
uint32_t ram_bb_bss_cbw40(uint32_t on)
{
    mac_tx_chan_offset_new(on & 0xffu);
    if (on == 0u) {
        bb_bss_cbw40_dig(0u);
        wifi_fbw_sel(0u);
    } else {
        bb_bss_cbw40_dig(1u);
        cbw2040_cfg(on == 1u ? 1u : 0u);
        wifi_fbw_sel(1u);
        if (on == 3u) {
            REG(NRX_BSS_CBW40) |= 1u;
        } else {
            REG(NRX_BSS_CBW40) &= ~1u;
        }
    }
    return on != 0u;
}

/*
 * libphy's ram_set_chan_freq_sw_start [0x4200264e]: the ROM's frequency channel enable, a ten
 * microsecond wait, then the frequency setting's busy bit waited out. Byte for byte the ROM's own
 * rom_set_chan_freq_sw_start [0x40003ce0], so the driver's own is it.
 */
void ram_set_chan_freq_sw_start(void)
{
    rom_set_chan_freq_sw_start();
}

/*
 * libphy's ram_get_i2c_read_mask [0x42002b4a]: the analog I2C read mask of one block, 0x61 to 0x6d, from
 * a table of thirteen halfwords -- the ROM's own table holds the same values as words -- and zero outside
 * it. The block is the argument less 0x61, held to a byte.
 */
uint32_t ram_get_i2c_read_mask(uint8_t block)
{
    static const uint16_t masks[13] = {
        0x0100, 0x0020, 0x0010, 0x0000, 0x0000, 0x0080, 0x0004,
        0x0000, 0x0200, 0x0040, 0x0008, 0x0000, 0x0400,
    };
    uint32_t i = (uint8_t)(block - 0x61u);

    return i <= 12u ? masks[i] : 0u;
}

/*
 * libphy's ram_get_i2c_mst0_mask [0x42002b6e]: the analog I2C master's mask of one block, the same
 * thirteen blocks, from its own table of thirteen halfwords, zero outside it.
 */
uint32_t ram_get_i2c_mst0_mask(uint8_t block)
{
    static const uint16_t masks[13] = {
        0x0400, 0x0080, 0x0040, 0x0000, 0x0000, 0x0200, 0x0010,
        0x0000, 0x0800, 0x0001, 0x0020, 0x0000, 0x1000,
    };
    uint32_t i = (uint8_t)(block - 0x61u);

    return i <= 12u ? masks[i] : 0u;
}

/*
 * libphy's ram_get_i2c_hostid [0x42003d4a's library copy, 0x42002b92]: which host reaches a block, the
 * blocks 0x62 to 0x6b from a bit each and the bits 0x223 of them, and, in every case, the analog I2C
 * master's host field set to 0x1f00. The library's mask, 0x223, and its 0x1f00 differ from the ROM's own
 * rom_get_i2c_hostid [0x40003d4a], whose own are 0x227 and 0x1fe00, so the body is written out.
 */
uint32_t ram_get_i2c_hostid(uint8_t block)
{
    uint32_t host = 0u;
    uint32_t i = (uint8_t)(block - 0x62u);

    if (i <= 9u) {
        host = (1u << i) & 0x223u;
        host = host != 0u;
    }
    REG(I2C_MST_MST0) = (REG(I2C_MST_MST0) & 0xffffe00fu) | 0x1f00u;
    return host;
}

/*
 * libphy's ram_chip_i2c_readReg_org [0x42002bcc]: a byte read from the analog I2C's register window at
 * 0x600af800, the host choosing the word, the caller's mask written inverted beside it, and the read's
 * bit 25 waited for. Byte for byte the ROM's own rom_chip_i2c_readReg_org [0x40003d88], so the driver's
 * own is it.
 */
uint32_t ram_chip_i2c_readReg_org(uint32_t block, uint32_t mask, uint32_t host, uint32_t reg)
{
    return rom_chip_i2c_readReg_org(block, mask, host, reg);
}

/*
 * libphy's ram_pbus_force_mode [0x42004196]: the pbus's force mode. On, FECTRL_FORCE_MODE's bit 27 is
 * cleared and the front end's FECTRL_RX_GAIN_TRIG_EN's bit 0 set; off, that bit is cleared, the force
 * mode's bit 31 set, and, where MODEM_SYSCON_WIFI_BB_CFG's bit 1 is set, the baseband's AGC_2 is stepped
 * through 0x32000000 and 0x800000 with waits of 1 and 2 microseconds and 0xff7fffff. The library differs
 * from the ROM's own rom_pbus_force_mode [0x40004290], which only clears the force mode's bit 27 where
 * that bit of the syscon is clear, so the body is written out.
 */
void ram_pbus_force_mode(uint32_t on)
{
    if (on != 0u) {
        REG(FECTRL_FORCE_MODE) &= ~0x08000000u;
        REG(FECTRL_RX_GAIN_TRIG_EN) |= 1u;
    } else {
        REG(FECTRL_RX_GAIN_TRIG_EN) &= ~1u;
        REG(FECTRL_FORCE_MODE) |= 0x08000000u;
        if ((REG(MODEM_SYSCON_WIFI_BB_CFG) & 2u) != 0u) {
            ets_delay_us(1);
            REG(AGC_2) = (REG(AGC_2) & 0x00ffffffu) | 0x32000000u;
            REG(AGC_2) |= 0x800000u;
            ets_delay_us(2);
            REG(AGC_2) &= 0xff7fffffu;
        }
    }
}

/*
 * libphy's ram_pbus_rd_addr [0x4200422e] and ram_pbus_rd_shift [0x4200428a]: the pbus address and bit
 * window a read of one of the front end's groups falls on, from the group's number and, for two groups,
 * its second. The library's own tables, at 0x4200ac64 and 0x4200ac78, differ from the ROM's, so the two
 * are written out as the tables say; a group past four, or past five for the shift, answers the last.
 */
uint32_t ram_pbus_rd_addr(uint32_t window, uint32_t second)
{
    switch (window) {
    case 0u: return MODEM_BASE + 0x920u;
    case 1u: return MODEM_BASE + 0x914u;
    case 2u: return MODEM_BASE + (second == 1u ? 0x918u : 0x91cu);
    case 3u: return MODEM_BASE + 0x91cu;
    case 4u: return MODEM_BASE + (second == 1u ? 0x920u : 0x924u);
    default: return MODEM_BASE + 0x924u;
    }
}

uint32_t ram_pbus_rd_shift(uint32_t window, uint32_t second)
{
    switch (window) {
    case 0u: return second == 1u ? 0x12u : 9u;
    case 1u:
    case 3u: return second == 1u ? 9u : 0u;
    case 2u:
    case 4u: return second == 1u ? 0u : 0x12u;
    case 5u: return 9u;
    default: return 0u;
    }
}

/*
 * libphy's ram_pbus_xpd_rx_on [0x420042c4], ram_pbus_xpd_tx_on [0x42004348] and ram_set_loopback_gain
 * [0x4200441a]: the receiver, the transmitter and the loopback's gain brought up through the ROM's
 * rom_pbus_force_test -- the table's slot 29 -- with the receiver's last word taking phy_param's byte 2
 * and the transmitter's and the loopback's taking the caller's arguments; the transmitter and the
 * loopback force the DCO pair 0x01000100, 0x01000100 through the ROM's pbus_set_dco, the library's own
 * pair where the ROM's rom_pbus_xpd_tx_on takes its own out of its rodata. Each differs from the ROM's
 * own, so each is written out.
 */
void ram_pbus_xpd_rx_on(uint32_t arg)
{
    rom_pbus_force_test(4u, 1u, 0u);
    rom_pbus_force_test(4u, 2u, 1u);
    rom_pbus_force_test(5u, 1u, 0u);
    rom_pbus_force_test(0u, 1u, 0x40u);
    rom_pbus_force_test(0u, 2u, phy_param[0x02]);
    rom_pbus_force_test(1u, 1u, 0x189u);
    rom_pbus_force_test(1u, 2u, arg);
}

void ram_pbus_xpd_tx_on(uint32_t arg, uint32_t second)
{
    uint32_t dco[2] = { 0x01000100u, 0x01000100u };

    rom_pbus_force_test(0u, 1u, 0x80u);
    rom_pbus_force_test(0u, 2u, 0u);
    rom_pbus_force_test(4u, 2u, 0u);
    pbus_set_dco(dco);
    rom_pbus_force_test(1u, 1u, 0x7cu);
    pbus_set_dco(dco);
    rom_pbus_force_test(1u, 2u, second);
    rom_pbus_force_test(4u, 1u, 0xbu);
    rom_pbus_force_test(5u, 1u, 0x1c0u);
    ets_delay_us(0x14);
    rom_pbus_force_test(5u, 1u, (arg + 0x1c0u) & 0xffffu);
    ets_delay_us(0x14);
}

void ram_set_loopback_gain(uint32_t gain, uint32_t arg, uint32_t second)
{
    rom_pbus_force_test(4u, 2u, 1u);
    rom_pbus_force_test(4u, 1u, 0x83u);
    rom_pbus_force_test(5u, 1u, (gain + 0x1c0u) & 0xffffu);
    rom_pbus_force_test(0u, 1u, arg);
    rom_pbus_force_test(0u, 2u, phy_param[0x02]);
    rom_pbus_force_test(1u, 1u, 0x1f9u);
    rom_pbus_force_test(1u, 2u, second);
}

/*
 * libphy's ram_txcal_debuge_mode [0x420044b0]: the transmit calibration's debug mode -- the pbus's debug
 * mode, phy_param's byte 0x12 handed to the table's transmitter bring-up (ram_pbus_xpd_tx_on), the DCO
 * pair at phy_param 0xa8 stepped by the gain 0's index, the transmit clock on, and the power detector
 * enabled. The library's own differs from the ROM's own rom_txcal_debuge_mode [0x40004780], so it is
 * written out; the transmitter bring-up and the power detector are reached through the table's slots 35
 * and 7, as the library reaches them.
 */
void ram_txcal_debuge_mode(void)
{
    uint8_t txcal = phy_param[0x12];

    pbus_debugmode();
    ((void (*)(uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_XPD_TX_ON])(txcal, 0u);
    pbus_set_dco(&phy_param[0xa8u + txbbgain_to_index(0u) * 8u]);
    set_txclk_en(1u);
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_EN_PWDET])(1u);
}

/*
 * libphy's ram_pwdet_tone_start [0x4200491e], ram_pwdet_wait_idle [0x420049a6] and ram_read_sar_dout
 * [0x420049ba]: the power detector's tone started and its measurement read. The tone's start raises the
 * SAR ADC's power detector drive, APB_SARADC_CTRL's bit 29, and the tone generator's FEDATA_TX_TONE0's
 * bit 18, waits, raises FECTRL_PWDET_CONF0's bit 0, and waits on FECTRL_PWDET_CONF1's bits 14 to 16 for
 * 7; where phy_param's byte 0x1a1 is set the tone's bit is then cleared, where the ROM's own
 * rom_pwdet_tone_start [0x400048b4] always clears it and waits on bits 24 to 26 -- so the body is
 * written out. The idle wait is the same wait on bits 14 to 16. The read unpacks the front end's four
 * measurement words at 0x824 to 0x830, each a pair at bits 1 and 17, into eight halfwords, where the
 * ROM's rom_read_sar_dout [0x40004b80] reads eight words one halfword each -- so its body is written
 * out too.
 */
void ram_pwdet_tone_start(void)
{
    REG(APB_SARADC_CTRL) |= APB_SARADC2_PWDET_DRV;
    REG(FEDATA_TX_TONE0) |= 0x00040000u;
    ets_delay_us(1);
    REG(FECTRL_PWDET_CONF0) &= ~1u;
    REG(FECTRL_PWDET_CONF0) |= 1u;
    ets_delay_us(2);
    while (((REG(FECTRL_PWDET_CONF1) >> 14) & 7u) != 7u) {
    }
    if (phy_param[PHY_PARAM_PWDET_TONE] != 0u) {
        REG(FEDATA_TX_TONE0) &= ~0x00040000u;
    }
}

void ram_pwdet_wait_idle(void)
{
    while (((REG(FECTRL_PWDET_CONF1) >> 14) & 7u) != 7u) {
    }
}

void ram_read_sar_dout(uint16_t *out)
{
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t v = REG(FECTRL_SAR_DOUT0 + 4u * i);

        out[2u * i] = (uint16_t)((v >> 1) & 0x1fffu);
        out[2u * i + 1u] = (uint16_t)((v >> 17) & 0x1fffu);
    }
}

/*
 * libphy's ram_start_tx_tone_step [0x42005066] and ram_stop_tx_tone [0x4200510a]: the transmit tone
 * generator's run -- FEDATA_TX_TONE_CTRL's bits 0 and 1 where the caller's two words are both zero (the
 * tone stopped) and cleared where either is set, FEDATA_TX_TONE_CONF's step and scale, and the two
 * generator words FEDATA_TX_TONE0 and FEDATA_TX_TONE1 packed from the caller's six arguments, their low
 * halves kept and their top bits preserved; the stop clears the two generator words' bit 18 and runs the
 * CTRL word's two bits to 3. The library's pack, with the negated scale bytes at bit 10 and the step at
 * bit 2, differs from the ROM's own rom_start_tx_tone_step [0x400052ac] and rom_stop_tx_tone
 * [0x40005372], which writes no CTRL word at all -- so both are written out.
 */
void ram_start_tx_tone_step(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    uint32_t v;

    a2 = (uint8_t)(-a2);
    a5 = (uint8_t)(-a5);
    v = REG(FEDATA_TX_TONE_CTRL);
    REG(FEDATA_TX_TONE_CTRL) = (a0 | a3) != 0u ? (v & ~3u) : (v | 3u);

    REG(FEDATA_TX_TONE_CONF) = (REG(FEDATA_TX_TONE_CONF) & ~3u) | (a1 & 3u);
    REG(FEDATA_TX_TONE_CONF) = ((a4 << 2) & 0xcu) | (REG(FEDATA_TX_TONE_CONF) & ~0xcu);

    v = ((a0 << 0x12) | ((uint32_t)((int32_t)a1 >> 2)) | (a2 << 0xa)) & 0x0fffffffu;
    REG(FEDATA_TX_TONE0) = v | (REG(FEDATA_TX_TONE0) & 0xf0000000u);

    v = ((a3 << 0x12) | ((uint32_t)((int32_t)a4 >> 2)) | (a5 << 0xa)) & 0x0fffffffu;
    REG(FEDATA_TX_TONE1) = v | (REG(FEDATA_TX_TONE1) & 0xf0000000u);
}

void ram_stop_tx_tone(void)
{
    REG(FEDATA_TX_TONE0) &= ~0x00040000u;
    REG(FEDATA_TX_TONE1) &= ~0x00040000u;
    REG(FEDATA_TX_TONE_CTRL) |= 3u;
}

/*
 * libphy's ram_rfpll_set_freq [0x4200547e]: the RF PLL's frequency from the crystal's selector, a
 * frequency in MHz and the caller's parameter, its five bytes packed out. The parameter's low twenty bits
 * are divided by a thousand, and again by ten where phy_param's byte 0x1a7 -- phy_set_step_01k's -- is
 * set, added to the frequency's own low twenty bits, divided by the crystal's divisor times three (0x1a,
 * 0x20 or 0x28), and the low word's remainder past 0xff000000 packed into the caller's five bytes. The
 * divisions are signed and 64-bit, the library's __divdi3; the ROM's own rom_rfpll_set_freq [0x400059e6]
 * divides 32-bit, with no ten and no sign -- so the body is written out.
 */
void ram_rfpll_set_freq(uint32_t freq, uint32_t xtal, uint32_t param, uint8_t out[5])
{
    int64_t div = phy_param[PHY_PARAM_STEP_01K] != 0u ? 10 : 1;
    int64_t v = (int64_t)(int32_t)param << 20;
    uint32_t divisor = (xtal == 1u ? 0x1au : xtal == 2u ? 0x20u : 0x28u) * 3u;
    int64_t t;
    uint32_t r;

    v = v / 1000;
    v = v / div;
    t = v + (int64_t)(uint64_t)(uint32_t)(freq << 20);
    t = t / (int64_t)divisor;
    r = (uint32_t)t + 0xff000000u;
    out[4] = (uint8_t)((r >> 27) & 1u);
    out[3] = (uint8_t)(r >> 19);
    out[2] = (uint8_t)(r >> 11);
    out[1] = (uint8_t)(r >> 3);
    out[0] = (uint8_t)(r & 7u);
}

/*
 * libphy's ram_bb_gain_index [0x42007000]: which of the baseband's seven gain entries a gain is, 0 to 6,
 * and 7 where it is none of them. The library's seven halfwords, 0, 0x20, 0x30, 0x38, 0x3c, 0x3e and
 * 0x3f, are its own; the ROM's own rom_bb_gain_index [0x400071c0] holds its own, so the body is written
 * out with the library's.
 */
uint32_t ram_bb_gain_index(uint32_t gain)
{
    static const uint16_t gains[7] = { 0x0000, 0x0020, 0x0030, 0x0038, 0x003c, 0x003e, 0x003f };
    uint32_t i = 0u;

    while (i < 7u && gains[i] != gain) {
        i++;
    }
    return i;
}

/*
 * libphy's phy_i2c_enter_critical [0x42000400] and phy_i2c_exit_critical [0x42000402]: empty returns on
 * this chip, the library's own pair, which the table's slots 13 and 14 take.
 */
void phy_i2c_enter_critical(void)
{
}

void phy_i2c_exit_critical(void)
{
}

/*
 * libphy's set_rx_gain_cal_iq_new [0x4200699a] and rxiq_cal_init [0x420075c8], the driver's own, step 5's
 * package 7c: the receive IQ calibration's gain search and the block that runs it. Each is held to the
 * library's off the board, by phy-ab.py, and build.mk then weakens the library's two.
 */

/* The ROM's pbus_rx_dco_cal [0x400062a2]: one receive DC estimation with the pbus forced; the first argument,
 * a buffer, and three more. The library's rxiq_cal_init reaches it while the estimate's blocks stay its own. */
void pbus_rx_dco_cal(uint32_t a0, void *a1, uint32_t a2, uint32_t a3, uint32_t a4);

/* The ROM's rom_set_txcap_reg, the table's slot 25: a six-byte table and an index, the pair written to the
 * analog I2C's block 0x6b. */
#define ROMFUNC_SET_TXCAP_REG 25

/*
 * libphy's set_rx_gain_cal_iq_new [0x4200699a]: the receive IQ calibration's gain search. Where the first
 * argument is set, the analog I2C's block 0x67 register 3 bits 2..2 is read and cleared and put back at the
 * end. The loopback gain, the loopback transmit IQ and the pbus's receive DC are set, then over two passes the
 * receive gain is stepped a sixteenth at a time -- the estimate's magnitude, the front end's
 * FEDATA_IQ_EST_MAG shifted seven, deciding the step: below 0x1000 the gain falls, above 0x20000 it rises and
 * the loopback side is dropped -- the pbus's receive DC taken again each step and the gain held between 0 and
 * 0x78. The second pass's gain and the second argument go to get_rfcal_rxiq_data_new, whose halfword is the
 * caller's third argument's. The fourth argument asks the library for its prints, which are left out. The
 * first argument to the tone is the caller's second, and phy_param's halfword 0x10 bit 3 reaches the pbus's
 * receive DC as its fifth; neither is labelled.
 */
void set_rx_gain_cal_iq_new(uint32_t save_i2c, uint32_t tone, uint32_t out, uint32_t verbose)
{
    uint16_t *outp = (uint16_t *)(uintptr_t)out;
    uint32_t dco[2] = { 0x01000100u, 0x01000100u };
    int32_t bit = (int32_t)((phy_param16(0x10u) >> 3) & 1u);
    uint32_t saved = 0u;
    int32_t mag, gain = 0x30, lb = 0x20;
    uint32_t pass = 2u;

    (void)verbose;
    if (save_i2c != 0u) {
        saved = ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_I2C_READ_MASK])(
            0x67u, 1u, 3u, 2u, 2u);
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_MASK])(
            0x67u, 1u, 3u, 2u, 2u, 0u);
    }
    dco[0] = 0x01000100u;
    ram_set_loopback_gain(0u, 0x43u, 0x20u);
    set_lb_txiq_new(phy_param16(0xd0u));
    pbus_rx_dco_cal(0xfa0u, dco, 0xau, 0u, (uint32_t)bit);

    for (;;) {
        ((void (*)(uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_FORCE_TEST])(1u, 1u, 0u);
        ((void (*)(uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_FORCE_TEST])(1u, 1u, 0x1f9u);
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_START_TX_TONE_STEP])(
            1u, tone, (uint32_t)gain & 0xffu, 0u, 0u, 0u);
        ((void (*)(uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_IQ_EST_ENABLE])(1u, 0x3ffu);
        mag = (int32_t)REG(FEDATA_IQ_EST_MAG) >> 7;
        rom_iq_est_disable();
        ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_STOP_TX_TONE])(1u);
        if (mag > 0x20000) {
            if (lb != 0) {
                lb = 0;
                goto adjust;
            }
            gain += 0x18;
            goto clamp;
        }
        if (mag >= 0x1000) {
            break;
        }
        gain -= 0x18;
    clamp:
        gain = (int16_t)gain;
    adjust:
        ram_set_loopback_gain(0u, 0x43u, (uint32_t)lb);
        pbus_rx_dco_cal(0xfa0u, dco, 0xau, 0u, (uint32_t)bit);
        gain = gain > 0x78 ? 0x78 : gain;
        gain = (int16_t)gain < 0 ? 0 : gain;
        gain = (int16_t)gain;
        if (pass != 1u) {
            pass = 1u;
            continue;
        }
        break;
    }
    *outp = (uint16_t)get_rfcal_rxiq_data_new((int32_t)tone, (int32_t)(uint8_t)gain, verbose);
    if (save_i2c != 0u) {
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_MASK])(
            0x67u, 1u, 3u, 2u, 2u, saved);
    }
}

/*
 * libphy's rxiq_cal_init [0x420075c8]: the receive IQ calibration. Where phy_param's word 0xA4 has bit 10 set
 * -- the calibration done -- it returns at once; else the channel's analog side is set to 6, the six-byte
 * table at phy_param 0xDC is written through the ROM's rom_set_txcap_reg, the front end's FECTRL_TXRX_OFF word
 * has bits 14 and 15 raised -- and bit 15 cleared later -- the pbus's debug mode is entered, the receiver powered
 * and the loopback's clocks raised; where the first argument is set, the pbus's word is read and its bit 1 set.
 * FEDATA_REG_SET3 has bit 29 raised, bit 13 raised, bit 30 lowered and bit 14 lowered, then the gain search of
 * set_rx_gain_cal_iq_new runs into phy_param 0xD4 with the tone 0x80, and the four halfwords at 0xD4..0xDA are
 * repacked -- the bits 8..13 moved down one and the bits 0..6 kept. Where the third argument is clear, bit 30
 * is raised, bit 14 raised, bit 29 lowered, FECTRL_TXRX_OFF's bit 15 cleared, the loopback's clocks lowered,
 * the transmitter powered down and the pbus's work mode put back. Then phy_param's bit 10 is set. The second
 * argument is unused, as the library's is when the driver calls it. No label names the bits.
 */
void rxiq_cal_init(uint32_t a0, void *p1, uint32_t a2)
{
    int32_t skip_cleanup = (int32_t)a2;
    uint32_t i;

    (void)p1;
    if ((phy_param32(0xa4u) & 0x400u) != 0u) {
        return;
    }
    chip_v7_set_chan_ana(6u);
    ((void (*)(const uint8_t *, uint32_t))phy_get_romfuncs()[ROMFUNC_SET_TXCAP_REG])(&phy_param[0xdcu], 6u);
    REG(FECTRL_TXRX_OFF) |= 0x4000u;
    REG(FECTRL_TXRX_OFF) |= 0x8000u;
    pbus_debugmode();
    ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_XPD_RX_ON])(0u);
    rom_loopback_mode_en(1u);
    if (a0 != 0u) {
        uint32_t v = ((uint32_t (*)(uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_RD])(1u, 1u);
        ((void (*)(uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_FORCE_TEST])(1u, 1u, (v | 2u) & 0xffffu);
    }
    REG(FEDATA_REG_SET3) |= 0x20000000u;
    REG(FEDATA_REG_SET3) |= 0x2000u;
    REG(FEDATA_REG_SET3) &= 0xbfffffffu;
    REG(FEDATA_REG_SET3) &= 0xffffbfffu;
    set_rx_gain_cal_iq_new(0u, 0x80u, (uint32_t)(uintptr_t)&phy_param[0xd4u], 0u);
    for (i = 0u; i != 4u; i++) {
        uint16_t v = phy_param16(0xd4u + i * 2u);
        phy_param16_set(0xd4u + i * 2u, (uint16_t)(((uint32_t)(v >> 1) & 0x1f80u) | ((uint32_t)v & 0x7fu)));
    }
    if (skip_cleanup == 0) {
        REG(FEDATA_REG_SET3) |= 0x40000000u;
        REG(FEDATA_REG_SET3) |= 0x4000u;
        REG(FEDATA_REG_SET3) &= 0xdfffffffu;
        REG(FECTRL_TXRX_OFF) &= 0xffff7fffu;
        rom_loopback_mode_en(0u);
        rom_pbus_xpd_tx_off();
        pbus_workmode();
    }
    phy_param32_set(0xa4u, phy_param32(0xa4u) | 0x400u);
}

/*
 * libphy's tx_cap_init_loop [0x42009578] and rfcal_txcap_loop [0x4200946c], the driver's own, step 5's package
 * 8: the transmit capacitor calibration over three channels and the scan of one byte's two analog fields.
 * Each is held to the library's off the board, by phy-ab.py, and build.mk then weakens the library's two.
 */

/* The ROM's dc_iq_est [0x40005dfa]: the estimate armed, its two accumulators shifted six and divided by one
 * more than the second argument, the magnitude's quotient eight times less their squares' sum held at zero,
 * turned to decibels and shifted four into the third argument's third word; the estimate then disabled. The
 * library's dc_iq_est_new with its fourth argument clear is the same read, and the library's loop here reaches
 * the ROM's function directly. */
void dc_iq_est(uint32_t a0, uint32_t a1, int32_t out[3]);

/*
 * libphy's rfcal_txcap_loop [0x4200946c]: one byte of the transmit capacitor's DAC scanned. The first pass
 * walks the analog I2C's block 0x6b register 2 bits 3..0 from 7 down to 0, and the second pass bits 7..4 from
 * 13 down to 1 by twos; each value is written, the estimate's magnitude read back through dc_iq_est_high, and
 * the largest kept -- its value written back at the pass's end -- the caller's byte advanced and the second
 * pass run for that next byte. The first argument asks the library for its prints, which are left out; here it
 * is phy_param's halfword 0x10 bit 7, which the block's caller passes and no label names.
 */
void rfcal_txcap_loop(uint32_t verbose, uint8_t *out)
{
    int32_t phase = 0;
    int8_t scan = 7;
    uint32_t step = 1u;
    int32_t best;
    int32_t est[3];

    (void)verbose;
    for (;;) {
        *out = (uint8_t)scan;
        best = 0;
        for (;;) {
            ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_MASK])(
                0x6bu, 1u, 2u, phase != 0 ? 7u : 3u, phase != 0 ? 4u : 0u, (uint32_t)(uint8_t)scan);
            dc_iq_est_high(1u, 0x800u, est);
            if (best < (int16_t)est[2]) {
                best = (int16_t)est[2];
                *out = (uint8_t)scan;
            }
            scan = (int8_t)((uint8_t)scan - (uint8_t)step);
            if (scan < 0) {
                break;
            }
        }
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_MASK])(
            0x6bu, 1u, 2u, phase != 0 ? 7u : 3u, phase != 0 ? 4u : 0u, (uint32_t)*out);
        out++;
        if (phase == 0) {
            phase = 1;
            scan = 0xd;
            step = 2u;
            continue;
        }
        return;
    }
}

/*
 * libphy's tx_cap_init_loop [0x42009578]: the transmit capacitor calibration over the three channels 1, 6 and
 * 11. For each: the frequency retuned to the channel, the front end's FECTRL_TXRX_OFF word's bits 14 and 15
 * raised, the pbus brought to its debug mode, the receiver powered, the loopback's clocks raised, the loopback
 * gain set, the pbus's receive DC taken with the pair 0x01000100, and the analog I2C's block 0x6b register 2's
 * low nibble set to 1 and its high nibble to 5. Then a tone at 0x80, its gain stepped by 0x18 while the
 * estimate's third word sits at 31 or below and down while it sits above 46, twice at most; the tone stopped and
 * rfcal_txcap_loop run into the byte phy_param 0xdc holds for the channel -- 0xdc, 0xde and 0xe0. The pbus's
 * work mode comes back, the loopback is dropped, the transmitter powered down and FECTRL_TXRX_OFF's bits 14 and
 * 15 cleared. The library passes phy_param's halfword 0x10 bit 7 to rfcal_txcap_loop as its print argument, left
 * out as elsewhere; the bit is not labelled.
 */
void tx_cap_init_loop(void)
{
    static const uint8_t chans[3] = { 1u, 6u, 11u };
    uint32_t dco[2] = { 0x01000100u, 0x01000100u };
    int32_t est[3];
    int32_t verbose = (int32_t)((phy_param16(0x10u) >> 7) & 1u);
    uint8_t *out = &phy_param[PHY_PARAM_DCAP_STG];
    uint32_t i;

    for (i = 0u; i != 3u; i++) {
        uint8_t gain = 0x3cu;
        uint32_t pass = 2u;

        set_channel_rfpll_freq(chans[i], phy_param[PHY_PARAM_XTAL], 0);
        REG(FECTRL_TXRX_OFF) |= 0x4000u;
        REG(FECTRL_TXRX_OFF) |= 0x8000u;
        pbus_debugmode();
        ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_PBUS_XPD_RX_ON])(0u);
        rom_loopback_mode_en(1u);
        ram_set_loopback_gain(0u, 0x43u, 0x20u);
        pbus_rx_dco_cal(0x800u, dco, 0xau, 0u, 0u);
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_MASK])(
            0x6bu, 1u, 2u, 3u, 0u, 1u);
        ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_MASK])(
            0x6bu, 1u, 2u, 7u, 4u, 5u);
        for (;;) {
            ((void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))phy_get_romfuncs()[ROMFUNC_START_TX_TONE_STEP])(
                1u, 0x80u, (uint32_t)gain, 0u, 0u, 0u);
            dc_iq_est(1u, 0x800u, est);
            if (est[2] > 46) {
                gain = (uint8_t)(gain + 0x18u);
            } else if (est[2] > 31) {
                break;
            } else {
                gain = (uint8_t)(gain - 0x18u);
            }
            if (pass != 1u) {
                pass = 1u;
                continue;
            }
            break;
        }
        rfcal_txcap_loop((uint32_t)verbose, out);
        ((void (*)(uint32_t))phy_get_romfuncs()[ROMFUNC_STOP_TX_TONE])(1u);
        out += 2u;
        rom_loopback_mode_en(0u);
        rom_pbus_xpd_tx_off();
        pbus_workmode();
        REG(FECTRL_TXRX_OFF) &= 0xffffbfffu;
        REG(FECTRL_TXRX_OFF) &= 0xffff7fffu;
    }
}

/*
 * libphy's txdc_cal_init_new [0x42008d1e], bt_txdc_cal_new [0x42008c7a] and set_lb_txiq_new [0x42006962], the
 * driver's own, step 5's package 9: the transmit DC calibration's bring-up, Bluetooth's and the loopback
 * transmit IQ set. Each is held to the library's off the board, by phy-ab.py, and build.mk then weakens the
 * library's three.
 */

/*
 * libphy's set_lb_txiq_new [0x42006962]: the argument's packed pair split through get_iq_value, its first byte,
 * signed, and its second less four, signed, handed to txiq_set_reg's forward and loopback halves.
 */
void set_lb_txiq_new(uint32_t a0)
{
    uint8_t out[2];

    get_iq_value(out, (uint16_t)a0);
    (void)txiq_set_reg((int8_t)out[0], 1);
    (void)txiq_set_reg((int8_t)(uint8_t)(out[1] - 4u), 0);
}

/*
 * libphy's txdc_cal_init_new [0x42008d1e]: the front end's transmit DC offsets brought up -- the pbus's debug
 * mode entered and the transmitter powered with the caller's second and third arguments -- then five gains, from
 * index_to_txbbgain, forced into the pbus's window 1 place 2 and one transmit DC pair settled each into the
 * caller's buffer (four halfwords a pair, so twenty), the receiver powered down and the pbus's work mode put
 * back. Where the fourth argument is set, the pbus's word is read and its bit 1 set first. The library's print
 * flag is phy_param's halfword 0x10 bit 4, and its prints are left out.
 */
void txdc_cal_init_new(uint16_t *dco, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t i;

    pbus_debugmode();
    rom_pbus_xpd_tx_on(a1, a2);
    if (a3 != 0u) {
        rom_pbus_force_test(1u, 1u, (rom_pbus_rd(1u, 1u) | 2u) & 0xffffu);
    }
    for (i = 0u; i < 5u; i++) {
        rom_pbus_force_test(1u, 2u, index_to_txbbgain(i));
        txdc_cal_new(dco + 4u * i);
    }
    ram_pbus_xpd_rx_on(0u);
    pbus_workmode();
}

/*
 * libphy's bt_txdc_cal_new [0x42008c7a]: where phy_param's word 0xA4 bit 12 is clear, the front end's Bluetooth
 * transmit DC pair settled -- the pbus's debug mode entered, the transmitter powered with channel 0xf and the
 * receiver's pbus window 4 place 2 forced from phy_param's byte 0x14 shifted three, the pair settled into
 * phy_param 0x104, the receiver powered down, the pbus's work mode put back and the bit set. No label names the
 * bit or the byte.
 */
void bt_txdc_cal_new(void)
{
    if ((phy_param32(0xa4u) & 0x1000u) != 0u) {
        return;
    }
    pbus_debugmode();
    rom_pbus_xpd_tx_on(0xfu, 0u);
    rom_pbus_force_test(1u, 1u, (rom_pbus_rd(1u, 1u) | 2u) & 0xffffu);
    rom_pbus_force_test(4u, 2u, (uint32_t)phy_param[0x14u] << 3);
    txdc_cal_new((uint16_t *)(void *)&phy_param[0x104u]);
    ram_pbus_xpd_rx_on(0u);
    pbus_workmode();
    phy_param32_set(0xa4u, phy_param32(0xa4u) | 0x1000u);
}

/*
 * libphy's register_chipv7_phy_init_param [0x420037aa]: the caller's init data spread into phy_param -- the
 * byte 0x50 from its first, the eighteen from 0x52 from its 2nd, the byte 0x66 from its 0x18th, the fourteen
 * from 0x70 from its 0x19th, the fourteen from 0x7e from its 0x27th, the fourteen from 0x8c from its 0x35th,
 * and the nine from 0x67 from its 0x43rd. The bytes 0x51, 0x64 and 0x65 are left as they were found.
 */
void register_chipv7_phy_init_param(const esp_phy_init_data_t *init_data)
{
    const uint8_t *init = init_data->params;
    uint32_t i;

    phy_param[0x50u] = init[0];
    for (i = 0u; i < 0x12u; i++) {
        phy_param[0x52u + i] = init[2u + i];
    }
    phy_param[0x66u] = init[0x18u];
    for (i = 0u; i < 14u; i++) {
        phy_param[0x70u + i] = init[0x19u + i];
    }
    for (i = 0u; i < 14u; i++) {
        phy_param[0x7eu + i] = init[0x27u + i];
    }
    for (i = 0u; i < 14u; i++) {
        phy_param[0x8cu + i] = init[0x35u + i];
    }
    for (i = 0u; i < 9u; i++) {
        phy_param[0x67u + i] = init[0x43u + i];
    }
}

/*
 * libphy's register_chipv7_phy [0x42003ea0]: the PHY's full bring-up and calibration, the driver's own this
 * step (package 10b), and the head the library's own has kept until now. It hands the ROM its PHY functions,
 * raises the Wi-Fi and modem APB clocks (0x404 of MODEM_SYSCON_CLK_CONF1, its own bit 10 named and bit 2 not),
 * clears the baseband's bit 1, forces the front end's transmit and receive off, resets the frequency module,
 * and where phy_param 0x4e asks applies the library's 23 dBm setting to the default init data built here.
 * On a fresh chip -- phy_param 0x25 clear -- it spreads the caller's init data, or the library's compiled-in
 * default built here when the caller brings none, into phy_param, and checks the saved calibration blob: in
 * mode 1 a blob that stands is recovered and the flag's bits 5, 7, 9 and 16 are cleared, one that will not do
 * clears the whole flag and moves the mode to 2, and any other mode restores the blob into phy_param and then
 * clears the flag. rf_init and bb_init run, the temperature init's two gates come from the flag's bits 5 and
 * 20, and, where the blob was not recovered in mode 1, the restored block is held against the chip, saved
 * back and its negated sum rewritten. Where phy_param 0xa1 asks, the channel's offset is set; the baseband
 * PLL is taken down, the RF PLL's SDM register read through the ROM's table, the chip marked up, the
 * frequency setting enabled, the front end's forcing released and the Wi-Fi clock put back as it was found.
 * The return is the blob check's number, zero where the blob stands.
 */
int register_chipv7_phy(const esp_phy_init_data_t *init_data, esp_phy_calibration_data_t *cal, int mode)
{
    /* The library's compiled-in default init data, built whether or not the caller brings its own. */
    esp_phy_init_data_t def = { { 0x0a, 0x00, 0x54, 0x54, 0x50, 0x50, 0x50, 0x4c, 0x4c, 0x4c, 0x4c, 0x48,
                                  0x44, 0x3c, 0x3c, 0x3c, 0x4c, 0x4c, 0x4c, 0x48 } };
    uint8_t buf[0x66];
    uint32_t version;
    uint32_t flag;
    uint32_t clk_conf1;
    int ret = 0;

    phy_get_romfunc_addr();
    clk_conf1 = REG(MODEM_SYSCON_CLK_CONF1);
    REG(MODEM_SYSCON_CLK_CONF1) |= 0x404u;
    REG(MODEM_SYSCON_WIFI_BB_CFG) &= ~0x2u;
    force_txrx_off(1u);
    freq_module_resetn();
    phy_dis_hw_set_freq_reg();
    i2c_master_reset();

    if (phy_param[PHY_PARAM_23DBM_EN] != 0u) {
        phy_set_23dbm(&def);
    }
    if (phy_param[PHY_PARAM_PHY_UP] == 0u) {
        register_chipv7_phy_init_param(init_data != NULL ? init_data : &def);
    }
    phy_get_xtal_freq();
    version = phy_get_rf_cal_version();

    if (phy_param[PHY_PARAM_PHY_UP] == 0u) {
        ret = (int)phy_rfcal_data_check(1u, (uint8_t *)cal, (const uint8_t *)init_data, version);
        if (mode == 1) {
            if (ret == 0) {
                rf_cal_data_recovery((const uint8_t *)cal);
                phy_param32_set(PHY_PARAM_PARAM_FLAG, phy_param32(PHY_PARAM_PARAM_FLAG) & 0xfffefd5fu);
            } else {
                mode = 2;
                phy_param32_set(PHY_PARAM_PARAM_FLAG, 0u);
            }
        } else {
            if (ret == 0) {
                phy_rfcal_data_check_value((uint8_t *)cal, buf, 0u);
            }
            phy_param32_set(PHY_PARAM_PARAM_FLAG, 0u);
        }
    }

    flag = phy_param32(PHY_PARAM_PARAM_FLAG);
    rf_init();
    bb_init();
    get_temp_init_new(((flag >> 5) & 1u) ^ 1u, ((flag >> 20) & 1u) ^ 1u);

    if (phy_param[PHY_PARAM_PHY_UP] == 0u && mode != 1) {
        if (ret == 0) {
            ret = phy_rfcal_data_check_value((uint8_t *)cal, buf, 1u) != 0u;
        }
        rf_cal_data_backup((uint8_t *)cal);
        version = phy_get_rf_cal_version();
        phy_rfcal_data_check(0u, (uint8_t *)cal, (const uint8_t *)init_data, version);
    }
    if (phy_param[PHY_PARAM_PHY_UP] == 0u && phy_param[PHY_PARAM_CHAN_OFFSET_EN] != 0u) {
        chip_v7_set_chan_offset(0u);
    }
    phy_bbpll_cal(0u);
    (void)rom_i2c_read(I2C_RFPLL_SDM, I2C_HOST_MST, 0u);
    phy_param[PHY_PARAM_PHY_UP] = 1u;
    phy_en_hw_set_freq();
    force_txrx_off(0u);
    REG(MODEM_SYSCON_CLK_CONF1) = clk_conf1;
    return ret;
}
