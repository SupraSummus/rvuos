/*
 * The receiver's samples, one at a time, on the CYW43439's WLAN processor: see user/wifi/tools/cyw43-iq.py and hw/phy.md.
 *
 * The vendor's firmware sets the radio up and is then stopped for good;
 * the processor, held in reset, starts again here, at vectors the host writes over the firmware's, its interrupts masked.
 * The PHY stays as the firmware left it, receiving on its channel.
 * Each sample is an estimate of the PHY's I/Q estimator, rx_iq_est, over one sample:
 * I times Q, I squared and Q squared, which give I and Q but for one sign common to both.
 * Time is the Cortex-M3's cycle counter, DWT's, at the 81.6 MHz the firmware leaves the processor at, measured here against D11's TSF.
 * With MODE_PSM D11's own processor takes the samples instead, by microcode the host has loaded, into D11's shared memory,
 * and this code starts it, times it and copies what it took.
 * With MODE_STREAM as well, the microcode pushes each chunk of its samples from the shared memory into D11's receive FIFO,
 * whose DMA lays them in RAM, a frame to a descriptor, and this code sets the ring up and times the frames.
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
/* D11's processor, the PSM, see hw/d11.md and tools/cyw43psm.py */
#define MACCONTROL   (D11 + 0x120u)
#define OBJADDR      (D11 + 0x160u)
#define OBJDATA      (D11 + 0x164u)
#define OBJ_SHARED_AUTO_INC 0x03010000u /* the shared memory, in words of 32 bits, an address that steps */
#define PSM_BASE_0   (D11 + 0x4c0u)  /* the microcode's next sample's halfword in the shared memory */
#define PSM_HALTED   0x04000404u     /* the internal registers on, wake, the PSM held at 0 */
#define PSM_RUNNING  0x04000402u     /* the internal registers on, wake, the PSM run */
#define SHARED_FROM  0x100u          /* the halfword the microcode's first sample goes to, the host's too */
#define SHARED_END   0x1000u         /* the shared memory's end, in halfwords */
#define PSM_LEAST    40u             /* cycles: less than any sample of the microcode's takes */
#define PSM_NEAR     4u              /* samples: how near psm_wait reads psm_base_0 on end */
#define RCV_DMA      (D11 + 0x220u)  /* fifo 0's receive DMA: control, ptr, ring address low and high, status 0 and 1 */
#define DMA_ENABLE   1u
#define DMA_OFFSET   0xfeu           /* control's receive offset, bytes before a frame in its buffer */
#define DMA_STATE    0xf0000000u     /* status 0's state, 0 disabled */
#define DMA_CURRENT  0x1fffu         /* status 0's descriptor next filled, as an address's low bits */
#define DESC_SOF     0x80000000u     /* a descriptor's first word, as the firmware posts its own */
#define DESC_EOT     0x10000000u     /* the ring's last */
#define RING         0x22000u        /* MODE_STREAM's descriptors, 16 bytes each, within an 8 KiB of their own */
#define FRAMES       0x24000u        /* MODE_STREAM's frames, a FRAME_SIZE each, run after run */
#define FRAME_SIZE   0x80u
#define MOST_FRAMES  510u            /* a run's, even, so that its n + 1 descriptors lie within RING's 8 KiB */
#define FRAME_LEAST  1000u           /* cycles: less than any frame of samples takes */

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
#define COUNT  (BASE + 0x04) /* the host's: samples in each run, or with MODE_STREAM frames; at the end what each run took */
#define WAIT   (BASE + 0x08) /* the host's: IQ_WAIT's value */
#define GO     (BASE + 0x0c) /* GO_VALUE from the host starts; ENDED once all is done */
#define FAULT  (BASE + 0x10) /* 0xfa000000 and the exception's number, if one came; 0xfb000000 and psm_base_0 if the microcode stopped short
                              or did not start,
                              0xfc000000 and the frames if the stream did */
