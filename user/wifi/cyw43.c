/*
 * The CYW43439's bus and boot; see cyw43.h.
 *
 * The chip answers on three functions of its gSPI bus: the bus's own registers (0),
 * the backplane (1), a window of 32 KiB onto the chip's address space, which a window register moves,
 * and WLAN (2), the packets to and from the firmware.
 * Until the bus is told otherwise it speaks 16-bit words, so the first words go with their halves swapped.
 * Booting is: bring the ALP clock up, hold the WLAN core and reset the RAM's,
 * write the firmware at RAM's start and the NVRAM at its end with its length after it,
 * let the WLAN core go, and wait for the HT clock and for function 2.
 */

#include "cyw43.h"

#include "lib.h"

#define FUNC_BUS       0u
#define FUNC_BACKPLANE 1u
#define FUNC_WLAN      2u

#define REG_BUS_CTRL             0x00u
#define REG_BUS_RESPONSE_DELAY   0x01u
#define REG_BUS_STATUS_ENABLE    0x02u
#define REG_BUS_INTERRUPT        0x04u
#define REG_BUS_INTERRUPT_ENABLE 0x06u
#define REG_BUS_STATUS           0x08u
#define REG_BUS_TEST_RO          0x14u
#define REG_BUS_TEST_RW          0x18u
#define REG_BUS_RESP_DELAY_F1    0x1du

#define WORD_LENGTH_32          0x01u
#define HIGH_SPEED              0x10u
#define INTERRUPT_POLARITY_HIGH 0x20u
#define WAKE_UP                 0x80u
#define STATUS_ENABLE           0x01u
#define INTR_WITH_STATUS        0x02u

#define STATUS_F2_RX_READY 0x20u

#define IRQ_DATA_UNAVAILABLE        0x0001u
#define IRQ_F2_F3_FIFO_RD_UNDERFLOW 0x0002u
#define IRQ_F2_F3_FIFO_WR_OVERFLOW  0x0004u
#define IRQ_COMMAND_ERROR           0x0008u
#define IRQ_DATA_ERROR              0x0010u
#define IRQ_F2_PACKET_AVAILABLE     0x0020u
#define IRQ_F1_OVERFLOW             0x0080u

#define REG_BACKPLANE_F2_WATERMARK  0x10008u
#define REG_BACKPLANE_ADDRESS_LOW   0x1000au
#define REG_BACKPLANE_ADDRESS_MID   0x1000bu
#define REG_BACKPLANE_ADDRESS_HIGH  0x1000cu
#define REG_BACKPLANE_CHIP_CLOCK_CSR 0x1000eu
#define REG_BACKPLANE_PULL_UP       0x1000fu

#define BACKPLANE_ADDRESS_MASK  0x7fffu
#define BACKPLANE_ADDRESS_32BIT 0x8000u
#define BACKPLANE_WINDOW_SIZE   0x8000u
#define BACKPLANE_MAX_TRANSFER  64u
#define CLOCK_ALP_AVAIL_REQ     0x08u
#define CLOCK_ALP_AVAIL         0x40u
#define CLOCK_HT_AVAIL_REQ      0x10u
#define CLOCK_HT_AVAIL          0x80u

#define AI_IOCTRL     0x408u
#define AI_RESETCTRL  0x800u
#define IOCTRL_CLOCK_EN 0x01u
#define IOCTRL_FGC      0x02u
#define IOCTRL_CPUHALT  0x20u
#define RESETCTRL_RESET 0x01u

#define CHIPCOMMON_BASE      0x18000000u
#define SDIOD_CORE_BASE      0x18002000u
#define WLAN_WRAPPER_BASE    0x18103000u
#define SOCSRAM_BASE         0x18004000u
#define SOCSRAM_WRAPPER_BASE 0x18104000u
#define SDIO_INT_HOST_MASK   0x24u
#define I_HMB_SW_MASK        0xf0u
#define CHIP_RAM_SIZE        (512u * 1024u)
#define F2_WATERMARK_BOOT    0x10u
#define F2_WATERMARK         0x20u

