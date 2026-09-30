/*
 * Escape attempt: a misaligned store from a region the root task may write into one it may only read.
 *
 * The free RAM's halves become two frames that touch,
 * the lower installed read and write, the upper read only,
 * and the word at the lower's last two bytes is stored to.
 * The ESP32-C6 would write the upper's two bytes, see DESIGN.md, "Boards",
 * so there the kernel refuses the pair; the program tries both orders and requires the same answer.
 * Either way the store must fault, mcause=7, naming its own address in mepc,
 * or raise a misaligned exception, mcause=6, on a core that does not split it.
 * Were the store allowed the program would say so, which fails the run; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "rvuos.h"

/* The store below, labelled in the statement that makes it, so the label cannot drift from it. */
extern const char store_at[];

enum {
    SLOT_LOWER_MEMORY = BOOT_CAP_COUNT,
    SLOT_UPPER_MEMORY,
    SLOT_LOWER,
    SLOT_UPPER,
};

/* Region slots: the root task boots with code in 0 and data in 1. */
#define LOWER_SLOT 2
#define UPPER_SLOT 3

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

static void expect(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        puts("escape: ");
        puts(what);
        puts(": FAILED\n");
        rv_halt(BOOT_CAP_DEBUG, 3);
    }
}

static uint32_t map_lower(void)
{
    return rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, LOWER_SLOT, SLOT_LOWER, RIGHT_R | RIGHT_W);
}

static uint32_t map_upper(void)
{
    return rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, UPPER_SLOT, SLOT_UPPER, RIGHT_R);
}

int main(void);
int main(void)
{
    uint32_t lower, upper;
    expect("halve the free ram", rv_split(BOOT_CAP_FREE_RAM, SLOT_LOWER_MEMORY, SLOT_UPPER_MEMORY));
    expect("make the lower half a frame", rv_retype(SLOT_LOWER_MEMORY, CAP_FRAME, SLOT_LOWER, &lower));
    expect("make the upper half a frame", rv_retype(SLOT_UPPER_MEMORY, CAP_FRAME, SLOT_UPPER, &upper));

    /* The upper first, then the lower below it; then the other way round, which stays for the store. */
    expect("map the upper half", map_upper());
    uint32_t below = map_lower();
    rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, UPPER_SLOT, 0, 0);
    rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, LOWER_SLOT, 0, 0);
    expect("map the lower half", map_lower());
    uint32_t above = map_upper();
    expect("answer both orders alike",
           below == above && (below == KERR_OK || below == KERR_OVERLAP) ? KERR_OK : KERR_INVALID_ARG);
    puts(below == KERR_OK ? "escape: both halves are mapped\n" : "escape: the kernel refused the pair\n");

    uint32_t addr = upper - 2;
    puts("escape: storing across the end of the lower half to ");
    rv_put_hex(BOOT_CAP_DEBUG, addr);
    puts(" at ");
    rv_put_hex(BOOT_CAP_DEBUG, (uint32_t)(uintptr_t)store_at);
    puts(", expecting a fault\n");

    /* Not followed at once by a store, which would have the ESP32-C6 check the second word for writing. */
    __asm__ volatile(".globl store_at\nstore_at:\n\tsw %0, 0(%1)\n\tnop" : : "r"(0x5a5a5a5au), "r"(addr) : "memory");

    /* Not reached when the part in the upper half faults. */
    puts("escape: breached, stored past the lower half\n");
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
