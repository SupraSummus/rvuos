/*
 * The bring-up's register sequences of macstart.c, run on the host against what the libraries' own did.
 * Each case's file under test/replay/ lists their accesses in a run of their start, in order,
 * as tools/mac-trace.py's replay takes them from its trace:
 * rd() answers each read with the value theirs had, and wr() holds each write to theirs, address and value.
 * The calls the sequences still make into the libraries do nothing here, as replay leaves out their accesses.
 * make BOARD=esp32c6 wifi-esp32c6-replay takes the files again; see user/wifi/esp32c6/build.mk.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mac.h"
#include "macregs.h"

#define STEPS 512

static struct step {
    char op; /* 'R' or 'W' */
    uint32_t address, value;
} steps[STEPS];
static unsigned count, next;
static const char *running;
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

/* The libraries' functions the sequences call, which keep their accesses. */
void hal_he_set_mac_delay(uint32_t a)
{
    (void)a;
}

void hal_he_set_ack_rate(uint32_t a)
{
    (void)a;
}

void hal_he_set_bbrxhung_time(uint32_t a)
{
    (void)a;
}

/* The ROM's, which the low-rate group calls; its own accesses are the ROM's and stay out of the replay. */
void phy_disable_low_rate(void)
{
}

/* Their hal_init up to the HE group, as the driver's drv_mac_config calls it, in main.c. */
static void head(void)
{
    mac_config_start();
    mac_queues_init();
    for (uint32_t i = 0; i < 4u; i++) {
        mac_rx_policy_word(i);
        mac_rx_set_policy(i);
    }
    /* The libraries' receive control block, which the trace holds as their write to RX_BASE and no more,
       so the own read is given it, and that write is held by address and place, not by value. */
    for (unsigned i = 0; i < count; i++) {
        if (steps[i].op == 'W' && steps[i].address == RX_BASE) {
            wDevCtrl = steps[i].value;
        }
    }
    mac_rx_base_init();
}

/* Their hal_init from the low-rate group on, and the coex PTI with the values the recorded run's coex gave. */
static void tail(void)
{
    mac_low_rate_disable();
    mac_crypto_init();
    mac_antenna_init();
    mac_config_finish();
    mac_coex_pti_init();
    mac_rx_active_pti(0);
    mac_rx_ack_pti(7);
    mac_wifi_default_pti(1);
}

static const struct {
    const char *file;
    void (*run)(void);
} cases[] = {
    {"config-head", head},
    {"config-tail", tail},
};

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
        if (!strchr(line, '\n')) {
            printf("mac-replay-test: %s: a line longer than %zu bytes\n", path, sizeof line - 2);
            exit(1);
        }
        if (line[0] == '#') {
            continue;
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
    unsigned failures = 0, accesses = 0;
    if (argc != 2) {
        printf("usage: mac-replay-test DIR\n");
        return 2;
    }
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        running = cases[i].file;
        load(argv[1], running);
        next = 0;
        wrong = 0;
        cases[i].run();
        if (!wrong && next != count) {
            printf("mac-replay-test: %s: their run made %u accesses more, from %c 0x%08x 0x%08x\n", running,
                   count - next, steps[next].op, steps[next].address, steps[next].value);
            wrong = 1;
        }
        failures += wrong;
        accesses += count;
    }
    if (failures) {
        printf("mac-replay-test: %u of %zu cases failed\n", failures, sizeof cases / sizeof cases[0]);
        return 1;
    }
    printf("mac-replay-test: ok, %zu cases, %u accesses\n", sizeof cases / sizeof cases[0], accesses);
    return 0;
}
