#ifndef RVUOS_HOST_HARNESS_H
#define RVUOS_HOST_HARNESS_H

/*
 * Shared state of the host build: the simulated machine
 * and the layout the harness boots the kernel with.
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"
#include "object.h"
#include "pmp.h"
#include "rvuos/replay.h"

/* Where the harness puts the boot pool; inside the kernel's part of RAM. */
#define HOST_BOOT_POOL_BASE (RAM_BASE + 0x1000u)
#define HOST_BOOT_POOL_SIZE 0x1000u

#define FREE_BASE (USER_DATA_BASE + USER_DATA_SIZE)

/* Set when khalt() is called; the harness longjmps here. */
extern jmp_buf host_halt_jmp;
extern int host_halt_code;

/* Print kernel console output. */
extern bool host_verbose;

/* Reset the machine and boot the root task as the replay driver sees it. */
struct thread *host_boot(void);

/* Perform one system call on the current thread. Returns the status. */
uint32_t host_syscall(const struct replay_record *c);

/*
 * True while the running driver thread could still run on real hardware:
 * its process maps the driver's code read-execute and its data read-write.
 * On QEMU the driver faults as soon as this stops holding;
 * the host has no instruction fetch to fault, so it checks instead,
 * and the harness must stop at the first false, as QEMU would.
 */
bool host_driver_alive(void);

#endif
