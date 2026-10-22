#ifndef RVUOS_RADIO_DEVREGS_H
#define RVUOS_RADIO_DEVREGS_H

/*
 * A device register's read and write, reached the one way by the driver's bring-up:
 * macregs.h's MAC and HAL words and btbb.c's baseband words both go through these.
 * On the board they are volatile loads and stores; on the host, for the bring-up's replay,
 * test/mac-replay-test.c and test/btbb-host.c give their own under MAC_HOST,
 * which answer from the libraries' recorded accesses and take the driver's, see mac-trace.py.
 */

#include <stdint.h>

#ifndef MAC_HOST
static inline uint32_t rd(uint32_t a)
{
    return *(volatile uint32_t *)a;
}

static inline void wr(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}
#else
uint32_t rd(uint32_t a);
void wr(uint32_t a, uint32_t v);
#endif

#endif
