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

/*
 * Where the harness puts the boot pool; inside the kernel's part of RAM.
 * The log lies at KLOG_BASE as on the target, since the root task can install it
 * and the PMP images of the two builds must agree.
 */
#define HOST_BOOT_POOL_BASE (RAM_BASE + 0x1000u)
#define HOST_BOOT_POOL_SIZE 0x1000u

/* khalt() longjmps here, and so does host_violated() when isolated. */
extern jmp_buf host_halt_jmp;
extern int host_halt_code;

/* Print kernel console output. */
extern bool host_verbose;

/*
 * Replay each input as if it ran alone; see host/fuzz.c.
 * After each the harness prints HOST_INPUT_END on a line of its own.
 */
extern bool host_isolated;
#define HOST_INPUT_END "isolated: end of input"

/*
 * End the input after an invariant report:
 * the run too, as libFuzzer needs to see a failure, unless isolated.
 */
__attribute__((noreturn)) void host_violated(void);

/* Reset the machine and boot the root task as the replay driver sees it. */
struct thread *host_boot(void);

/* Perform one system call on the current thread. Returns the status. */
uint32_t host_syscall(const struct replay_record *c);

/*
 * Deliver one event: pass the record to the thread it names
 * the way the replay driver does, then perform it.
 * False when the driver died on the way, see host_driver_alive.
 */
bool host_event(const struct replay_record *c);

/*
 * True while the running driver thread could still run on real hardware:
 * its process maps the driver's code read-execute
 * and its data, the UART and the log read-write.
 * On QEMU the driver faults as soon as this stops holding;
 * the host has no instruction fetch to fault, so it checks instead,
 * and the harness must stop at the first false, as QEMU would.
 */
bool host_driver_alive(void);

/*
 * What there is of each unit a paid loop counts in, over every object of every pool:
 * a node is a filled slot or a pool's own node, a link a node below another,
 * an object an object, and a waiter a thread waiting.
 */
enum { UNIT_NODE, UNIT_LINK, UNIT_OBJECT, UNIT_WAITER, UNITS };
void host_units(unsigned out[UNITS]);

/*
 * Around each call: note the state before it, then check what the call changed
 * against what its caller held; see host/history.c.
 */
void history_begin(void);
void history_end(void);

#ifdef RVUOS_WORK
/* Around each call in the harness fuzz-work: count its loops, then check their claims. */
void work_begin(void);
void work_end(void);
/* intr_pending was asked: a paid loop may step again. */
void work_ask(void);
void *work_memset(void *dst, int c, size_t n);
void *work_memcpy(void *dst, const void *src, size_t n);
/* Forget the frames a halt or an isolated report left open; see host_boot. */
void work_reset(void);
#endif

#endif
