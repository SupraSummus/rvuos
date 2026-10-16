#ifndef RVUOS_USER_DEVICES_H
#define RVUOS_USER_DEVICES_H

/*
 * The devices of nRF52840 the boot grants as frames from BOOT_CAP_DEVICES up,
 * in the order of DEVICE_RANGE_LIST in the kernel's board.h, as many as it lists.
 * A frame maps at its own address, so a program names a register by the device's base.
 */
#define BOOT_DEVICES 5
#define BOOT_CAP_GPIO   (BOOT_CAP_DEVICES + 0) /* both ports, P0 and P1 */
#define BOOT_CAP_RADIO  (BOOT_CAP_DEVICES + 1) /* the 2.4 GHz radio, a bus master */
#define BOOT_CAP_TIMER2 (BOOT_CAP_DEVICES + 2) /* a TIMER the kernel leaves free */
#define BOOT_CAP_TIMER3 (BOOT_CAP_DEVICES + 3) /* two more, for routes to the radio */
#define BOOT_CAP_TIMER4 (BOOT_CAP_DEVICES + 4)

/* The devices' registers and their interrupt lines, each the peripheral's ID. */
#define RADIO_BASE  0x40001000u
#define RADIO_IRQ   1u
#define TIMER2_BASE 0x4000a000u
#define TIMER2_IRQ  10u
#define TIMER3_BASE 0x4001a000u
#define TIMER3_IRQ  26u
#define TIMER4_BASE 0x4001b000u
#define TIMER4_IRQ  27u

#define GPIO_P0_BASE 0x50000000u
#define GPIO_P1_BASE 0x50000300u

/* A port's registers, as offsets from its base. */
#define GPIO_OUTSET     0x508u
#define GPIO_OUTCLR     0x50cu
#define GPIO_DIRSET     0x518u

/* The LED as the Fanstel BT840X the port was brought up on wires it: P1.06, lit while the pin is low. */
#define LED_PORT_BASE GPIO_P1_BASE
#define LED_PIN       6u

#endif
