/*
 * DMA channels, and the controller that stands in where a board has none to lend; see dma.h
 * and DESIGN.md, "DMA".
 */

#include "dma.h"

#include "object.h"

paddr_t channel_dma[DMA_CHANNELS + (DMA_CHANNELS == 0)];

struct dma *dma_binding(uint32_t channel)
{
    if (!dma_channel(channel)) {
        return NULL;
    }
    paddr_t d = channel_dma[channel - DMA_FIRST];
    return d != 0 ? p2v(d) : NULL;
}

void dma_init(void)
{
#if DMA_CHANNELS > 0
    for (uint32_t i = 0; i < DMA_CHANNELS; i++) {
        LOOP_BOUND(DMA_CHANNELS);
        channel_dma[i] = 0;
    }
#endif
    dmac_init();
}

/* The word the controller takes for one of a Dma's ends, which OP_DMA_END found the frame to hold. */
uint32_t dma_word(const struct dma *d, uint32_t end)
{
    uint32_t word = 0;
    dmac_end(d->channel, end, block_base(&d->ends[end]), block_size(&d->ends[end]), d->names[end], &word);
    return word;
}

bool dma_busy(const struct dma *d)
{
    uint32_t device, address, length;
    return dmac_running(d->channel, &device, &address, &length);
}

/*
 * Halted before the end goes, here and in dma_forget, so the controller never reaches memory a Dma no longer holds.
 * A halt of a channel that runs nothing costs what one that runs costs, so neither asks first.
 */
void dma_drop(struct cap *end)
{
    struct dma *d = p2v(cap_holder(end));
    dmac_halt(d->channel);
    d->names[end - d->ends] = 0;
    *end = (struct cap){ 0 };
}

void dma_forget(struct dma *d)
{
    dmac_halt(d->channel);
    channel_dma[d->channel - DMA_FIRST] = 0;
}

#if DMA_FAKE
/*
 * A controller of DMA_CHANNELS channels that moves nothing:
 * even channels read memory and odd ones write it, any frame being memory and any word of a frame a device,
 * and a transfer runs from its start until a halt, so that a start meets a channel still running as on a board,
 * and the self-check reads back what it was told as it reads a board's controller.
 */
static struct {
    uint32_t device, address, length;
    bool on;
} fake[DMA_CHANNELS];

uint32_t dmac_memory(uint32_t channel)
{
    return channel % 2 == 0 ? DMA_SOURCE : DMA_SINK;
}

bool dmac_end(uint32_t channel, uint32_t end, uint32_t base, uint32_t size, uint32_t name, uint32_t *word)
{
    if (end == dmac_memory(channel)) {
        *word = base;
        return name == 0;
    }
    if (name % 4 != 0 || name >= size) {
        return false;
    }
    *word = base + name;
    return true;
}

void dmac_start(uint32_t channel, uint32_t device, uint32_t address, uint32_t length)
{
    fake[channel - DMA_FIRST].device = device;
    fake[channel - DMA_FIRST].address = address;
    fake[channel - DMA_FIRST].length = length;
    fake[channel - DMA_FIRST].on = true;
}

void dmac_halt(uint32_t channel)
{
    fake[channel - DMA_FIRST].on = false;
}

bool dmac_running(uint32_t channel, uint32_t *device, uint32_t *address, uint32_t *length)
{
    *device = fake[channel - DMA_FIRST].device;
    *address = fake[channel - DMA_FIRST].address;
    *length = fake[channel - DMA_FIRST].length;
    return fake[channel - DMA_FIRST].on;
}

void dmac_init(void)
{
    for (uint32_t i = 0; i < DMA_CHANNELS; i++) {
        LOOP_BOUND(DMA_CHANNELS);
        fake[i].device = fake[i].address = fake[i].length = 0;
        fake[i].on = false;
    }
}
#elif DMA_CHANNELS == 0
/* No controller: no channel is lent, so nothing below is reached but the boot's. */
uint32_t dmac_memory(uint32_t channel)
{
    (void)channel;
    return DMA_SOURCE;
}

bool dmac_end(uint32_t channel, uint32_t end, uint32_t base, uint32_t size, uint32_t name, uint32_t *word)
{
    (void)channel;
    (void)end;
    (void)base;
    (void)size;
    (void)name;
    (void)word;
    return false;
}

void dmac_start(uint32_t channel, uint32_t device, uint32_t address, uint32_t length)
{
    (void)channel;
    (void)device;
    (void)address;
    (void)length;
}

void dmac_halt(uint32_t channel)
{
    (void)channel;
}

bool dmac_running(uint32_t channel, uint32_t *device, uint32_t *address, uint32_t *length)
{
    (void)channel;
    *device = *address = *length = 0;
    return false;
}

void dmac_init(void)
{
}
#endif
