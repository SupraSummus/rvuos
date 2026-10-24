/*
 * RP2350's DMA controller as dma.h's controller, on either kind of core.
 *
 * The DMA has sixteen channels, each a pair of addresses and a count it moves words between,
 * paced by the data request, the DREQ, of the device TREQ_SEL names;
 * a channel reads and writes wherever its addresses point, every device and all of SRAM, the kernel's too,
 * so the kernel holds the controller whole and lends its channels, DMA_FIRST on,
 * each reading memory into a device's FIFO: the read address walks the range a start checked,
 * and the write address stays on the FIFO's one register.
 * Nothing but the kernel reaches the DMA's registers: board.h lists no frame over them.
 * The kernel sets every field a channel runs by, the chain among them, and writes the trigger last,
 * so a lent channel triggers no other and no other triggers it.
 *
 * A halt is the datasheet's abort, RP2350-E5's workaround first:
 * EN cleared, so that the channel ignores a trigger, then CHAN_ABORT,
 * which holds its bit until the transfers in flight have left the channel,
 * so once it reads clear the channel reaches nothing more.
 *
 * Register addresses are those of the RP2350 datasheet, "DMA" and "PIO".
 */

#include "dma.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

#define DMA_BASE              0x50000000u
#define DMA_READ_ADDR(c)      (DMA_BASE + 0x00u + 0x40u * (c))
#define DMA_WRITE_ADDR(c)     (DMA_BASE + 0x04u + 0x40u * (c))
#define DMA_TRANS_COUNT(c)    (DMA_BASE + 0x08u + 0x40u * (c))
#define DMA_CTRL_TRIG(c)      (DMA_BASE + 0x0cu + 0x40u * (c))
#define DMA_AL1_CTRL(c)       (DMA_BASE + 0x10u + 0x40u * (c)) /* CTRL without the trigger */
#define DMA_CHAN_ABORT        (DMA_BASE + 0x464u)
#define DMA_DBG_TCR(c)        (DMA_BASE + 0x804u + 0x40u * (c)) /* the count a trigger loads */

#define CTRL_EN               (1u << 0)
#define CTRL_SIZE             (3u << 2)
#define CTRL_SIZE_WORD        (2u << 2)
#define CTRL_INCR_READ        (1u << 4)
#define CTRL_INCR_READ_REV    (1u << 5)
#define CTRL_INCR_WRITE       (1u << 6)
#define CTRL_INCR_WRITE_REV   (1u << 7)
#define CTRL_RING_SIZE        (0xfu << 8)
#define CTRL_CHAIN_TO(c)      ((uint32_t)(c) << 13)
#define CTRL_TREQ_SEL(t)      ((uint32_t)(t) << 17)
#define CTRL_TREQ             (0x3fu << 17)
#define CTRL_IRQ_QUIET        (1u << 23)
#define CTRL_BUSY             (1u << 26)
#define CTRL_ERRORS           (3u << 29) /* READ_ERROR and WRITE_ERROR, each cleared by a one */
/* What a channel runs by: words, the read address walking up and the write address on one register. */
#define CTRL_WALK             (CTRL_SIZE | CTRL_INCR_READ | CTRL_INCR_READ_REV | CTRL_INCR_WRITE | CTRL_INCR_WRITE_REV | \
                               CTRL_RING_SIZE)
#define TRANS_COUNT_MASK      0x0fffffffu /* the count; MODE above it is 0, a count that runs down once */

_Static_assert(DMA_FIRST + DMA_CHANNELS <= 16u, "the DMA has sixteen channels");
_Static_assert(DMA_MOST / 4u <= TRANS_COUNT_MASK, "the longest transfer's words fit the count");

/* Where each channel's transfer started, which the self-check holds the channel's addresses to. */
static uint32_t started[DMA_CHANNELS];

/*
 * A device a channel may feed is a transmit FIFO of a PIO block's, named by its DREQ, which the device's word is:
 * PIO n's state machine m is DREQ 8n + m, its FIFO TXFm at 0x10 + 4m in the block, for a frame that holds the block whole.
 */
