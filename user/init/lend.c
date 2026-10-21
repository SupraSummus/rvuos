/* Lent time through a chain of lenders; see DESIGN.md, "Scheduling". */

#include <stdint.h>

#include "init.h"
#include "rvuos.h"

/*
 * CHAIN threads in a row, in a table of their own mounted beside the root task's:
 * each but the last waits with its time lent to the next, and the last counts as it spins.
 */
#define CHAIN       16u
#define CHAIN_TABLE 1u     /* the table slot it is mounted in */
#define CHAIN_NTFN  0u     /* what the lenders wait on, which nobody signals */
#define LINK(i)     (1u + (i))
#define LINK_STACK  0x80u
#define CHAIN_US    20000u
static volatile uint32_t chained;

static void link_main(uint32_t i)
{
    if (i == CHAIN - 1) {
        for (;;) {
            chained++;
        }
    }
    uint32_t bits;
    rv_lend(SLOT_IN(CHAIN_TABLE, CHAIN_NTFN), SLOT_IN(CHAIN_TABLE, LINK(i + 1)), &bits);
    puts("link: woken\n");
    rv_halt(BOOT_CAP_HALT, 1);
}

/*
 * Lent time goes on through a chain of lenders, a first lender with no time for its borrower lending what it borrows;
 * see DESIGN.md, "Scheduling".
 * The links start on spare time, so each can make its lend, and then lose it;
 * they start from the end, so that the next is there whenever spare time runs one:
 * the head earns an eighth of the processor without spare time and the rest earn nothing,
 * so the end of the chain runs only on the head's time, fifteen lenders up, an eighth of each span,
 * each time the release finds that time come again through the chain, and stops once the head earns none.
 * They live in the child's pool, which goes with them once the child is done, and their stacks below the spinners' and the dodger's.
 */
void lend_demo(uint32_t data_base, uint32_t data_size)
{
    expect("allocate the chain's table",
           rv_pool_alloc(SLOT_POOL, CAP_CAPTABLE, SLOT_ROAMER, LINK(CHAIN)));
    expect("mount it", rv_process_mount(BOOT_CAP_PROCESS, CHAIN_TABLE, SLOT_ROAMER));
    expect("let its slot go", rv_invoke(OP_CAP_DELETE, BOOT_CAP_CAPTABLE, SLOT_ROAMER, 0, 0));
    expect("allocate what the lenders wait on",
           rv_pool_alloc(SLOT_POOL, CAP_NOTIFICATION, SLOT_IN(CHAIN_TABLE, CHAIN_NTFN), 0));
    uint32_t stacks = data_base + data_size / 4 - (SPINNERS + 1) * SPINNER_STACK;
    for (uint32_t i = CHAIN; i-- > 0;) {
        uint32_t link = SLOT_IN(CHAIN_TABLE, LINK(i));
        expect("allocate a link", rv_thread_alloc(SLOT_POOL, link, BOOT_CAP_PROCESS, 1));
        expect("configure it", rv_invoke(OP_THREAD_CONFIGURE, link, (uint32_t)&link_main, stacks - i * LINK_STACK, i));
        expect("give it spare time", rv_time_bind(SLOT_HALF_TIME, link, 0, 0, 0));
        expect("start it", rv_invoke(OP_THREAD_RESUME, link, 0, 0, 0));
    }
    expect("let each lend to the next", sleep_us(SLEEP_US));
    expect("derive the half without spare time",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    for (uint32_t i = 1; i < CHAIN; i++) {
        expect("take a link's spare time",
               rv_time_bind(SLOT_CAPPED_TIME, SLOT_IN(CHAIN_TABLE, LINK(i)), 0, 0, 0));
    }
    expect("give the head an eighth without spare time",
           rv_time_bind(SLOT_CAPPED_TIME, SLOT_IN(CHAIN_TABLE, LINK(0)), 0, TIME_UNITS / 8, 0));
    uint32_t before = chained;
    expect("sleep while the end of the chain runs", sleep_us(CHAIN_US));
    uint32_t mid = chained;
    expect("sleep on", sleep_us(CHAIN_US));
    expect("the end of the chain ran on its head's time, fifteen lenders up, again each time the release gave it time",
           mid != before && chained != mid ? KERR_OK : KERR_INVALID_ARG);
    expect("take the head's units", rv_time_bind(SLOT_CAPPED_TIME, SLOT_IN(CHAIN_TABLE, LINK(0)), 0, 0, 0));
    before = chained;
    expect("sleep while nobody pays for it", sleep_us(CHAIN_US));
    expect("and on nothing else", chained == before ? KERR_OK : KERR_INVALID_ARG);
    expect("take the chain's units back", rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    /* A pool that holds a table the root task's process mounts is not one it may destroy. */
    expect("unmount the chain's table", rv_process_unmount(BOOT_CAP_PROCESS, CHAIN_TABLE));
    puts("root: lend chain ok\n");
}
