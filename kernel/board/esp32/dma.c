/*
 * The GDMA of the ESP32-C6 and the ESP32-C5, the DMA controller on the AHB, as dma.h's controller.
 *
 * The GDMA has three pairs of channels, a transmit channel reading memory into a device and a receive channel
 * writing what a device gives into memory; the kernel lends the three transmit channels, DMA_FIRST on,
 * so that a channel's source is memory and its sink a device.
 * A channel follows a chain of descriptors in memory, each naming a buffer of at most 4095 bytes and the next,
 * and feeds the device its PERI_SEL names, one of board.h's GDMA_DEVICE_LIST.
 * The kernel writes a channel's descriptors in its own memory, DMA_DESCRIPTORS of them,
 * so the channel reads only the range a start checked, whatever the program writes meanwhile.
 * Nothing but the kernel reaches the GDMA's registers: board.h lists no frame over them.
 *
 * A halt stops the channel's descriptors and resets it, its FIFO with it, and connects it to no device.
 * A transmit channel only reads memory, so a read that was under way as the reset came lands in a FIFO that is gone.
 *
 * The two chips' GDMAs differ in little, which board.h's GDMA_V2 says: the C5's, AHB DMA version 2,
 * takes the first descriptor's address in a register of its own, OUT_LINK_ADDR, rather than in OUT_LINK's low bits,
 * so OUT_LINK's own bits lie lower; it reports a bus's refusal, RESP_ERR;
 * and it has a window, INTR_MEM_START to INTR_MEM_END, which keeps every channel within the SRAM,
 * where no frame of a program's lies outside of anyway; the C6's has none.
 *
 * Register addresses are those of Espressif's ESP-IDF, soc/esp32c6's gdma_reg.h, soc/esp32c5's ahb_dma_reg.h,
 * and each chip's pcr_reg.h.
 */

#include "dma.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/* The GDMA's clock and reset, in PCR, board.h's GDMA_PCR_CONF. */
#define PCR_GDMA_CLK_EN      (1u << 0)
#define PCR_GDMA_RST_EN      (1u << 1)

#define GDMA_BASE            0x60080000u
#define GDMA_OUT_INT_RAW(c)  (GDMA_BASE + 0x30u + 0x10u * (c))
#define GDMA_OUT_INT_ENA(c)  (GDMA_BASE + 0x38u + 0x10u * (c))
#define GDMA_OUT_INT_CLR(c)  (GDMA_BASE + 0x3cu + 0x10u * (c))
#define GDMA_MISC_CONF       (GDMA_BASE + 0x64u)
#define GDMA_OUT_CONF0(c)    (GDMA_BASE + 0xd0u + 0xc0u * (c))
#define GDMA_OUT_LINK(c)     (GDMA_BASE + 0xe0u + 0xc0u * (c))
#define GDMA_OUT_PERI_SEL(c) (GDMA_BASE + 0x100u + 0xc0u * (c))

#if GDMA_V2
#define GDMA_OUT_LINK_ADDR(c) (GDMA_BASE + 0x3b8u + 0x4u * (c))
#define GDMA_INTR_MEM_START  (GDMA_BASE + 0x3c4u)
#define GDMA_INTR_MEM_END    (GDMA_BASE + 0x3c8u)
#define OUTLINK_STOP         (1u << 0)
#define OUTLINK_START        (1u << 1)
#define OUTLINK_PARK         (1u << 3) /* the descriptors' state machine is idle */
#define OUT_RESP_ERR         (1u << 6) /* the bus refused an access, outside the window say */
#else
#define OUTLINK_ADDR         0x000fffffu /* the first descriptor's address, its low bits, within the SRAM */
#define OUTLINK_STOP         (1u << 20)
#define OUTLINK_START        (1u << 21)
#define OUTLINK_PARK         (1u << 23)
#define OUT_RESP_ERR         0u
#endif

#define MISC_CLK_EN          (1u << 3) /* the registers' clock always on */
#define OUT_RST              (1u << 0) /* the channel's state machine and FIFO, held in reset */
#define OUT_EOF_MODE         (1u << 3) /* the end counts once the last byte has left the FIFO for the device */
#define OUT_DSCR_ERR         (1u << 2)
#define OUT_TOTAL_EOF        (1u << 3) /* the descriptor with SUC_EOF is done */
#define OUT_INT_ALL          0x7fu
#define PERI_NONE            0x3fu

/* A descriptor: its buffer's size and length, its end and that the GDMA owns it; the buffer; the next, or 0. */
#define DSCR_SIZE(n)         ((uint32_t)(n))
#define DSCR_LENGTH(n)       ((uint32_t)(n) << 12)
#define DSCR_SUC_EOF         (1u << 30)
#define DSCR_OWNER           (1u << 31)
#define DSCR_LENGTH_OF(w)    (((w) >> 12) & 0xfffu)
#define DSCR_MOST            4092u /* 4095 bytes, to the grain */

_Static_assert(DMA_MOST == DMA_DESCRIPTORS * DSCR_MOST, "the longest transfer fills every descriptor");
_Static_assert(DMA_FIRST + DMA_CHANNELS <= 3u, "the GDMA has three transmit channels");

struct dscr {
    uint32_t word;
    uint32_t buffer;
    uint32_t next;
};

/* Each channel's descriptors, and whether it was started since its last halt. */
static struct dscr dscr[DMA_CHANNELS][DMA_DESCRIPTORS];
static bool started[DMA_CHANNELS];

