#ifndef RVUOS_RADIO_PROF_H
#define RVUOS_RADIO_PROF_H

/*
 * What the driver's work costs, measured as it runs, for the log: each section's count, its total and its most,
 * in the core's cycles by the user-mode counter drv_main starts, which counts the process's own alone,
 * or, for a frame's time on the air, in microseconds.
 */

#include <stdint.h>

struct prof {
    uint32_t n;
    uint32_t most;
    uint64_t sum;
};

struct drv_prof {
    struct prof tx_lock;   /* the wait for the sending's lock, cycles */
    struct prof tx_prep;   /* a frame from the lock taken to its first try armed, cycles */
    struct prof tx_air;    /* a try from its arm to its end, the backoff among it, us */
    struct prof tx_gap;    /* from a frame's end to the next frame's first arm, us: the medium the sending leaves */
    struct prof rx_isr;    /* the interrupt's collect of the frames the MAC wrote, cycles, per frame */
    struct prof rx_read;   /* the station's thread's reading of a frame into the link, cycles */
    struct prof tx_link;   /* the first thread's sending of the link's frames, a frame or an A-MSDU, cycles */
    struct prof phy_track; /* the PHY kept to the temperature, cycles; see phy.c */
    struct prof rf_off;    /* the RF turned off for the power save, and on again, cycles; see mac.c */
    struct prof rf_on;
};
extern struct drv_prof drv_prof;

static inline uint32_t prof_cycles(void)
{
    uint32_t c;
    __asm__ volatile("csrr %0, 0x802" : "=r"(c));
    return c;
}

static inline void prof_add(struct prof *p, uint32_t v)
{
    p->n++;
    p->sum += v;
    if (v > p->most) {
        p->most = v;
    }
}

#endif
