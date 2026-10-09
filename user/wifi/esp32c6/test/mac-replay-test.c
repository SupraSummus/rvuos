/*
 * The bring-up's register sequences of macstart.c, run on the host against what the libraries' own did.
 * The case's file under test/replay/ lists their accesses in a run of their start, in order,
 * as tools/mac-trace.py's replay takes them from its trace:
 * rd() answers each read with the value theirs had, and wr() holds each write to theirs, address and value.
 * The calls the sequences still make into the libraries do nothing here, as replay leaves out their accesses.
 * mac_config is the driver's own start's whole MAC configuration, so its order is this case's.
 * make BOARD=esp32c6 wifi-esp32c6-replay takes the file again; see user/wifi/esp32c6/build.mk.
 * The accesses of theirs the own start leaves out by design are places in test/replay-expected.txt, in
 * test/diff-expected.txt's form: where an access of ours is not theirs, a place that their accesses from there match,
 * in order and exactly, is stepped over whole, and each place must be stepped over once.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mac.h"
#include "macregs.h"

#define STEPS  2048
#define PLACES 64

struct step {
    char op; /* 'R' or 'W' */
    uint32_t address, value, mask;
};
static struct step steps[STEPS], left[STEPS];
static unsigned count, next, lefts;
static struct place {
    unsigned first, n, line; /* its accesses in left[], and its first line in the file */
    int met;
} places[PLACES];
static unsigned nplaces;
static const char *const running = "config";
static int wrong; /* the case's first wrong access is told, and the rest of it does nothing */

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

/* A place not yet met whose accesses are theirs from here, stepped over; whether there was one. */
static int step_over(void)
{
    for (struct place *p = places; p < places + nplaces; p++) {
        if (p->met) {
            continue;
        }
        unsigned k = 0;
        while (k < p->n && next + k < count) {
            const struct step *e = &left[p->first + k], *t = &steps[next + k];
            if (t->op != e->op || t->address != e->address || (t->value & e->mask) != e->value) {
                break;
            }
            k++;
        }
        if (k == p->n) {
            p->met = 1;
            next += p->n;
            return 1;
        }
    }
    return 0;
}

static int theirs(char op, uint32_t a, uint32_t v)
{
    return next < count && steps[next].op == op && steps[next].address == a && (op == 'R' || steps[next].value == v);
}

uint32_t rd(uint32_t a)
{
    if (wrong) {
        return 0;
    }
    while (!theirs('R', a, 0) && step_over()) {
    }
    if (!theirs('R', a, 0)) {
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
    while (!theirs('W', a, v) && step_over()) {
    }
    if (!theirs('W', a, v)) {
        tell('W', a, v);
        return;
    }
    next++;
}

/*
 * The libphy call the power table is filled through, which stays theirs; its values are the recorded run's, so that
 * the writes the table feeds are held here. What is not named is unused by the table's readers.
 */
void phy_get_max_pwr(uint32_t index, uint8_t out[2])
{
    static const uint8_t pwr[0x2b] = {
        [0x00] = 0x14, [0x05] = 0x14, [0x09] = 0x14, [0x0a] = 0x14, [0x0b] = 0x14,
        [0x10] = 0x13, [0x11] = 0x13, [0x12] = 0x13, [0x13] = 0x13, [0x14] = 0x13, [0x15] = 0x13,
        [0x16] = 0x12, [0x17] = 0x12, [0x18] = 0x11, [0x19] = 0x0f, [0x1a] = 0x0f, [0x1b] = 0x0f,
    };

    out[0] = pwr[index];
    out[1] = 0;
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
    /* The receive base, the list the MAC fills first: their start's is their control block, which the trace holds as
       their write to RX_BASE and no more, so the own start is given it, and that write is held by its place. */
    uint32_t rx_base = 0;
    for (unsigned i = 0; i < count; i++) {
        if (steps[i].op == 'W' && steps[i].address == RX_BASE) {
            rx_base = steps[i].value;
        }
    }
    mac_config(host_slowclk_cal_get, host_coex_pti_get, rx_base);
}

/*
 * The places of the expected file: a reason on "#" lines, then the libraries' accesses, each "-R4 0xaddress 0xvalue"
 * or "-W4 ...", a value "/0xmask" holding the masked bits alone; a blank line or a reason ends a place.
 */
static void expect(const char *path)
{
    char line[512];
    unsigned no = 0;
    int open = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("mac-replay-test: cannot open %s\n", path);
        exit(1);
    }
    while (fgets(line, sizeof line, f)) {
        char op;
        unsigned width, a, v, m = 0xffffffffu;
        no++;
        if (line[0] == '#' || line[0] == '\n') {
            open = 0;
            continue;
        }
        int got = sscanf(line, "-%c%u %x %x/%x", &op, &width, &a, &v, &m);
        if (got < 4 || (op != 'R' && op != 'W') || width != 4) {
            printf("mac-replay-test: %s:%u: not an access of theirs: %s", path, no, line);
            exit(1);
        }
        if (!open) {
            if (nplaces == PLACES) {
                printf("mac-replay-test: %s: more than %d places\n", path, PLACES);
                exit(1);
            }
            places[nplaces++] = (struct place){lefts, 0, no, 0};
            open = 1;
        }
        if (lefts == STEPS) {
            printf("mac-replay-test: %s: more than %d accesses\n", path, STEPS);
            exit(1);
        }
        left[lefts++] = (struct step){op, a, v & m, m};
        places[nplaces - 1].n++;
    }
    fclose(f);
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
        steps[count++] = (struct step){op, a, v, 0xffffffffu};
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        printf("usage: mac-replay-test DIR [EXPECTED]\n");
        return 2;
    }
    load(argv[1], running);
    if (argc == 3) {
        expect(argv[2]);
    }
    run();
    while (!wrong && next != count && step_over()) {
    }
    if (!wrong && next != count) {
        printf("mac-replay-test: %s: their run made %u accesses more, from %c 0x%08x 0x%08x\n", running,
               count - next, steps[next].op, steps[next].address, steps[next].value);
        wrong = 1;
    }
    for (unsigned p = 0; p < nplaces; p++) {
        if (!places[p].met) {
            printf("mac-replay-test: %s:%u: the place left out by design is not where theirs runs\n", argv[2],
                   places[p].line);
            wrong = 1;
        }
    }
    if (wrong) {
        printf("mac-replay-test: the case failed\n");
        return 1;
    }
    printf("mac-replay-test: ok, %u accesses", count);
    if (nplaces != 0) {
        printf(", %u of them left out by design in %u place%s", lefts, nplaces, nplaces == 1 ? "" : "s");
    }
    printf("\n");
    return 0;
}