#define PIO_BASE(n)   (0x50200000u + 0x100000u * (n))
#define PIO_BLOCK     0x4000u
#define PIO_DREQS     24u
#define PIO_FIFO(dreq) (PIO_BASE((dreq) / 8u) + 0x10u + 4u * ((dreq) % 8u))

uint32_t dmac_memory(uint32_t channel)
{
    (void)channel;
    return DMA_SOURCE;
}

bool dmac_end(uint32_t channel, uint32_t end, uint32_t base, uint32_t size, uint32_t name, uint32_t *word)
{
    (void)channel;
    *word = end == DMA_SOURCE ? base : name;
    if (end == DMA_SOURCE) {
        return name == 0 && base >= RAM_BASE && base - RAM_BASE < RAM_SIZE && size <= RAM_BASE + RAM_SIZE - base;
    }
    uint32_t block = PIO_BASE(name / 8u);
    return name < PIO_DREQS && name % 8u < 4u && base <= block && block - base <= size &&
           PIO_BLOCK <= size - (block - base);
}

void dmac_start(uint32_t channel, uint32_t dreq, uint32_t address, uint32_t length)
{
    started[channel - DMA_FIRST] = address;
    REG(DMA_READ_ADDR(channel)) = address;
    REG(DMA_WRITE_ADDR(channel)) = PIO_FIFO(dreq);
    REG(DMA_TRANS_COUNT(channel)) = length / 4u;
    /* What the program wrote of the range is out of the core before the channel may read it. */
#ifdef __riscv
    __asm__ volatile("fence w, o" ::: "memory");
#else
    __asm__ volatile("dmb" ::: "memory");
#endif
    REG(DMA_CTRL_TRIG(channel)) = CTRL_EN | CTRL_SIZE_WORD | CTRL_INCR_READ | CTRL_CHAIN_TO(channel) |
                                  CTRL_TREQ_SEL(dreq) | CTRL_IRQ_QUIET | CTRL_ERRORS;
}

void dmac_halt(uint32_t channel)
{
    REG(DMA_AL1_CTRL(channel)) = CTRL_CHAIN_TO(channel);
    REG(DMA_CHAN_ABORT) = 1u << channel;
    while (REG(DMA_CHAN_ABORT) & (1u << channel)) {
        LOOP_WAIT("the transfers in flight to leave the channel, a few cycles");
    }
}

/*
 * Running while BUSY, from a start until the last write is done or a halt.
 * The device is the DREQ that paces the channel, if it writes that DREQ's FIFO alone, else none;
 * the range is the one the start began, as long as the count it loaded,
 * or none if the channel reads anywhere else.
 */
bool dmac_running(uint32_t channel, uint32_t *device, uint32_t *address, uint32_t *length)
{
    uint32_t ctrl = REG(DMA_CTRL_TRIG(channel));
    uint32_t dreq = (ctrl & CTRL_TREQ) >> 17;
    bool one = (ctrl & CTRL_WALK) == (CTRL_SIZE_WORD | CTRL_INCR_READ) && REG(DMA_WRITE_ADDR(channel)) == PIO_FIFO(dreq);
    *device = one ? dreq : ~0u;
    *address = started[channel - DMA_FIRST];
    *length = (REG(DMA_DBG_TCR(channel)) & TRANS_COUNT_MASK) * 4u;
    if (REG(DMA_READ_ADDR(channel)) - *address > *length) {
        *length = 0;
    }
    return (ctrl & CTRL_BUSY) != 0;
}

/* Every lent channel halted and chained to itself; board_init takes the DMA out of reset first. */
void dmac_init(void)
{
    for (uint32_t c = DMA_FIRST; c < DMA_FIRST + DMA_CHANNELS; c++) {
        LOOP_BOUND(DMA_CHANNELS);
        started[c - DMA_FIRST] = 0;
        dmac_halt(c);
    }
}
