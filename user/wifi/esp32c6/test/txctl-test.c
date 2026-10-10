/*
 * txctl.c on the host, under the sanitizers.
 * The backoff, a station's fair share of the medium: the window doubling from 15 to 1023 over a frame's tries,
 * draws that cover it evenly and never pass it, and two seeds drawing apart, a seed of 0 among them.
 * The rate control, AARF's steps: up after a run of first tries acknowledged, back at once when the probe's is lost,
 * down after two lost in a row, never past either end.
 */

#include <stdio.h>

#include "txctl.h"

#define RATES 9u
#define DRAWS 200000u

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("txctl-test: FAIL %s\n", what);
        failures++;
    }
}

static void windows(void)
{
    static const uint32_t want[] = { 15, 31, 63, 127, 255, 511, 1023, 1023, 1023 };
    int ok = 1;
    for (uint32_t t = 0; t < sizeof(want) / sizeof(want[0]); t++) {
        ok &= txctl_window(t) == want[t];
    }
    check(ok, "each try's window doubles from 15 to 1023, and stays there");
}

/* The draws for a try: none past the window, each of a first try's 16 within a tenth of its share, the mean its middle. */
static void draws(uint32_t try)
{
    static uint32_t seen[TXCTL_CW_MAX + 1u];
    struct txctl c;
    uint32_t cw = txctl_window(try), past = 0;
    double sum = 0;
    txctl_start(&c, RATES, 0, 12345u);
    for (uint32_t v = 0; v <= cw; v++) {
        seen[v] = 0;
    }
    for (uint32_t i = 0; i < DRAWS; i++) {
        uint32_t b = txctl_backoff(&c, try);
        past += b > cw;
        seen[b <= cw ? b : 0]++;
        sum += b;
    }
    int even = 1;
    for (uint32_t v = 0; v <= cw && try == 0; v++) {
        even &= seen[v] > DRAWS / 16u * 9u / 10u && seen[v] < DRAWS / 16u * 11u / 10u;
    }
    double mean = sum / DRAWS;
    check(past == 0, "no draw past the window");
    check(even, "a first try's draws cover its window evenly");
    check(mean > cw / 2.0 * 0.98 && mean < cw / 2.0 * 1.02, "the draws' mean at the window's middle");
}

static void seeds(void)
{
    struct txctl a, b, z;
    txctl_start(&a, RATES, 0, 1);
    txctl_start(&b, RATES, 0, 2);
    txctl_start(&z, RATES, 0, 0);
    int same = 1, stuck = 1;
    for (uint32_t i = 0; i < 64; i++) {
        same &= txctl_backoff(&a, 6) == txctl_backoff(&b, 6);
        stuck &= txctl_backoff(&z, 6) == 0;
    }
    check(!same, "two stations' seeds draw apart");
    check(!stuck, "a seed of 0 draws as any other");
}

static void first_tries(struct txctl *c, uint32_t n, int acked)
{
    for (uint32_t i = 0; i < n; i++) {
        txctl_first(c, acked);
    }
}

static void rates(void)
{
    struct txctl c;
    txctl_start(&c, RATES, 3, 1);
    check(txctl_rate(&c, 1) == 3 && txctl_rate(&c, 2) == 4 && txctl_rate(&c, 7) == RATES - 1u,
          "a frame's first two tries at the rate held, then a step slower each, to the slowest");
    first_tries(&c, TXCTL_UP_MIN - 1u, 1);
    check(c.at == 3, "a run short of up holds the rate");
    first_tries(&c, 1, 1);
    check(c.at == 2, "a run of up raises it");
    first_tries(&c, 1, 0);
    check(c.at == 3 && c.up == 2u * TXCTL_UP_MIN, "the probe's first try lost brings it back, up doubled");
    first_tries(&c, 2u * TXCTL_UP_MIN + 1u, 1);
    first_tries(&c, 1, 0);
    check(c.at == 2, "one lost past the probation holds it");
    first_tries(&c, 1, 0);
    check(c.at == 3 && c.up == TXCTL_UP_MIN, "two in a row lower it, and up is the least again");
    first_tries(&c, 1000, 1);
    check(c.at == 0, "the fastest is the top");
    first_tries(&c, 1000, 0);
    check(c.at == RATES - 1u, "the slowest is the bottom");
}

int main(void)
{
    windows();
    for (uint32_t t = 0; t <= TXCTL_CW_STEPS; t++) {
        draws(t);
    }
    seeds();
    rates();
    if (failures) {
        printf("txctl-test: %d failed\n", failures);
        return 1;
    }
    printf("txctl-test: every check passed\n");
    return 0;
}