#define FEEDBEAD     0xfeedbeadu
#define TEST_PATTERN 0x12345678u

static uint32_t cmd_word(uint32_t write, uint32_t func, uint32_t addr, uint32_t len)
{
    /* Every command here increments the address. */
    return (write << 31) | (1u << 30) | ((func & 3u) << 28) | ((addr & 0x1ffffu) << 11) | (len & 0x7ffu);
}

static uint32_t swap16(uint32_t x)
{
    return (x << 16) | (x >> 16);
}

static int fail(struct cyw43 *c, uint32_t step, uint32_t detail)
{
    c->step = step;
    c->detail = detail;
    return -1;
}

static uint32_t read32_swapped(struct cyw43 *c, uint32_t addr)
{
    uint32_t v = 0;
    c->status = gspi_read(&c->bus, swap16(cmd_word(0, FUNC_BUS, addr, 4)), &v, 1);
    return swap16(v);
}

static void write32_swapped(struct cyw43 *c, uint32_t addr, uint32_t val)
{
    uint32_t w[2] = { swap16(cmd_word(1, FUNC_BUS, addr, 4)), swap16(val) };
    c->status = gspi_write(&c->bus, w, 2);
}

/* A backplane read answers after a word of padding, SPI_RESP_DELAY_F1's. */
static uint32_t readn(struct cyw43 *c, uint32_t func, uint32_t addr, uint32_t len)
{
    uint32_t w[2] = { 0, 0 };
    c->status = gspi_read(&c->bus, cmd_word(0, func, addr, len), w, func == FUNC_BACKPLANE ? 2 : 1);
    return func == FUNC_BACKPLANE ? w[1] : w[0];
}

static void writen(struct cyw43 *c, uint32_t func, uint32_t addr, uint32_t val, uint32_t len)
{
    uint32_t w[2] = { cmd_word(1, func, addr, len), val };
    c->status = gspi_write(&c->bus, w, 2);
}

static void set_window(struct cyw43 *c, uint32_t addr)
{
    uint32_t w = addr & ~BACKPLANE_ADDRESS_MASK;
    if (((w >> 24) & 0xffu) != ((c->window >> 24) & 0xffu)) {
        writen(c, FUNC_BACKPLANE, REG_BACKPLANE_ADDRESS_HIGH, (w >> 24) & 0xffu, 1);
    }
    if (((w >> 16) & 0xffu) != ((c->window >> 16) & 0xffu)) {
        writen(c, FUNC_BACKPLANE, REG_BACKPLANE_ADDRESS_MID, (w >> 16) & 0xffu, 1);
    }
    if (((w >> 8) & 0xffu) != ((c->window >> 8) & 0xffu)) {
        writen(c, FUNC_BACKPLANE, REG_BACKPLANE_ADDRESS_LOW, (w >> 8) & 0xffu, 1);
    }
    c->window = w;
}

static uint32_t bp_readn(struct cyw43 *c, uint32_t addr, uint32_t len)
{
    set_window(c, addr);
    uint32_t bus_addr = addr & BACKPLANE_ADDRESS_MASK;
    if (len == 4) {
        bus_addr |= BACKPLANE_ADDRESS_32BIT;
    }
    return readn(c, FUNC_BACKPLANE, bus_addr, len);
}

static void bp_writen(struct cyw43 *c, uint32_t addr, uint32_t val, uint32_t len)
{
    set_window(c, addr);
    uint32_t bus_addr = addr & BACKPLANE_ADDRESS_MASK;
    if (len == 4) {
        bus_addr |= BACKPLANE_ADDRESS_32BIT;
    }
    writen(c, FUNC_BACKPLANE, bus_addr, val, len);
}

static uint8_t bp_read8(struct cyw43 *c, uint32_t addr)
{
    return (uint8_t)bp_readn(c, addr, 1);
}