#define CYCLES (BASE + 0x14) /* the cycles of TSF_SPAN microseconds of D11's TSF */
#define STEP   (BASE + 0x18) /* where the code is: 1 the calibration's setup, 2 + k the tone k, 0x70 its restore */
#define PI     (BASE + 0x1c) /* the host's, with tones: the PHY's state */
#define TONES  (BASE + 0x20) /* the host's: how many tones, at most MOST_TONES */
#define MODE   (BASE + 0x24) /* the host's: how a sample is taken, MODE_* */
#define MODE_FAST  1u        /* no poll, and the low halfwords alone: an 8-bit sample's squares and product fit them */
#define MODE_CHECK 2u        /* with MODE_FAST: IQ_CONTROL read once before the results, its bit 0 kept as LATE */
#define MODE_NO_Q  4u        /* with MODE_FAST or MODE_PSM: Q squared not read, the host taking Q as the product over I */
#define MODE_PSM   8u        /* the host's microcode takes the samples, low halfwords, into SHARED from SHARED_FROM */
#define MODE_STREAM 16u      /* with MODE_PSM: the microcode streams them through D11's receive DMA into FRAMES, COUNT frames a run */
#define STREAMING(mode) (((mode) & (MODE_PSM | MODE_STREAM)) == (MODE_PSM | MODE_STREAM))
#define FREQ   (BASE + 0x40) /* the host's: each tone's frequency in Hz, signed */
#define FIRST  (BASE + 0x80) /* the cycle count at each run's start, the run before the tones first */
#define STREAM_TIMES (BASE + 0x100) /* with MODE_STREAM, four words a run: cycles and frames seen at an eighth, and at the end */
#define OUT     0x21000u     /* the samples, four words each: cycle count, product, I squared, Q squared */
#define OUT_END 0x7e000u
#define OUT_END_TONES 0x2d000u /* with tones, where the cleared RAM ends; the ROM's functions use the RAM above */
#define MAGIC_VALUE 0x49514341u
#define GO_VALUE    2u
#define ENDED       3u
#define MOST_TONES  16u
#define TSF_SPAN    100000u
#define TIMEOUT     80000000u /* cycles, about a second: the most the microcode's run is waited for */
#define POLLS       4096u   /* the most reads of IQ_CONTROL a sample waits for, more than 256 waits' worth */
#define LATE        1u      /* in a sample's cycle count: its estimate had not ended after POLLS reads */
/*
 * So a run ends: TSF_SPAN microseconds, then (1 + TONES) runs of COUNT samples, or frames, at most what most_of allows,
 * each of at most POLLS reads, or with MODE_PSM waits of at most TIMEOUT cycles in all and copies of COUNT samples,
 * and with tones the ROM's functions, which wait on the PHY for bounded times of their own.
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

static volatile uint32_t *take_fast(volatile uint32_t *out, uint32_t n, uint32_t control, uint32_t mode)
{
    for (uint32_t i = 0; i < n; i++, out += 4) {
        uint32_t t = REG(DWT_CYCCNT) & ~LATE, late = 0;
        phy_write(IQ_CONTROL, control | 1u);
        if (mode & MODE_CHECK) {
            late = phy_read(IQ_CONTROL) & 1u;
        }
        out[0] = t | late;
        out[1] = phy_read(IQ_PRODUCT);
        out[2] = phy_read(IQ_I_POWER);
        out[3] = mode & MODE_NO_Q ? 0 : phy_read(IQ_Q_POWER);
    }
    return out;
}

/* The next halfword of the shared memory, which OBJDATA gives two at a time. */
static uint32_t shared_next(uint32_t *word, uint32_t *half)
{
    if (!*half) {
        *word = REG(OBJDATA);
    }
    *half ^= 1u;
    return *half ? *word & 0xffffu : *word >> 16;
}

/*
 * psm_base_0 once it is at least target, or the time is up; the read is D11's, as the microcode's own accesses are,
 * so while the target is more than a few samples off, it waits on its own cycle counter instead, for less than they take.
 */
static uint32_t psm_wait(uint32_t target, uint32_t per, uint32_t t0)
{
    uint32_t v;
    while ((v = REG16(PSM_BASE_0)) < target && REG(DWT_CYCCNT) - t0 < TIMEOUT) {
        uint32_t ahead = (target - v) / per;
        if (ahead > PSM_NEAR) {
            uint32_t from = REG(DWT_CYCCNT);
            while (REG(DWT_CYCCNT) - from < (ahead - PSM_NEAR) * PSM_LEAST) {
            }
        }
    }
    return v;
}

/*
 * The microcode runs from 0, takes n samples of per halfwords and stays, psm_base_0 at its end.
 * Its time is where psm_base_0 is first seen past an eighth of the way and at the end,
 * so that how long it takes to start and stop is left out, and each sample's cycle count is the line through the two.
 */
static volatile uint32_t *take_psm(volatile uint32_t *out, uint32_t n, uint32_t per, uint32_t run)
{
    uint32_t stop = SHARED_FROM + n * per, mark = SHARED_FROM + (n / 8u + 1u) * per, t0, ta, tb, a, v;
    REG(MACCONTROL) = PSM_HALTED;
    t0 = REG(DWT_CYCCNT);
    REG(MACCONTROL) = PSM_RUNNING;
    /* psm_base_0 holds what ran before until the microcode's first instruction sets it to SHARED_FROM */
    while (REG16(PSM_BASE_0) >= mark) {
        if (REG(DWT_CYCCNT) - t0 >= TIMEOUT) {
            REG(FAULT) = 0xfb000000u | REG16(PSM_BASE_0);
            break;
        }
    }
    a = psm_wait(mark, per, t0);
    ta = REG(DWT_CYCCNT);
    v = psm_wait(stop, per, t0);
    tb = REG(DWT_CYCCNT);
    REG(MACCONTROL) = PSM_HALTED;
    if (v != stop) {
        REG(FAULT) = 0xfb000000u | v;
    }
    int32_t ka = (int32_t)((a - SHARED_FROM) / per), kb = (int32_t)((v - SHARED_FROM) / per);
    int32_t step = kb > ka ? (int32_t)(((tb - ta) << 8) / (uint32_t)(kb - ka)) : 0; /* cycles a sample, in 256ths */
    REG(FIRST + 4u * run) = ta - (uint32_t)((ka * step) >> 8);
    REG(OBJADDR) = OBJ_SHARED_AUTO_INC | SHARED_FROM / 2u;
    uint32_t word = 0, half = 0;
    for (uint32_t i = 0; i < n; i++, out += 4) {
        out[0] = (ta + (uint32_t)((((int32_t)i - ka) * step) >> 8)) & ~LATE;
        out[1] = shared_next(&word, &half);
        out[2] = shared_next(&word, &half);
        out[3] = per > 2u ? shared_next(&word, &half) : 0;
    }
    return out;
}

