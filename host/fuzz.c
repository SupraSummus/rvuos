/*
 * libFuzzer harness over the system call surface.
 *
 * The input is a sequence of replay_record, as many as the replay driver takes.
 * Each is performed on whichever driver thread runs and every kernel invariant
 * is checked afterwards by the kernel's own self-check.
 * Because no system call takes a pointer,
 * the registers are the entire attack surface
 * and the fuzzer needs no model of user memory.
 *
 * Run with files as arguments to replay them once,
 * or with a corpus directory to fuzz.
 * With --verbose the kernel's trace lines are printed,
 * in the same format the kernel prints on QEMU,
 * so tests/differential.py can compare the two.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    for (int i = 1; i < *argc; i++) {
        if (strcmp((*argv)[i], "--verbose") == 0) {
            host_verbose = true;
        }
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (setjmp(host_halt_jmp) != 0) {
        /* The root task halted the machine, or the self-check failed. */
        return 0;
    }

    host_boot();
    selfcheck_run();
    debug_trace = true;
    /* The driver starts its second thread traced; see rvuos/replay.h. */
    if (host_syscall(&replay_start) != KERR_OK) {
        abort();
    }

    if (size > REPLAY_MAX_RECORDS * sizeof(struct replay_record)) {
        size = REPLAY_MAX_RECORDS * sizeof(struct replay_record);
    }
    for (size_t off = 0; off + sizeof(struct replay_record) <= size;
         off += sizeof(struct replay_record)) {
        struct replay_record c;
        memcpy(&c, data + off, sizeof(c));
        host_syscall(&c);
        if (!host_driver_alive()) {
            break;
        }
    }
    /* The replay driver ends with ebreak; a dead process faults at once. */
    kputs("user fault\n");
    return 0;
}
