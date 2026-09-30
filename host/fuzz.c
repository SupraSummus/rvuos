/*
 * libFuzzer harness over the system call surface.
 *
 * The input is a sequence of replay_record, as many as the replay driver takes.
 * Each is passed to the thread it names as the driver would, then performed,
 * and every kernel invariant is checked afterwards by the kernel's own self-check.
 * Because no system call takes a pointer,
 * the registers are the entire attack surface of a call,
 * and user memory matters only where a record loads or stores a word itself, see rvuos/replay.h,
 * which the host decides from the process's regions.
 *
 * Run with files as arguments to replay them once,
 * or with a corpus directory to fuzz.
 * With --verbose the kernel's trace lines are printed,
 * in the same format the kernel prints on QEMU,
 * so tests/differential.py can compare the two.
 * With --print each file given is printed instead, a record per line with the names of host/ops.h.
 * With --isolated each file given replays as if it ran alone:
 * from RAM as at power-up, ending at its first invariant report,
 * and followed by HOST_INPUT_END, where tests/differential.py and tests/mutants.sh cut the output.
 * A crash still ends the run, and leaves its input without that line.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "ops.h"

static bool host_print;

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    for (int i = 1; i < *argc; i++) {
        if (strcmp((*argv)[i], "--verbose") == 0) {
            host_verbose = true;
        } else if (strcmp((*argv)[i], "--isolated") == 0) {
            host_isolated = true;
        } else if (strcmp((*argv)[i], "--print") == 0) {
            host_print = true;
        }
    }
    if (host_isolated) {
        /* A report on stderr comes after the trace lines before it where the two share a pipe. */
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
    if (host_isolated || host_print) {
        /*
         * An input that leaves more mallocs than frees libFuzzer runs again to look for a leak,
         * and the first boot allocates RAM, as the first line printed does its buffer, so an input would end twice.
         * libFuzzer reads its flags after this, and LeakSanitizer still looks when the run ends.
         */
        static char no_leak_rerun[] = "-detect_leaks=0";
        char **args = calloc((size_t)*argc + 2, sizeof(*args));
        if (args == NULL) {
            abort();
        }
        memcpy(args, *argv, (size_t)*argc * sizeof(*args));
        args[(*argc)++] = no_leak_rerun;
        *argv = args;
    }
    return 0;
}

static void replay(const uint8_t *data, size_t size)
{
    host_boot();
    selfcheck_run();
    /* What OP_DEBUG_TRACE does, which the driver calls here. */
    debug_trace = true;
    sched_accounts_fill();
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
        host_event(&c);
    }
    host_end();
}

/* The records as the harness takes them, by name where the operation has one. */
static void print(const uint8_t *data, size_t size)
{
    for (size_t off = 0; off + sizeof(struct replay_record) <= size; off += sizeof(struct replay_record)) {
        struct replay_record c;
        memcpy(&c, data + off, sizeof(c));
        const struct op_info *info = op_info(c.op);
        char op[16];
        snprintf(op, sizeof(op), "op %u", (unsigned)c.op);
        printf("%3zu actor %u %-20s slot %-3u a1 0x%08x a2 0x%08x a3 0x%08x\n", off / sizeof(c), (unsigned)c.actor,
               info != NULL ? info->name : op, (unsigned)c.slot, (unsigned)c.a1, (unsigned)c.a2, (unsigned)c.a3);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (host_print) {
        print(data, size);
        return 0;
    }
    /* Back through the longjmp, the root task halted the machine or an isolated input broke an invariant. */
    if (setjmp(host_halt_jmp) == 0) {
        replay(data, size);
    }
    if (host_isolated) {
        puts(HOST_INPUT_END);
        fflush(stdout);
    }
    return 0;
}
