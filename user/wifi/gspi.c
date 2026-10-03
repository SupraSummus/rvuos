/*
 * gSPI through PIO0; see gspi.h.
 *
 * The program is the pico-sdk's slower one, as embassy's cyw43-pio carries it,
 * for a PIO clock below 75 MHz, here 50 MHz, so the bus runs at 25 MHz:
 *
 *     .side_set 1                ; the clock
 *     lp:  out pins, 1  side 0   ; x + 1 bits out, most significant first
 *          jmp x-- lp   side 1
 *          set pindirs, 0 side 0 ; the chip drives the line from here
 *          nop          side 0
 *     lp2: in pins, 1   side 1   ; y + 1 bits in, sampled as the clock rises
 *          jmp y-- lp2  side 0
 *
 * It wraps to its start, where the autopull finds the FIFO empty and the clock stays low.
 * A transfer sets x and y while the machine is stopped, through the FIFO and executed instructions,
 * drives the line again and jumps to the start.
 * Embassy's program also waits for the chip's interrupt after the transfer; this one does not, since the driver polls.
 */

#include "gspi.h"

#include "lib.h"

#define PIO_CTRL              0x000u
#define PIO_FSTAT             0x004u
#define PIO_TXF0              0x010u
#define PIO_RXF0              0x020u
#define PIO_INPUT_SYNC_BYPASS 0x038u
#define PIO_INSTR_MEM0        0x048u
#define PIO_SM0_CLKDIV        0x0c8u
#define PIO_SM0_EXECCTRL      0x0ccu
#define PIO_SM0_SHIFTCTRL     0x0d0u
#define PIO_SM0_INSTR         0x0d8u
#define PIO_SM0_PINCTRL       0x0dcu

#define CTRL_SM0_ENABLE  (1u << 0)
#define CTRL_SM0_RESTART (1u << 4)
#define CTRL_SM0_CLKDIV_RESTART (1u << 8)
#define FSTAT_RXEMPTY0   (1u << 8)
#define FSTAT_TXFULL0    (1u << 16)

#define SHIFTCTRL_AUTOPUSH (1u << 16)
#define SHIFTCTRL_AUTOPULL (1u << 17)

#define PINCTRL(sideset_count, set_count, out_count, in_base, sideset_base, set_base, out_base)                  \
    (((sideset_count) << 29) | ((set_count) << 26) | ((out_count) << 20) | ((in_base) << 15) |                 \
     ((sideset_base) << 10) | ((set_base) << 5) | (out_base))

/* The instructions executed by hand: pull block, out x 32, out y 32, set pindirs, set pins, jmp 0. */
#define I_PULL_BLOCK   0x80a0u
#define I_OUT_X_32     0x6020u
#define I_OUT_Y_32     0x6040u
#define I_SET_PINDIRS1 0xe081u
#define I_SET_PINS0    0xe000u
#define I_JMP_0        0x0000u

static const uint16_t program[] = {
    0x6001, /* out pins, 1     side 0 */
    0x1040, /* jmp x--, 0      side 1 */
    0xe080, /* set pindirs, 0  side 0 */
    0xa042, /* nop             side 0 */
    0x5001, /* in pins, 1      side 1 */
    0x0084, /* jmp y--, 4      side 0 */
};
#define PROGRAM_WRAP_TOP 5u

/* The PIO clock: clk_sys's 150 MHz divided by three. */
#define CLKDIV_INT 3u

/* A transfer that has not moved a word in this many polls has failed; a word takes about 1.3 us. */
#define SPINS_PER_WORD 100000u

/* IO_BANK0's GPIOn_CTRL within the frame over GPIO16 to GPIO31. */
#define PIN_CTRL(n)    (((n) - 16u) * 8u + 4u)
#define FUNC_NULL      0x1fu
#define FUNC_PIO0      6u
#define OUTOVER_LOW    (2u << 12)
#define OUTOVER_HIGH   (3u << 12)
#define OEOVER_ENABLE  (3u << 14)

#define PIO(b, reg) REG32((b)->pio + (reg))

static void exec(struct gspi *b, uint32_t instr)
{
    PIO(b, PIO_SM0_INSTR) = instr;
}

static void drive(struct gspi *b, uint32_t pin, int high)
{
    REG32(b->pins + PIN_CTRL(pin)) = FUNC_NULL | OEOVER_ENABLE | (high ? OUTOVER_HIGH : OUTOVER_LOW);
}

static uint32_t pinctrl_normal(void)
{
    return PINCTRL(1u, 1u, 1u, GSPI_PIN_DATA, GSPI_PIN_CLK, GSPI_PIN_DATA, GSPI_PIN_DATA);
}

