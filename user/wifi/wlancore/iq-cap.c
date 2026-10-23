/*
 * The receiver's samples, one at a time, on the CYW43439's WLAN processor: see user/wifi/tools/cyw43-iq.py and hw/phy.md.
 *
 * The vendor's firmware sets the radio up and is then stopped for good;
 * the processor, held in reset, starts again here, at vectors the host writes over the firmware's, its interrupts masked.
 * The PHY stays as the firmware left it, receiving on its channel.
 * Each sample is an estimate of the PHY's I/Q estimator, rx_iq_est, over one sample:
 * I times Q, I squared and Q squared, which give I and Q but for one sign common to both.
 * Time is the Cortex-M3's cycle counter, DWT's, at the 81.6 MHz the firmware leaves the processor at, measured here against D11's TSF.
 *
 * With tones, the control: the firmware's own receive IQ calibration is set up and its tones played,
 * by the ROM's functions on the firmware's PHY state, which the host finds, and a run of samples taken during each,
 * after one taken before any; the calibration's loopback, and its tones at its amplitude, as the firmware itself plays them.
 * Those functions use the firmware's RAM, so all of this code's lies in RAM the firmware had cleared, see the host.
 */
#include <stdint.h>

#define REG(a)   (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))
#define REG8(a)  (*(volatile uint8_t *)(a))
#define DEMCR      0xe000edfcu
#define DWT_CTRL   0xe0001000u
#define DWT_CYCCNT 0xe0001004u

#define D11          0x18001000u
#define TSF_LOW      (D11 + 0x180u)  /* D11's TSF, in microseconds */
#define PHYREGADDR   (D11 + 0x3fcu)  /* a halfword, or a word of the data above the address, one write */
#define PHYREGDATA   (D11 + 0x3feu)
#define CLOCKS       0x6dau          /* the PHY's clock enables; bits 5 and 8 the estimator's */
#define ESTIMATE_CLOCKS 0x120u
#define IQ_CONTROL   0x481u          /* bit 0 starts an estimate and reads set until it ends */
#define IQ_WAIT      0x482u          /* bits 0 to 7: samples at 20 MHz before the estimate takes its own; 0 and 1 wait 256 */
#define IQ_SAMPLES   0x483u          /* how many samples an estimate takes */
#define IQ_PRODUCT   0x484u          /* I times Q, two halfwords, the low first, two's complement */
#define IQ_I_POWER   0x486u          /* I squared, two halfwords */
#define IQ_Q_POWER   0x488u          /* Q squared, its low halfword; its high one at IQ_Q_POWER_HI */
#define IQ_Q_POWER_HI 0xa9au