static void bp_write8(struct cyw43 *c, uint32_t addr, uint8_t val)
{
    bp_writen(c, addr, val, 1);
}

uint16_t cyw43_bp_read16(struct cyw43 *c, uint32_t addr)
{
    return (uint16_t)bp_readn(c, addr, 2);
}

uint32_t cyw43_bp_read32(struct cyw43 *c, uint32_t addr)
{
    return bp_readn(c, addr, 4);
}

static void bp_write32(struct cyw43 *c, uint32_t addr, uint32_t val)
{
    bp_writen(c, addr, val, 4);
}

static uint8_t read8(struct cyw43 *c, uint32_t func, uint32_t addr)
{
    return (uint8_t)readn(c, func, addr, 1);
}

static void write8(struct cyw43 *c, uint32_t func, uint32_t addr, uint8_t val)
{
    writen(c, func, addr, val, 1);
}

static void write16(struct cyw43 *c, uint32_t func, uint32_t addr, uint16_t val)
{
    writen(c, func, addr, val, 2);
}

/* Polls a byte of the backplane's function registers for bits, every millisecond, for at most ms of them. */
static int wait_bits8(struct cyw43 *c, uint32_t addr, uint8_t bits, uint32_t ms)
{
    for (uint32_t i = 0; i <= ms; i++) {
        if (read8(c, FUNC_BACKPLANE, addr) & bits) {
            return 0;
        }
        c->sleep(c, 1000);
    }
    return -1;
}

void cyw43_load(struct cyw43 *c, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t words[1 + BACKPLANE_MAX_TRANSFER / 4];
    while (len) {
        uint32_t offs = addr & BACKPLANE_ADDRESS_MASK;
        uint32_t n = len < BACKPLANE_MAX_TRANSFER ? len : BACKPLANE_MAX_TRANSFER;
        if (n > BACKPLANE_WINDOW_SIZE - offs) {
            n = BACKPLANE_WINDOW_SIZE - offs;
        }
        memset(&words[1], 0, sizeof(words) - sizeof(words[0]));
        memcpy(&words[1], data, n);
        set_window(c, addr);
        words[0] = cmd_word(1, FUNC_BACKPLANE, offs, n);
        c->status = gspi_write(&c->bus, words, 1 + (n + 3) / 4);
        addr += n;
        data += n;
        len -= n;
    }
}

int cyw43_verify(struct cyw43 *c, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t words[1 + BACKPLANE_MAX_TRANSFER / 4];
    while (len) {
        uint32_t offs = addr & BACKPLANE_ADDRESS_MASK;
        uint32_t n = len < BACKPLANE_MAX_TRANSFER ? len : BACKPLANE_MAX_TRANSFER;
        if (n > BACKPLANE_WINDOW_SIZE - offs) {
            n = BACKPLANE_WINDOW_SIZE - offs;
        }
        set_window(c, addr);
        c->status = gspi_read(&c->bus, cmd_word(0, FUNC_BACKPLANE, offs, n), words, 1 + (n + 3) / 4);
        if (memcmp(&words[1], data, n) != 0) {
            return fail(c, STEP_FW_VERIFY, addr);
        }
        addr += n;
        data += n;
        len -= n;
    }
    return 0;
}

static void disable_core(struct cyw43 *c, uint32_t base)
{
    if (bp_read8(c, base + AI_RESETCTRL) & RESETCTRL_RESET) {
        return;
    }
    bp_write8(c, base + AI_IOCTRL, 0);
    c->sleep(c, 1000);
    bp_write8(c, base + AI_RESETCTRL, RESETCTRL_RESET);
    c->sleep(c, 1000);
}

static void reset_core(struct cyw43 *c, uint32_t base)
{
    disable_core(c, base);
    bp_write8(c, base + AI_IOCTRL, IOCTRL_FGC | IOCTRL_CLOCK_EN);
    (void)bp_read8(c, base + AI_IOCTRL);
    bp_write8(c, base + AI_RESETCTRL, 0);
    c->sleep(c, 1000);
    bp_write8(c, base + AI_IOCTRL, IOCTRL_CLOCK_EN);
    (void)bp_read8(c, base + AI_IOCTRL);
    c->sleep(c, 1000);
}