/* A device a channel may feed, by the request PERI_SEL takes for it, which a frame names by holding its block whole. */
static const struct {
    uint32_t request, base, size;
} devices[] = { GDMA_DEVICE_LIST };
#define DEVICES (sizeof(devices) / sizeof(devices[0]))

uint32_t dmac_memory(uint32_t channel)
{
    (void)channel;
    return DMA_SOURCE;
}

bool dmac_end(uint32_t channel, uint32_t end, uint32_t base, uint32_t size, uint32_t name, uint32_t *word)
{
    (void)channel;
    if (end == DMA_SOURCE) {
        *word = base;
        return name == 0 && base >= RAM_BASE && base - RAM_BASE < RAM_SIZE && size <= RAM_BASE + RAM_SIZE - base;
    }
    for (uint32_t i = 0; i < DEVICES; i++) {
        LOOP_BOUND(DEVICES);
        if (devices[i].request == name && base <= devices[i].base &&
            devices[i].base - base <= size && devices[i].size <= size - (devices[i].base - base)) {
            *word = name;
            return true;
        }
    }
    return false;
}

void dmac_start(uint32_t channel, uint32_t device, uint32_t address, uint32_t length)
{
    uint32_t c = channel - DMA_FIRST;
    REG(GDMA_OUT_CONF0(c)) = OUT_RST;
    REG(GDMA_OUT_CONF0(c)) = OUT_EOF_MODE;
    REG(GDMA_OUT_PERI_SEL(c)) = device;
    for (uint32_t i = 0; i < DMA_DESCRIPTORS; i++) {
        LOOP_BOUND(DMA_DESCRIPTORS);
        uint32_t n = length < DSCR_MOST ? length : DSCR_MOST;
        length -= n;
        dscr[c][i].buffer = address;
        dscr[c][i].next = length != 0 ? (uint32_t)&dscr[c][i + 1] : 0;
        dscr[c][i].word = DSCR_OWNER | DSCR_SIZE(n) | DSCR_LENGTH(n) | (length == 0 ? DSCR_SUC_EOF : 0);
        address += n;
        if (length == 0) {
            break;
        }
    }
    REG(GDMA_OUT_INT_CLR(c)) = OUT_INT_ALL;
    started[c] = true;
    /* The descriptors are written before the GDMA may read them. */
    __asm__ volatile("fence w, o" ::: "memory");
#if GDMA_V2
    REG(GDMA_OUT_LINK_ADDR(c)) = (uint32_t)&dscr[c][0];
    REG(GDMA_OUT_LINK(c)) = OUTLINK_START;
#else
    REG(GDMA_OUT_LINK(c)) = ((uint32_t)&dscr[c][0] & OUTLINK_ADDR) | OUTLINK_START;
#endif
}

void dmac_halt(uint32_t channel)
{
    uint32_t c = channel - DMA_FIRST;
    REG(GDMA_OUT_LINK(c)) = OUTLINK_STOP;
    REG(GDMA_OUT_CONF0(c)) = OUT_RST;
    REG(GDMA_OUT_CONF0(c)) = OUT_EOF_MODE;
    REG(GDMA_OUT_PERI_SEL(c)) = PERI_NONE;
    REG(GDMA_OUT_INT_CLR(c)) = OUT_INT_ALL;
    started[c] = false;
}

/*
 * Running from a start until the last descriptor is done, an error stops it, or a halt,
 * and whenever the descriptors' state machine is not idle, as it is once halted, which the self-check so reads back.
 */
bool dmac_running(uint32_t channel, uint32_t *device, uint32_t *address, uint32_t *length)
{
    uint32_t c = channel - DMA_FIRST;
    *device = REG(GDMA_OUT_PERI_SEL(c)) & PERI_NONE;
    *address = dscr[c][0].buffer;
    *length = 0;
    for (uint32_t i = 0; i < DMA_DESCRIPTORS; i++) {
        LOOP_BOUND(DMA_DESCRIPTORS);
        *length += DSCR_LENGTH_OF(dscr[c][i].word);
        if (dscr[c][i].next == 0) {
            break;
        }
    }
    return (started[c] && (REG(GDMA_OUT_INT_RAW(c)) & (OUT_TOTAL_EOF | OUT_DSCR_ERR | OUT_RESP_ERR)) == 0) ||
           (REG(GDMA_OUT_LINK(c)) & OUTLINK_PARK) == 0;
}

/*
 * The GDMA comes out of reset, as a reset of the chip keeps PCR's,
 * with every transmit channel halted and no interrupt of its enabled, and on the C5 its window the SRAM.
 */
void dmac_init(void)
{
    REG(GDMA_PCR_CONF) = PCR_GDMA_CLK_EN | PCR_GDMA_RST_EN;
    REG(GDMA_PCR_CONF) = PCR_GDMA_CLK_EN;
    REG(GDMA_MISC_CONF) = MISC_CLK_EN;
#if GDMA_V2
    REG(GDMA_INTR_MEM_START) = RAM_BASE;
    REG(GDMA_INTR_MEM_END) = RAM_BASE + RAM_SIZE;
#endif
    for (uint32_t c = 0; c < DMA_CHANNELS; c++) {
        LOOP_BOUND(DMA_CHANNELS);
        REG(GDMA_OUT_INT_ENA(c)) = 0;
        dmac_halt(DMA_FIRST + c);
    }
}
