/*
 * The bring-up's register sequences of macstart.c, run on the host against what the libraries' own did.
 * The case's file under test/replay/ lists their accesses in a run of their start, in order,
 * as tools/mac-trace.py's replay takes them from its trace:
 * rd() answers each read with the value theirs had, and wr() holds each write to theirs, address and value.
 * The calls the sequences still make into the libraries do nothing here, as replay leaves out their accesses.
 * mac_config is the driver's own start's whole MAC configuration, so its order is this case's.
 * make BOARD=esp32c6 wifi-esp32c6-replay takes the file again; see user/wifi/esp32c6/build.mk.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mac.h"
#include "macregs.h"

#define STEPS 2048

static struct step {
    char op; /* 'R' or 'W' */
    uint32_t address, value;
} steps[STEPS];
static unsigned count, next;
static const char *const running = "config";
static int wrong; /* the case's first wrong access is told, and the rest of it does nothing */

/* The libraries' receive control block, which their write to RX_BASE carries and the driver's mac_rx_base_init reads. */
uint32_t wDevCtrl;

static void tell(char op, uint32_t a, uint32_t v)
{
    printf("mac-replay-test: %s: access %u, %c 0x%08x", running, next + 1, op, a);
    if (op == 'W') {
        printf(" 0x%08x", v); /* a read of ours has no value yet */
    }
    if (next == count) {
        printf(", past their last\n");
    } else {
        printf(", where theirs was %c 0x%08x 0x%08x\n", steps[next].op, steps[next].address, steps[next].value);
    }
    wrong = 1;
}

uint32_t rd(uint32_t a)
{
    if (wrong) {
        return 0;
    }
    if (next == count || steps[next].op != 'R' || steps[next].address != a) {
        tell('R', a, 0);
        return 0;
    }
    return steps[next++].value;
}

void wr(uint32_t a, uint32_t v)
{
    if (wrong) {
        return;
    }
    if (next == count || steps[next].op != 'W' || steps[next].address != a || steps[next].value != v) {
        tell('W', a, v);
        return;
    }
    next++;
}

/* The HE group's one call left theirs: the power table. */
void hal_init_tx_pwr(void)
{
}

/* The ROM's, which the low-rate group calls; its own accesses are the ROM's and stay out of the replay. */
void phy_disable_low_rate(void)
{
}

/*
 * The adapter's trace, the driver's own, which mac_config calls to name its segments; a no-op here, since the
 * segments are not device accesses and the fixture holds none.
 */
void osi_trace(const char *what, uint32_t a0, uint32_t a1)
{
    (void)what;
    (void)a0;
    (void)a1;
}

/* The adapter's slow clock period, the recorded run's, in Q13.19 microseconds. */
static uint32_t host_slowclk_cal_get(void)
{
    return 3855000u;
}

/* The coex PTI bytes the recorded run's adapter gave, by their event; the twelve ofdma events among them. */
static int host_coex_pti_get(uint32_t event, uint8_t *pti)
{
    switch (event) {
    case 1:
        *pti = 5;
        return 0;
    case 3:
        *pti = 7;
        return 0;
    case 0xa:
        *pti = 3;
        return 0;
    case 0xf:
        *pti = 1;
        return 0;
    }
    return -1;
}

/*
 * The libraries' hal_init, which mac_config in macstart.c runs, as main.c's own start calls it:
 * one case, so that a change to that order fails here rather than on the board alone.
 */
static void run(void)
{
    /* The libraries' receive control block, which the trace holds as their write to RX_BASE and no more,
       so the own read is given it, and that write is held by address and place, not by value. */
    for (unsigned i = 0; i < count; i++) {
        if (steps[i].op == 'W' && steps[i].address == RX_BASE) {
            wDevCtrl = steps[i].value;
        }
    }
    mac_config(host_slowclk_cal_get, host_coex_pti_get);
}

static void load(const char *dir, const char *file)
{
    char path[256], line[512];
    snprintf(path, sizeof path, "%s/%s.txt", dir, file);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("mac-replay-test: %s: cannot open %s\n", file, path);
        exit(1);
    }
    count = 0;
    while (fgets(line, sizeof line, f)) {
        char op;
        unsigned width, a, v;
        if (line[0] == '#') {
            /* A comment: the header naming the functions can run past the buffer, so skip to its end. */
            while (!strchr(line, '\n') && fgets(line, sizeof line, f)) {
            }
            continue;
        }
        if (!strchr(line, '\n')) {
            printf("mac-replay-test: %s: a line longer than %zu bytes\n", path, sizeof line - 2);
            exit(1);
        }
        if (sscanf(line, "%c%u %x %x", &op, &width, &a, &v) != 4 || (op != 'R' && op != 'W') || width != 4) {
            printf("mac-replay-test: %s: not an access of four bytes: %s", path, line);
            exit(1);
        }
        if (count == STEPS) {
            printf("mac-replay-test: %s: more than %d accesses\n", path, STEPS);
            exit(1);
        }
        steps[count++] = (struct step){op, a, v};
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: mac-replay-test DIR\n");
        return 2;
    }
    load(argv[1], running);
    run();
    if (!wrong && next != count) {
        printf("mac-replay-test: %s: their run made %u accesses more, from %c 0x%08x 0x%08x\n", running,
               count - next, steps[next].op, steps[next].address, steps[next].value);
        wrong = 1;
    }
    if (wrong) {
        printf("mac-replay-test: the case failed\n");
        return 1;
    }
    printf("mac-replay-test: ok, %u accesses\n", count);
    return 0;
}