int cyw43_bus_up(struct cyw43 *c)
{
    c->window = 0xaaaaaaaau;
    gspi_hold_data_low(&c->bus);
    gspi_power(&c->bus, 0);
    c->sleep(c, 20000);
    gspi_power(&c->bus, 1);
    c->sleep(c, 250000);

    uint32_t v = 0;
    for (uint32_t i = 0; i < 50 && (v = read32_swapped(c, REG_BUS_TEST_RO)) != FEEDBEAD; i++) {
        c->sleep(c, 10000);
    }
    if (v != FEEDBEAD) {
        return fail(c, STEP_BUS_TEST, v);
    }
    write32_swapped(c, REG_BUS_TEST_RW, TEST_PATTERN);
    v = read32_swapped(c, REG_BUS_TEST_RW);
    if (v != TEST_PATTERN) {
        return fail(c, STEP_BUS_RW, v);
    }

    write32_swapped(c, REG_BUS_CTRL,
                    WORD_LENGTH_32 | HIGH_SPEED | INTERRUPT_POLARITY_HIGH | WAKE_UP |
                        (0x4u << (8 * REG_BUS_RESPONSE_DELAY)) |
                        ((STATUS_ENABLE | INTR_WITH_STATUS) << (8 * REG_BUS_STATUS_ENABLE)));
    v = readn(c, FUNC_BUS, REG_BUS_TEST_RO, 4);
    if (v != FEEDBEAD) {
        return fail(c, STEP_BUS_CONFIG, v);
    }
    v = readn(c, FUNC_BUS, REG_BUS_TEST_RW, 4);
    if (v != TEST_PATTERN) {
        return fail(c, STEP_BUS_CONFIG, v);
    }

    write8(c, FUNC_BUS, REG_BUS_RESP_DELAY_F1, 4);
    write8(c, FUNC_BUS, REG_BUS_INTERRUPT,
           IRQ_DATA_UNAVAILABLE | IRQ_COMMAND_ERROR | IRQ_DATA_ERROR | IRQ_F1_OVERFLOW);
    write16(c, FUNC_BUS, REG_BUS_INTERRUPT_ENABLE,
            IRQ_F2_F3_FIFO_RD_UNDERFLOW | IRQ_F2_F3_FIFO_WR_OVERFLOW | IRQ_COMMAND_ERROR | IRQ_DATA_ERROR |
                IRQ_F2_PACKET_AVAILABLE | IRQ_F1_OVERFLOW);
    return 0;
}

int cyw43_prepare(struct cyw43 *c)
{
    write8(c, FUNC_BACKPLANE, REG_BACKPLANE_CHIP_CLOCK_CSR, CLOCK_ALP_AVAIL_REQ);
    write8(c, FUNC_BACKPLANE, REG_BACKPLANE_F2_WATERMARK, F2_WATERMARK_BOOT);
    if (wait_bits8(c, REG_BACKPLANE_CHIP_CLOCK_CSR, CLOCK_ALP_AVAIL, 100) != 0) {
        return fail(c, STEP_ALP, read8(c, FUNC_BACKPLANE, REG_BACKPLANE_CHIP_CLOCK_CSR));
    }
    write8(c, FUNC_BACKPLANE, REG_BACKPLANE_CHIP_CLOCK_CSR, 0);

    uint32_t id = cyw43_bp_read16(c, CHIPCOMMON_BASE);
    if (id != CYW43_CHIP_ID) {
        return fail(c, STEP_CHIP_ID, id);
    }

    disable_core(c, WLAN_WRAPPER_BASE);
    disable_core(c, SOCSRAM_WRAPPER_BASE);
    reset_core(c, SOCSRAM_WRAPPER_BASE);
    /* The 43439's RAM: its banks' power-down and its remap off, as WHD sets them. */
    bp_write32(c, SOCSRAM_BASE + 0x10u, 3);
    bp_write32(c, SOCSRAM_BASE + 0x44u, 0);
    return 0;
}

