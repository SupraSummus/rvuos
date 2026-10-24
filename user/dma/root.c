/*
 * DMA: a root task that holds the board's DMA controller to what the kernel says of it, on silicon,
 * through a loop, a device that hands every word the channel feeds it back to the CPU; see loop.h.
 *
 * Its channel reads a frame of the free RAM into the loop. It checks
 * that a transfer brings back every word of its range, from where its offset says,
 * that the controller tells a start that a transfer still runs and when it ends,
 * and that a cut, a revoke of the source's frame, its end connected anew or a stop, has the channel read no more:
 * the memory is written over once the cut returns, and none of what was written may come back.
 * The same run with no cut, the control, shows the loop would see a channel that reads on.
 * A kernel built with SELFCHECK=1 checks after every call what the controller says it runs, too.
 *
 * It halts with 0 if every check holds, 2 if one does not, and 3 if a call it needs fails.
 * Build and run: make BOARD=<board> dma-test.
 */

#include <stdint.h>

#include "lib/log.h"
#include "lib/say.h"
#include "loop.h"
#include "rvuos.h"

enum {
    SLOT_FREE = BOOT_CAP_COUNT, /* the free RAM as a frame */
    SLOT_BUFFER,                /* the frame the channel reads, carved */
    SLOT_OTHER,                 /* another, right above it, which a cut connects instead */
    SLOT_LENT,                  /* the buffer's frame derived, as a program would lend it, which a cut revokes */
    SLOT_DMA_LINE,              /* the channel, carved */
    SLOT_DMA,                   /* the Dma on it */
};

/* Region slots: the root task boots with code in 0 and data in 1, and the loop maps its own from REGION_LOOP. */
#define REGION_FREE 2
#define REGION_LOOP 3

#define WORDS  4096u /* a transfer's, 16 KiB */
#define BUFFER (WORDS * 4u)
#define BEFORE 64u /* the words the CPU takes before a cut */

enum cut { CUT_NONE, CUT_REVOKE, CUT_END, CUT_STOP };
static const char *const cut_name[] = { "nothing, the control", "a revoke", "an end anew", "a stop" };

static struct out out;
static struct loop loop;
static uint32_t failures;

static void expect(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        say(&out, "dma: %s: FAILED, %u\n", what, status);
        rv_halt(BOOT_CAP_HALT, 3);
    }
}

static void check(const char *what, int holds)
{
    say(&out, "dma: %s: %s\n", holds ? "ok" : "FAIL", what);
    failures += !holds;
}

/* The words a run writes, one pattern for each salt, none alike at an index. */
static uint32_t word(uint32_t i, uint32_t salt)
{
    return (i + 1u) * 2654435761u ^ salt * 0x9e3779b9u;
}

static void fill(volatile uint32_t *memory, uint32_t salt)
{
    for (uint32_t i = 0; i < WORDS; i++) {
        memory[i] = word(i, salt);
    }
}

static uint32_t start(uint32_t offset, uint32_t length)
{
    loop_ready(&loop);
    return rv_invoke(OP_DMA_START, SLOT_DMA, offset, length, 0);
}

static uint32_t busy(void)
{
    uint32_t w[3];
    expect("the Dma's state", rv_cap_info_of(SLOT_DMA, CAP_DMA, w));
    return w[2];
}

/*
 * Whether a transfer from offset brings back exactly the words from there, and, from a loop that ends, nothing after;
 * one from an offset leaves out the buffer's last LOOP_GRAIN words, so that it stays whole grains long.
 */
static int echo(volatile uint32_t *memory, uint32_t offset, uint32_t salt)
{
    uint32_t words = WORDS - (offset != 0 ? LOOP_GRAIN : 0), w, wrong = 0, came = 0;
    fill(memory, salt);
    expect("a start", start(offset, words * 4u));
    for (; came < words && loop_take(&loop, &w); came++) {
        wrong += w != word(offset / 4u + came, salt);
    }
    int more = LOOP_ENDS && loop_take(&loop, &w);
    say(&out, "dma: from %u: %u words of %u, %u wrong%s\n", offset, came, words, wrong, more ? ", more after" : "");
    return came == words && wrong == 0 && !more && !busy();
}

/*
 * Whether a transfer cut after the CPU took BEFORE words, its memory written over then, brings back none of what was written
 * and no more of what was there than the loop held, or, cut by nothing, what was written.
 */