void gspi_init(struct gspi *b, uintptr_t pio, uintptr_t pins)
{
    b->pio = pio;
    b->pins = pins;

    drive(b, GSPI_PIN_CS, 1);
    drive(b, GSPI_PIN_ON, 0);

    PIO(b, PIO_CTRL) = 0;
    for (uint32_t i = 0; i < sizeof(program) / sizeof(program[0]); i++) {
        PIO(b, PIO_INSTR_MEM0 + 4u * i) = program[i];
    }
    PIO(b, PIO_SM0_CLKDIV) = CLKDIV_INT << 16;
    PIO(b, PIO_SM0_EXECCTRL) = PROGRAM_WRAP_TOP << 12;
    PIO(b, PIO_SM0_SHIFTCTRL) = SHIFTCTRL_AUTOPULL | SHIFTCTRL_AUTOPUSH;
    PIO(b, PIO_INPUT_SYNC_BYPASS) = 1u << GSPI_PIN_DATA;

    /* The clock an output, low: set reaches only the data line, so point it at the clock for a moment. */
    PIO(b, PIO_SM0_PINCTRL) = PINCTRL(1u, 1u, 1u, GSPI_PIN_DATA, GSPI_PIN_CLK, GSPI_PIN_CLK, GSPI_PIN_DATA);
    exec(b, I_SET_PINDIRS1);
    exec(b, I_SET_PINS0);
    PIO(b, PIO_SM0_PINCTRL) = pinctrl_normal();
    gspi_hold_data_low(b);

    REG32(b->pins + PIN_CTRL(GSPI_PIN_DATA)) = FUNC_PIO0;
    REG32(b->pins + PIN_CTRL(GSPI_PIN_CLK)) = FUNC_PIO0;
    PIO(b, PIO_CTRL) = CTRL_SM0_RESTART | CTRL_SM0_CLKDIV_RESTART;
}

void gspi_hold_data_low(struct gspi *b)
{
    PIO(b, PIO_CTRL) = 0;
    exec(b, I_SET_PINDIRS1);
    exec(b, I_SET_PINS0);
}

void gspi_power(struct gspi *b, int on)
{
    drive(b, GSPI_PIN_ON, on);
}

/*
 * Words out, then words in and the status word the chip answers with after them;
 * all ones for the status if the bus stalled before the end.
 */
static uint32_t transfer(struct gspi *b, const uint32_t *out, uint32_t n_out, uint32_t *in, uint32_t n_in)
{
    drive(b, GSPI_PIN_CS, 0);
    PIO(b, PIO_CTRL) = 0;
    PIO(b, PIO_CTRL) = CTRL_SM0_RESTART;
    while (!(PIO(b, PIO_FSTAT) & FSTAT_RXEMPTY0)) {
        (void)PIO(b, PIO_RXF0);
    }
    PIO(b, PIO_TXF0) = n_out * 32u - 1u;
    exec(b, I_PULL_BLOCK);
    exec(b, I_OUT_X_32);
    PIO(b, PIO_TXF0) = (n_in + 1u) * 32u - 1u;
    exec(b, I_PULL_BLOCK);
    exec(b, I_OUT_Y_32);
    exec(b, I_SET_PINDIRS1);
    exec(b, I_JMP_0);
    PIO(b, PIO_CTRL) = CTRL_SM0_ENABLE;

    uint32_t sent = 0, got = 0, idle = 0, status = 0xffffffffu;
    while (got <= n_in && idle < SPINS_PER_WORD) {
        uint32_t fstat = PIO(b, PIO_FSTAT);
        idle++;
        if (sent < n_out && !(fstat & FSTAT_TXFULL0)) {
            PIO(b, PIO_TXF0) = out[sent++];
            idle = 0;
        }
        if (!(fstat & FSTAT_RXEMPTY0)) {
            uint32_t w = PIO(b, PIO_RXF0);
            if (got < n_in) {
                in[got] = w;
            } else {
                status = w;
            }
            got++;
            idle = 0;
        }
    }
    PIO(b, PIO_CTRL) = 0;
    drive(b, GSPI_PIN_CS, 1);
    return status;
}

uint32_t gspi_write(struct gspi *b, const uint32_t *words, uint32_t count)
{
    return transfer(b, words, count, 0, 0);
}

uint32_t gspi_read(struct gspi *b, uint32_t cmd, uint32_t *words, uint32_t count)
{
    return transfer(b, &cmd, 1, words, count);
}
