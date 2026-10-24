#ifndef RVUOS_DMA_H
#define RVUOS_DMA_H

/*
 * DMA: bytes moved between a frame of memory and a device by a channel of the board's DMA controller,
 * which the kernel starts on a range it checked and stops before it lets go of either end; see DESIGN.md, "DMA".
 *
 * A board with a controller says in board.h which of its channels the kernel lends, DMA_CHANNELS from DMA_FIRST,
 * the grain of a transfer, DMA_GRAIN, and its longest, DMA_MOST, and gives the dmac_ functions below;
 * with DMA_FAKE dma.c gives a controller of its own, which moves nothing,
 * as QEMU's boards and the host have, so that the operations run there as on silicon.
 * A board with neither lends no channel, and nor does a kernel built with DMA_OFF, make DMA=0, on any board:
 * DMA_LENT then leaves the code for DMA out.
 */

#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"
#include "paddr.h"

#ifdef DMA_OFF
#undef DMA_FIRST
#undef DMA_CHANNELS
#undef DMA_GRAIN
#undef DMA_MOST
#undef DMA_FAKE
#endif
#ifndef DMA_CHANNELS
#define DMA_FIRST    0u
#define DMA_CHANNELS 0u
#define DMA_GRAIN    4u
#define DMA_MOST     0u
#endif
#ifndef DMA_FAKE
#define DMA_FAKE 0
#endif
/*
 * Whether the kernel lends any channel: where it lends none, no DmaLine or Dma ever exists,
 * so the code that acts on one is dead, and the compiler leaves it out on this constant.
 */
#define DMA_LENT (DMA_CHANNELS != 0)
_Static_assert(DMA_FIRST + DMA_CHANNELS <= UINT16_MAX, "a DmaLine capability names its range in two bytes each");
_Static_assert(DMA_GRAIN != 0 && (DMA_GRAIN & (DMA_GRAIN - 1)) == 0 && DMA_MOST % DMA_GRAIN == 0,
               "a grain is a power of two, and the longest transfer a whole number of them");

struct cap;
struct dma;

/*
 * The board's controller.
 * dmac_memory: which end of a channel lies in memory, DMA_SOURCE or DMA_SINK; the other is a device.
 * dmac_end: the word the controller takes for an end of a channel, named name within the frame [base, base + size),
 * false if the frame holds no such end: memory the controller may reach, named 0, or a device whole, by its request.
 * dmac_start: the channel moves length bytes from address on, in memory, to or from the device the word names.
 * dmac_halt: the channel moves nothing from now on, and reaches no memory once it returns.
 * dmac_running: whether the channel may still reach memory, and, if so, the device's word and the range,
 * as the controller and what it reads say, for the self-check to read back.
 * dmac_init: every channel the kernel lends halted, at boot.
 */
uint32_t dmac_memory(uint32_t channel);
bool dmac_end(uint32_t channel, uint32_t end, uint32_t base, uint32_t size, uint32_t name, uint32_t *word);
void dmac_start(uint32_t channel, uint32_t device, uint32_t address, uint32_t length);
void dmac_halt(uint32_t channel);
bool dmac_running(uint32_t channel, uint32_t *device, uint32_t *address, uint32_t *length);
void dmac_init(void);

/* The Dma bound to each channel the kernel lends, from DMA_FIRST, 0 for none; at least one entry. */
extern paddr_t channel_dma[];

/* Whether the kernel lends the channel. */
static inline bool dma_channel(uint32_t channel)
{
    return channel - DMA_FIRST < DMA_CHANNELS;
}

/* The Dma bound to a channel the kernel lends, or NULL. */
struct dma *dma_binding(uint32_t channel);

/* Every channel free, and halted, at boot. */
void dma_init(void);

/* The word the controller takes for a Dma's end, which is connected. */
uint32_t dma_word(const struct dma *d, uint32_t end);

/* Whether a transfer of the Dma's may still reach its memory. */
bool dma_busy(const struct dma *d);

/* Clear a Dma's end the tree has already let go of: the channel is halted first. */
void dma_drop(struct cap *end);

/* A Dma its pool's destroy takes: halted, and its channel free again. */
void dma_forget(struct dma *d);

#endif
