#ifndef RVUOS_MSPI_H
#define RVUOS_MSPI_H

/* The flash at 80 MHz, quad I/O where it can, and with PSRAM=1 the PSRAM; once the SPLL is set, see mspi.c. */
void mspi_init(void);

#endif