/* The ROM's functions the calibration runs, 0x872770's, each taking the PHY's state, pi, first; Thumb addresses. */
typedef void (*phy_fn)(uint32_t pi, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define CAL_SETUP    ((phy_fn)0x872849u) /* the loopback and its gains */
#define CAL_9DE      ((phy_fn)0x872e01u) /* with 2, 0x9de and 0x9df cleared */
#define TONE         ((phy_fn)0x86ea6du) /* wlc_lcn20phy_start_tx_tone: pi, Hz, amplitude, 0, 1, 1 as the calibration has it */
#define TONE_STOP    ((phy_fn)0x86eb6du)
#define CAL_RESTORE  ((phy_fn)0x872819u)
#define CAL_AMPLITUDE 0xb5u
#define LCN_CAL_TYPE 0x566u              /* bytes of the LCN20's own state, pi's word at 0x40, as 0x872770 sets them */
#define LCN_CAL_FLAG 0x565u

/* The host's words, in RAM the vendor's firmware had cleared, and the code itself at CODE, its vectors at 0 as well. */
#define CODE   0x20000u
#define BASE   0x1f000u
#define STACK  BASE
#define MAGIC  (BASE + 0x00) /* MAGIC_VALUE once this code runs */
#define COUNT  (BASE + 0x04) /* the host's: samples in each run */
#define WAIT   (BASE + 0x08) /* the host's: IQ_WAIT's value */
#define GO     (BASE + 0x0c) /* GO_VALUE from the host starts; ENDED once all is done */
#define FAULT  (BASE + 0x10) /* 0xfa000000 and the exception's number, if one came */
#define CYCLES (BASE + 0x14) /* the cycles of TSF_SPAN microseconds of D11's TSF */
#define STEP   (BASE + 0x18) /* where the code is: 1 the calibration's setup, 2 + k the tone k, 0x70 its restore */
#define PI     (BASE + 0x1c) /* the host's, with tones: the PHY's state */
#define TONES  (BASE + 0x20) /* the host's: how many tones, at most MOST_TONES */
#define FREQ   (BASE + 0x40) /* the host's: each tone's frequency in Hz, signed */
#define FIRST  (BASE + 0x80) /* the cycle count at each run's start, the run before the tones first */
#define OUT     0x21000u     /* the samples, four words each: cycle count, product, I squared, Q squared */
#define OUT_END 0x7e000u
#define OUT_END_TONES 0x2d000u /* with tones, where the cleared RAM ends; the ROM's functions use the RAM above */
#define MAGIC_VALUE 0x49514341u
#define GO_VALUE    2u
#define ENDED       3u
#define MOST_TONES  16u
#define TSF_SPAN    100000u
#define POLLS       4096u   /* the most reads of IQ_CONTROL a sample waits for, more than 256 waits' worth */
#define LATE        1u      /* in a sample's cycle count: its estimate had not ended after POLLS reads */
/*
 * So a run ends: TSF_SPAN microseconds, then (1 + TONES) runs of COUNT samples, at most what fits before the end,
 * each of at most POLLS reads, and with tones the ROM's functions, which wait on the PHY for bounded times of their own.
 */

static uint32_t phy_read(uint32_t reg)
{
    REG16(PHYREGADDR) = (uint16_t)reg;
    return REG16(PHYREGDATA);
}

static void phy_write(uint32_t reg, uint32_t value)
{
    REG(PHYREGADDR) = value << 16 | reg;
}

static uint32_t phy_read32(uint32_t lo, uint32_t hi)
{
    return phy_read(hi) << 16 | phy_read(lo);
}

void iq_start(void);
void iq_fault(void);

__attribute__((section(".vectors"), used)) const uint32_t iq_vectors[16] = {
    STACK, (uint32_t)iq_start, (uint32_t)iq_fault, (uint32_t)iq_fault, (uint32_t)iq_fault, (uint32_t)iq_fault, (uint32_t)iq_fault,
};

void iq_fault(void)
{
    uint32_t ipsr;
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    REG(FAULT) = 0xfa000000u | ipsr;
    for (;;) {
    }
}

static volatile uint32_t *take(volatile uint32_t *out, uint32_t n, uint32_t run)
{
    uint32_t control = phy_read(IQ_CONTROL) & ~3u;
    phy_write(IQ_SAMPLES, 1);
    phy_write(IQ_WAIT, (phy_read(IQ_WAIT) & ~0xffu) | (REG(WAIT) & 0xffu));
    phy_write(CLOCKS, phy_read(CLOCKS) | ESTIMATE_CLOCKS);
    REG(FIRST + 4u * run) = REG(DWT_CYCCNT);
    for (uint32_t i = 0; i < n; i++, out += 4) {
        uint32_t t = REG(DWT_CYCCNT) & ~LATE;
        phy_write(IQ_CONTROL, control | 1u);
        uint32_t polls = 0;
        while ((phy_read(IQ_CONTROL) & 1u) && ++polls < POLLS) {
        }
        out[0] = t | (polls == POLLS ? LATE : 0);
        out[1] = phy_read32(IQ_PRODUCT, IQ_PRODUCT + 1);
        out[2] = phy_read32(IQ_I_POWER, IQ_I_POWER + 1);
        out[3] = phy_read32(IQ_Q_POWER, IQ_Q_POWER_HI);
    }
    return out;
}

void iq_start(void)
{
    __asm__ volatile("cpsid i");
    REG(DEMCR) |= 1u << 24;
    REG(DWT_CYCCNT) = 0;
    REG(DWT_CTRL) |= 1u;
    REG(MAGIC) = MAGIC_VALUE;
    while (REG(GO) != GO_VALUE) {
    }
    uint32_t s0 = REG(TSF_LOW), c0 = REG(DWT_CYCCNT);
    while (REG(TSF_LOW) - s0 < TSF_SPAN) {
    }
    REG(CYCLES) = REG(DWT_CYCCNT) - c0;

    uint32_t tones = REG(TONES) < MOST_TONES ? REG(TONES) : MOST_TONES, pi = REG(PI);
    uint32_t most = ((tones ? OUT_END_TONES : OUT_END) - OUT) / 16u / (1u + tones), n = REG(COUNT) < most ? REG(COUNT) : most;
    uint32_t clocks = phy_read(CLOCKS);
    volatile uint32_t *out = (volatile uint32_t *)OUT;
    if (tones) {
        uint32_t lcn = REG(pi + 0x40u), scratch[2];
        REG8(lcn + LCN_CAL_TYPE) = 2;
        REG8(lcn + LCN_CAL_FLAG) = 0;
        REG(STEP) = 1;
        CAL_SETUP(pi, 0, 0, 0, 0, 0);
        CAL_9DE(pi, 2, (uint32_t)scratch, 0, 0, 0);
    }
    out = take(out, n, 0);
    for (uint32_t k = 0; k < tones; k++) {
        REG(STEP) = 2 + k;
        TONE(pi, REG(FREQ + 4u * k), CAL_AMPLITUDE, 0, 1, 1);
        out = take(out, n, 1 + k);
        TONE_STOP(pi, 0, 0, 0, 0, 0);
    }
    if (tones) {
        REG(STEP) = 0x70;
        CAL_RESTORE(pi, 0, 0, 0, 0, 0);
    }
    phy_write(CLOCKS, clocks);
    REG(COUNT) = n;
    REG(GO) = ENDED;
    for (;;) {
    }
}
