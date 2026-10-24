#ifndef RVUOS_DMA_LOOP_H
#define RVUOS_DMA_LOOP_H

/*
 * The loop the DMA test sends through: a device a channel feeds, whose words come back to the CPU in order,
 * so that the test reads every word the channel read of memory, and when.
 * Each board's is its own file, which says why its words come back.
 *
 * LOOP_DEVICE: the frame slot of the device; LOOP_REQUEST: its request, as OP_DMA_END names it;
 * LOOP_DEPTH: the most words the channel may read ahead of the CPU before the device stops asking;
 * LOOP_ENDS: whether the loop gives no word once the channel stops feeding it,
 * or, as SPI2 does, goes on with words of its own;
 * LOOP_GRAIN: the words a transfer is a whole number of, for every word of it to come back as it was read.
 * struct loop: what the loop keeps between calls, which the root task holds, since no other file may keep a global.
 * loop_open: the device ready, its frames mapped from region slot region on.
 * loop_ready: ready for a transfer to start, whatever the last one left behind.
 * loop_take: the next word back, false if none came within a while.
 */

#include <stdbool.h>
#include <stdint.h>

#include "devices.h"

#if defined(PIO0_BASE)
#define LOOP_DEVICE  BOOT_CAP_PIO0
#define LOOP_REQUEST 0u /* DREQ_PIO0_TX0 */
#define LOOP_DEPTH   12u
#define LOOP_ENDS    1
#define LOOP_GRAIN   1u
struct loop {
    uint32_t unused;
};
#elif defined(SPI2_BASE)
#define LOOP_DEVICE  BOOT_CAP_SPI2
#define LOOP_REQUEST DMA_REQUEST_SPI2
#define LOOP_DEPTH   64u
#define LOOP_ENDS    0
#define LOOP_WORDS   16u /* a transfer of SPI2's buffer */
#define LOOP_GRAIN   LOOP_WORDS
struct loop {
    uint32_t words[LOOP_WORDS];
    uint32_t have, next;
};
#else
#error "no loop for the DMA test on this board"
#endif

void loop_open(struct loop *loop, uint32_t region);
void loop_ready(struct loop *loop);
bool loop_take(struct loop *loop, uint32_t *word);

#endif
