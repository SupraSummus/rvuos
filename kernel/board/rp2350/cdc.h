#ifndef RVUOS_CDC_H
#define RVUOS_CDC_H

/*
 * A USB CDC-ACM serial port on RP2350's USB controller, for the halt alone; see halt.c.
 * It is polled and blocks: the halt has nothing else to do.
 */

/* Reset the controller and connect to the host, which enumerates the port as it polls. */
void cdc_start(void);

/* One byte, sent in a packet of 64 once the host holds the port open, bare newlines as CR LF. */
void cdc_putc(char c);

/* One byte as it is. */
void cdc_put_raw(char c);

void cdc_puts(const char *s);

/* Send what the last packet holds and wait until the host has taken it. */
void cdc_flush(void);

/* Wait until the host lets the port go: it drops DTR, resets the bus or goes away. */
void cdc_wait_closed(void);

#endif