int cyw43_start(struct cyw43 *c, const uint8_t *nvram, uint32_t len)
{
    uint32_t padded = (len + 3u) & ~3u;
    uint32_t addr = CHIP_RAM_SIZE - 4u - padded;
    cyw43_load(c, addr, nvram, padded);
    uint32_t words = padded / 4u;
    uint32_t magic = (~words << 16) | words;
    bp_write32(c, CHIP_RAM_SIZE - 4u, magic);
    uint32_t back = cyw43_bp_read32(c, CHIP_RAM_SIZE - 4u);
    if (back != magic) {
        return fail(c, STEP_NVRAM_VERIFY, back);
    }

    reset_core(c, WLAN_WRAPPER_BASE);
    uint8_t io = bp_read8(c, WLAN_WRAPPER_BASE + AI_IOCTRL);
    uint8_t rst = bp_read8(c, WLAN_WRAPPER_BASE + AI_RESETCTRL);
    if ((io & (IOCTRL_FGC | IOCTRL_CLOCK_EN)) != IOCTRL_CLOCK_EN || (rst & RESETCTRL_RESET)) {
        return fail(c, STEP_CORE_UP, ((uint32_t)io << 8) | rst);
    }
    if (wait_bits8(c, REG_BACKPLANE_CHIP_CLOCK_CSR, CLOCK_HT_AVAIL, 500) != 0) {
        return fail(c, STEP_HT, read8(c, FUNC_BACKPLANE, REG_BACKPLANE_CHIP_CLOCK_CSR));
    }

    bp_write32(c, SDIOD_CORE_BASE + SDIO_INT_HOST_MASK, I_HMB_SW_MASK);
    write16(c, FUNC_BUS, REG_BUS_INTERRUPT_ENABLE, IRQ_F2_PACKET_AVAILABLE);
    write8(c, FUNC_BACKPLANE, REG_BACKPLANE_F2_WATERMARK, F2_WATERMARK);

    uint32_t status = 0;
    for (uint32_t i = 0; i <= 1000 && !((status = readn(c, FUNC_BUS, REG_BUS_STATUS, 4)) & STATUS_F2_RX_READY); i++) {
        c->sleep(c, 1000);
    }
    if (!(status & STATUS_F2_RX_READY)) {
        return fail(c, STEP_F2, status);
    }

    /* The pads' pulls off, and the HT clock asked for again, as embassy's driver does once F2 is ready. */
    write8(c, FUNC_BACKPLANE, REG_BACKPLANE_PULL_UP, 0);
    (void)read8(c, FUNC_BACKPLANE, REG_BACKPLANE_PULL_UP);
    write8(c, FUNC_BACKPLANE, REG_BACKPLANE_CHIP_CLOCK_CSR, CLOCK_HT_AVAIL_REQ);
    if (wait_bits8(c, REG_BACKPLANE_CHIP_CLOCK_CSR, CLOCK_HT_AVAIL, 500) != 0) {
        return fail(c, STEP_HT, read8(c, FUNC_BACKPLANE, REG_BACKPLANE_CHIP_CLOCK_CSR));
    }
    return 0;
}

uint32_t cyw43_status(struct cyw43 *c)
{
    return readn(c, FUNC_BUS, REG_BUS_STATUS, 4);
}

void cyw43_wlan_read(struct cyw43 *c, uint32_t *words, uint32_t len)
{
    c->status = gspi_read(&c->bus, cmd_word(0, FUNC_WLAN, 0, len), words, (len + 3u) / 4u);
}

void cyw43_wlan_write(struct cyw43 *c, uint32_t *words, uint32_t len)
{
    words[0] = cmd_word(1, FUNC_WLAN, 0, len);
    c->status = gspi_write(&c->bus, words, 1u + (len + 3u) / 4u);
}
