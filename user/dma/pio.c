/*
 * RP2350's loop for the DMA test: PIO0's state machine 0 runs a program of three instructions,
 * "pull block; mov isr, osr; push block", which hands each word of its transmit FIFO to its receive FIFO.
 * It stalls on a full receive FIFO, so the transmit FIFO fills and its DREQ falls:
 * the channel reads no further ahead of the CPU than the two FIFOs and the shift registers hold,
 * four words each and one each, LOOP_DEPTH with room to spare.
 * Register offsets are the RP2350 datasheet's, "PIO".
 */

#include "loop.h"
#include "rvuos.h"

#if defined(PIO0_BASE)

#define REG(off) (*(volatile uint32_t *)(PIO0_BASE + (off)))

#define PIO_CTRL          0x000u
#define PIO_FSTAT         0x004u
#define PIO_RXF0          0x020u
#define PIO_INSTR_MEM(i)  (0x048u + 4u * (i))
#define PIO_SM0_CLKDIV    0x0c8u
#define PIO_SM0_EXECCTRL  0x0ccu
#define PIO_SM0_SHIFTCTRL 0x0d0u
#define PIO_SM0_INSTR     0x0d8u
#define PIO_SM0_PINCTRL   0x0dcu

#define CTRL_SM0_ENABLE   (1u << 0)
#define CTRL_SM0_RESTART  (1u << 4)
#define FSTAT_RXEMPTY0    (1u << 8)
#define SHIFTCTRL_FJOIN_RX (1u << 31)
#define SHIFTCTRL_DEFAULT 0x000c0000u /* both shift right, thresholds 32, no autopush or autopull */
#define EXECCTRL_WRAP(top, bottom) ((uint32_t)(top) << 12 | (uint32_t)(bottom) << 7)

#define OP_PULL_BLOCK     0x80a0u
#define OP_MOV_ISR_OSR    0xa0c7u
#define OP_PUSH_BLOCK     0x8020u
#define OP_JMP_0          0x0000u

/* How many times loop_take looks before it gives up: far longer than the state machine takes for a word. */
#define LOOP_LOOKS 200000u

void loop_open(struct loop *loop, uint32_t region)
{
    (void)loop;
    if (rv_process_install(BOOT_CAP_PROCESS, region, BOOT_CAP_PIO0, RIGHT_R | RIGHT_W) != KERR_OK) {
        rv_puts(BOOT_CAP_PRINT, "dma: map PIO0: FAILED\n");
        rv_halt(BOOT_CAP_HALT, 3);
    }
    REG(PIO_CTRL) = 0;
    REG(PIO_INSTR_MEM(0)) = OP_PULL_BLOCK;
    REG(PIO_INSTR_MEM(1)) = OP_MOV_ISR_OSR;
    REG(PIO_INSTR_MEM(2)) = OP_PUSH_BLOCK;
    REG(PIO_SM0_CLKDIV) = 1u << 16;
    REG(PIO_SM0_EXECCTRL) = EXECCTRL_WRAP(2, 0);
    REG(PIO_SM0_PINCTRL) = 0;
    /* A join and back empties both FIFOs, of whatever an earlier run left. */
    REG(PIO_SM0_SHIFTCTRL) = SHIFTCTRL_DEFAULT | SHIFTCTRL_FJOIN_RX;
    REG(PIO_SM0_SHIFTCTRL) = SHIFTCTRL_DEFAULT;
    REG(PIO_CTRL) = CTRL_SM0_RESTART;
    REG(PIO_SM0_INSTR) = OP_JMP_0;
    REG(PIO_CTRL) = CTRL_SM0_ENABLE;
}

/* The state machine hands on whatever comes, so nothing waits to be emptied. */
void loop_ready(struct loop *loop)
{
    (void)loop;
}

bool loop_take(struct loop *loop, uint32_t *word)
{
    (void)loop;
    for (uint32_t i = 0; i < LOOP_LOOKS; i++) {
        if ((REG(PIO_FSTAT) & FSTAT_RXEMPTY0) == 0) {
            *word = REG(PIO_RXF0);
            return true;
        }
    }
    return false;
}
#endif
