#ifndef RVUOS_WIFI_BT_H
#define RVUOS_WIFI_BT_H

/*
 * EXPERIMENT: the CYW43439's Bluetooth core, brought up beside the running WLAN firmware,
 * and taken over with a little code of our own in the core's RAM; see bt.c.
 * Built only with WIFI_BT=1, so the Wi-Fi system without it is as it is on main.
 */

#include <stdint.h>

struct cyw43;
struct out;

/*
 * What the Bluetooth bring-up needs of the driver: the chip, where its log is said, the firmware blob it hands
 * over a byte range at a time, and a way to keep answering the root task's check while the core is busy.
 */
struct bt_io {
    struct cyw43 *chip;
    struct out *out;
    void (*answer)(void *context);
    void *answer_context;
    const uint8_t *(*blob)(void *context, uint32_t off, uint32_t *left);
    void *blob_context;
    uint32_t channel; /* the transmitter test's, 0..39: 2402 + 2 * channel MHz */
    uint32_t poke; /* a word our code on the core writes at poke_at on every call, 0 for none; see bt_poke_ok */
    uint32_t poke_at;
    uint32_t mem; /* a word the host writes straight into the core's RAM before the test, 0 for none */
    uint32_t mem_at;
};

/*
 * Brings the core up with our code on it, checks that the code answers and what the core's timer and a modem write
 * cost, and runs the transmitter test with the RAM word and the poke asked for; 0, or -1 if it never came up.
 */
int bt_run(struct bt_io *io);

#endif