/* The frames receive DMA has filled this run, by its status. */
static uint32_t frames_in(void)
{
    return ((REG(RCV_DMA + 0x10u) & DMA_CURRENT) - (RING & DMA_CURRENT)) / 16u;
}

/* As psm_wait, on the frames: status 0 is D11's too. */
static uint32_t frames_wait(uint32_t target, uint32_t t0)
{
    uint32_t v;
    while ((v = frames_in()) < target && REG(DWT_CYCCNT) - t0 < TIMEOUT) {
        if (target - v > 1u) {
            uint32_t from = REG(DWT_CYCCNT);
            while (REG(DWT_CYCCNT) - from < (target - v - 1u) * FRAME_LEAST) {
            }
        }
    }
    return v;
}

/*
 * A ring of n frames in FRAMES, after the earlier runs', and one descriptor more, never filled, for the ring's end;
 * the microcode pushes n frames, the first of them its second chunk before it was filled, and stays.
 */
static void take_stream(uint32_t n, uint32_t run)
{
    uint32_t at = FRAMES + run * n * FRAME_SIZE, t0, ta, tb, a, v;
    REG(RCV_DMA) &= ~DMA_ENABLE;
    for (uint32_t i = 0; i < 1000u && (REG(RCV_DMA + 0x10u) & DMA_STATE); i++) {
    }
    for (uint32_t i = 0; i <= n; i++) {
        volatile uint32_t *d = (volatile uint32_t *)(RING + 16u * i);
        d[0] = DESC_SOF | (i == n ? DESC_EOT : 0);
        d[1] = FRAME_SIZE;
        d[2] = at + i * FRAME_SIZE;
        d[3] = 0;
    }
    REG(RCV_DMA + 0x08u) = RING;
    REG(RCV_DMA + 0x0cu) = 0;
    REG(RCV_DMA) = (REG(RCV_DMA) & ~DMA_OFFSET) | DMA_ENABLE;
    REG(RCV_DMA + 0x04u) = (RING + 16u * n) & DMA_CURRENT;
    REG(MACCONTROL) = PSM_HALTED;
    t0 = REG(DWT_CYCCNT);
    REG(MACCONTROL) = PSM_RUNNING;
    a = frames_wait(n / 8u + 1u, t0);
    ta = REG(DWT_CYCCNT);
    v = frames_wait(n, t0);
    tb = REG(DWT_CYCCNT);
    REG(MACCONTROL) = PSM_HALTED;
    if (v != n) {
        REG(FAULT) = 0xfc000000u | v;
    }
    REG(STREAM_TIMES + 16u * run) = ta;
    REG(STREAM_TIMES + 16u * run + 4u) = a;
    REG(STREAM_TIMES + 16u * run + 8u) = tb;
    REG(STREAM_TIMES + 16u * run + 12u) = v;
}

static volatile uint32_t *take(volatile uint32_t *out, uint32_t n, uint32_t run)
{
    uint32_t control = phy_read(IQ_CONTROL) & ~3u, mode = REG(MODE);
    phy_write(IQ_SAMPLES, 1);
    phy_write(IQ_WAIT, (phy_read(IQ_WAIT) & ~0xffu) | (REG(WAIT) & 0xffu));
    phy_write(CLOCKS, phy_read(CLOCKS) | ESTIMATE_CLOCKS);
    REG(FIRST + 4u * run) = REG(DWT_CYCCNT);
    if (STREAMING(mode)) {
        take_stream(n, run);
        return out;
    }
    if (mode & MODE_PSM) {
        return take_psm(out, n, mode & MODE_NO_Q ? 2u : 3u, run);
    }
    if (mode & MODE_FAST) {
        return take_fast(out, n, control, mode);
    }
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

/* The most a run takes: samples in OUT, or in the shared memory, or with MODE_STREAM frames in FRAMES, an even number. */
static uint32_t most_of(uint32_t mode, uint32_t tones)
{
    uint32_t end = tones ? OUT_END_TONES : OUT_END, most = (end - OUT) / 16u / (1u + tones);
    if (STREAMING(mode)) {
        most = ((end - FRAMES) / FRAME_SIZE - 1u) / (1u + tones);
        return (most < MOST_FRAMES ? most : MOST_FRAMES) & ~1u;
    }
    if (mode & MODE_PSM) {
        uint32_t shared = (SHARED_END - SHARED_FROM) / (mode & MODE_NO_Q ? 2u : 3u);
        return most < shared ? most : shared;
    }
    return most;
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
    uint32_t most = most_of(REG(MODE), tones), n = REG(COUNT) < most ? REG(COUNT) : most;
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