static int cut(volatile uint32_t *memory, enum cut how, uint32_t salt)
{
    uint32_t w, i = 0, before = 0, after = 0, other = 0;
    fill(memory, salt);
    expect("the source, lent", rv_invoke(OP_DMA_END, SLOT_DMA, DMA_SOURCE, SLOT_LENT, 0));
    expect("a start", start(0, BUFFER));
    for (; i < BEFORE; i++) {
        if (!loop_take(&loop, &w) || w != word(i, salt)) {
            expect("the words before the cut", KERR_STATE);
        }
    }
    if (how == CUT_REVOKE) {
        expect("the revoke", rv_cap_revoke(BOOT_CAP_CAPTABLE, SLOT_LENT));
    } else if (how == CUT_END) {
        expect("the source anew", rv_invoke(OP_DMA_END, SLOT_DMA, DMA_SOURCE, SLOT_OTHER, 0));
    } else if (how == CUT_STOP) {
        expect("the stop", rv_invoke(OP_DMA_STOP, SLOT_DMA, 0, 0, 0));
    }
    fill(memory, salt + 1u);
    for (; i < WORDS && loop_take(&loop, &w); i++) {
        before += w == word(i, salt);
        after += w == word(i, salt + 1u);
        other += w != word(i, salt) && w != word(i, salt + 1u);
    }
    say(&out, "dma: cut by %s: then %u words from before, %u from after, %u of the loop's own\n",
        cut_name[how], before, after, other);
    if (how == CUT_NONE) {
        return after != 0 && other == 0 && !busy();
    }
    return after == 0 && before <= LOOP_DEPTH && (!LOOP_ENDS || other == 0) && !busy();
}

int main(void);
int main(void)
{
    uint32_t base, size, w;
    out = log_out(BOOT_CAP_PRINT);
    if (rv_test_trace(BOOT_CAP_TEST) == KERR_OK) {
        say(&out, "dma: the kernel checks itself after every call\n");
    }
    expect("the free RAM as a frame", rv_retype(BOOT_CAP_FREE_RAM, CAP_FRAME, SLOT_FREE, &base));
    expect("map the free RAM", rv_process_install(BOOT_CAP_PROCESS, REGION_FREE, SLOT_FREE, RIGHT_R | RIGHT_W));
    expect("the buffer", rv_invoke(OP_FRAME_CARVE, SLOT_FREE, 0, BUFFER, SLOT_BUFFER));
    expect("the other buffer", rv_invoke(OP_FRAME_CARVE, SLOT_FREE, BUFFER, BUFFER, SLOT_OTHER));
    expect("the buffer lent", rv_cap_derive(BOOT_CAP_CAPTABLE, SLOT_LENT, SLOT_BUFFER, RIGHT_R));
    expect("the buffer's frame", rv_frame_info(SLOT_BUFFER, &base, &size));
    volatile uint32_t *memory = (volatile uint32_t *)(uintptr_t)base;
    loop_open(&loop, REGION_LOOP);

    expect("a channel", rv_invoke(OP_DMA_CARVE, BOOT_CAP_DMA, 0, 1, SLOT_DMA_LINE));
    expect("bind the channel", rv_invoke(OP_DMA_BIND, SLOT_DMA_LINE, BOOT_CAP_POOL, SLOT_DMA, 0));
    expect("the source", rv_invoke(OP_DMA_END, SLOT_DMA, DMA_SOURCE, SLOT_BUFFER, 0));
    expect("the sink", rv_invoke(OP_DMA_END, SLOT_DMA, DMA_SINK, LOOP_DEVICE, LOOP_REQUEST));

    check("a transfer brings back its range", echo(memory, 0, 1));
    check("a transfer from an offset brings back from there", echo(memory, 4, 2));
    fill(memory, 3);
    expect("a start", start(0, BUFFER));
    check("a start while one runs is refused", rv_invoke(OP_DMA_START, SLOT_DMA, 0, 4u, 0) == KERR_STATE);
    for (uint32_t i = 0; i < WORDS && loop_take(&loop, &w); i++) {
    }
    check("the transfer ends once the loop took it all", !busy());

    check("with no cut the channel reads on after the memory is written over", cut(memory, CUT_NONE, 10));
    check("a revoke stops the channel before it returns", cut(memory, CUT_REVOKE, 20));
    check("an end connected anew stops the channel before it returns", cut(memory, CUT_END, 30));
    check("a stop stops the channel before it returns", cut(memory, CUT_STOP, 40));

    say(&out, failures == 0 ? "dma: passed\n" : "dma: FAILED\n");
    rv_halt(BOOT_CAP_HALT, failures == 0 ? 0 : 2);
    return 0;
}
