/*
 * A fixed executor for the CYW43439's Bluetooth core, parked: see tools/cyw43-exec.py and BT.md.
 *
 * The core's processor, parked as tools/cyw43-rehost.py parks it, slides into this code instead of the sniffer,
 * its interrupts masked, and runs nothing of the vendor's.
 * It runs a list of operations the host leaves in the core's RAM, each time the host writes a new number to GO,
 * and writes that number to DONE once the list ends; an experiment is data, and this code stays the same.
 * Time is the Cortex-M3's cycle counter, DWT's, at the 48 MHz the parked core runs at.
 *
 * It writes whatever words it is handed, the carrier's among them,
 * so the host keeps every write of a list inside the band before it hands the list over.
 */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define DEMCR      0xe000edfcu
#define DWT_CTRL   0xe0001000u
#define DWT_CYCCNT 0xe0001004u

/*
 * The host's words, in the core's RAM, clear of the patch's records there (0x21e000 on) and of the rehost's mailbox;
 * the code itself lies beside the vendor patch, where the core fetches code, as RAM at 0x2xxxxx takes none.
 */
#define BASE    0x21c000u
#define GO      (BASE + 0x0)   /* the host writes a new number to have the list run */
#define DONE    (BASE + 0x4)   /* the number of the run that ended last */
#define COUNT   (BASE + 0x8)   /* how many results the run left */
#define ALIVE   (BASE + 0xc)   /* turns of the loop between runs, for the host to see it run */
#define MARKED  (BASE + 0x10)  /* the cycle count of the last MARK */
#define GAVE_UP (BASE + 0x14)  /* UNTILs of the run that waited UNTIL_MOST and went on */
#define LIST    (BASE + 0x100) /* the operations, three words each: kind, address, value */
#define OUT     (BASE + 0xd00) /* the results, two words each: the cycle count and the word read */
#define OPS     256u
#define RESULTS 512u
#define STEPS   10000000u      /* the most operations a run takes, loops included */
#define UNTIL_MOST 48000000u   /* the cycles an UNTIL waits at most, a second */
/* So a run ends: STEPS operations, each a WAIT of at most 2^32 cycles, an UNTIL of at most UNTIL_MOST, or a word. */

enum { END, WRITE, WAIT, READ, LOOP, UNTIL, MARK, WAIT_MARK };

static void run(void)
{
    const volatile uint32_t *list = (const volatile uint32_t *)LIST;
    uint32_t n = 0, at = 0, turns = 0, gave_up = 0; /* one count of turns: the host nests no loop in another */
    for (uint32_t step = 0; step < STEPS && at < OPS; step++) {
        uint32_t kind = list[3u * at], addr = list[3u * at + 1u], value = list[3u * at + 2u];
        at++;
        if (kind == END) {
            break;
        } else if (kind == WRITE) {
            REG(addr) = value;
        } else if (kind == WAIT || kind == WAIT_MARK) { /* value cycles from now, or from the last MARK */
            uint32_t from = kind == WAIT ? REG(DWT_CYCCNT) : REG(MARKED);
            while (REG(DWT_CYCCNT) - from < value) {
            }
        } else if (kind == READ) {
            if (n < RESULTS) {
                REG(OUT + 8u * n) = REG(DWT_CYCCNT);
                REG(OUT + 8u * n + 4u) = REG(addr);
                n++;
            }
        } else if (kind == LOOP) { /* back to operation addr, until it has run value times in all */
            if (++turns < value) {
                at = addr;
            } else {
                turns = 0;
            }
        } else if (kind == UNTIL) { /* until a bit of the mask is set in the word at addr, or UNTIL_MOST went by */
            uint32_t from = REG(DWT_CYCCNT);
            while ((REG(addr) & value) == 0u) {
                if (REG(DWT_CYCCNT) - from >= UNTIL_MOST) {
                    gave_up++;
                    break;
                }
            }
        } else if (kind == MARK) {
            REG(MARKED) = REG(DWT_CYCCNT);
        }
    }
    REG(COUNT) = n;
    REG(GAVE_UP) = gave_up;
}

__attribute__((noreturn, used)) void exec_main(void)
{
    REG(DEMCR) |= 1u << 24; /* TRCENA, for DWT */
    REG(DWT_CYCCNT) = 0;
    REG(DWT_CTRL) |= 1u;    /* CYCCNTENA */
    uint32_t seen = REG(GO);
    REG(DONE) = seen;
    for (;;) {
        REG(ALIVE) = REG(ALIVE) + 1u;
        uint32_t go = REG(GO);
        if (go != seen) {
            seen = go;
            run();
            REG(DONE) = go;
        }
    }
}

/* Where the slide lands, by the host's branch: a stack of its own, below the host's words, and on. */
__attribute__((naked, noreturn, section(".start"))) void exec_start(void)
{
    __asm volatile("ldr sp, =0x21bff0\n"
                   "b exec_main\n");
}
