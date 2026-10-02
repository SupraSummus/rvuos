#ifndef RVUOS_WIFI_CYW43_H
#define RVUOS_WIFI_CYW43_H

/*
 * The CYW43439 behind its gSPI bus: the bus's registers, the chip's backplane, and its boot.
 * The protocol is Infineon's, as embassy's cyw43 driver (MIT or Apache-2.0) speaks it;
 * the constants and the order of the steps come from there.
 */

#include <stdint.h>

#include "gspi.h"

/* How the chip layer waits: the process supplies it, since only the process knows its timer. */
struct cyw43;
typedef void (*cyw43_sleep_fn)(struct cyw43 *chip, uint32_t us);

struct cyw43 {
    struct gspi bus;
    cyw43_sleep_fn sleep;
    uint32_t window;    /* the backplane window last set, or ~0 */
    uint32_t status;    /* the last status word */
    uint32_t step;      /* where a failure happened */
    uint32_t detail;    /* what that step read */
};

/* Steps, for a failure's report. */
enum {
    STEP_NONE,
    STEP_BUS_TEST,      /* the test register never read FEEDBEAD */
    STEP_BUS_RW,        /* the read-write test register did not keep a word */
    STEP_BUS_CONFIG,    /* the test register reads wrong in 32-bit mode */
    STEP_ALP,           /* the ALP clock never came */
    STEP_CHIP_ID,       /* the chip is not a 43439 */
    STEP_FW_VERIFY,     /* the firmware read back differs */
    STEP_NVRAM_VERIFY,  /* the NVRAM's length word read back differs */
    STEP_CORE_UP,       /* the WLAN core did not come out of reset */
    STEP_HT,            /* the HT clock never came */
    STEP_F2,            /* function 2 never became ready */
    STEP_WLAN_INIT,     /* the firmware refused its setup, detail its error */
    STEP_SCAN,          /* the firmware refused a scan */
    STEP_JOIN,          /* the join was refused or failed, detail the error or -1 */
    STEP_AP,            /* the access point was refused */
};

/* Powers the chip and brings its bus up in 32-bit mode; 0 on success. */
int cyw43_bus_up(struct cyw43 *chip);
/* Readies the chip for its firmware, after which cyw43_load writes it. */
int cyw43_prepare(struct cyw43 *chip);
/* Writes bytes to the chip's RAM at an offset; len need not be a multiple of four. */
void cyw43_load(struct cyw43 *chip, uint32_t ram_offset, const uint8_t *data, uint32_t len);
/* Checks a stretch already written. */
int cyw43_verify(struct cyw43 *chip, uint32_t ram_offset, const uint8_t *data, uint32_t len);
/* Writes the NVRAM and its length word, starts the core and waits for it; 0 on success. */
int cyw43_start(struct cyw43 *chip, const uint8_t *nvram, uint32_t len);

uint32_t cyw43_bp_read32(struct cyw43 *chip, uint32_t addr);
uint16_t cyw43_bp_read16(struct cyw43 *chip, uint32_t addr);

/* Function 2, the firmware's packets: the bus status, which says whether one waits and how long it is. */
#define CYW43_STATUS_F2_PKT_AVAILABLE 0x00000100u
#define CYW43_STATUS_F2_PKT_LEN(s)    (((s) & 0x000ffe00u) >> 9)
uint32_t cyw43_status(struct cyw43 *chip);
/* Reads a packet of len bytes into words. */
void cyw43_wlan_read(struct cyw43 *chip, uint32_t *words, uint32_t len);
/* Writes the len bytes that follow words[0], which the call fills with the command. */
void cyw43_wlan_write(struct cyw43 *chip, uint32_t *words, uint32_t len);

#define CYW43_CHIP_ID 43439u

#endif
