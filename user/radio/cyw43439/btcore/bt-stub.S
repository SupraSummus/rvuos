/*
 * Our own code for the CYW43439's Bluetooth core; see user/wifi/bt.c, bt-map.h and user/wifi/BT.md.
 *
 * The core is a Cortex-M3. Its ROM branches into the vendor patch's code in the core's RAM, and user/wifi/bt.c takes one
 * such point over: it uploads .takeover to that point, a branch to the stub, and .text to free RAM beside it.
 * The stub runs on the point's own first instruction and goes back into the patch.
 *
 * It leaves the core's registers as it found them, since the code it returns into is the firmware's, mid-flow.
 * In between it serves the mailbox the host fills: a magic word, a command, two arguments, a result and two
 * counters, so the host can have it read and write single words of the core's memory, the modem's registers
 * among it. The magic guards the servicing, so the rubbish RAM holds at a boot is never acted on.
 *
 * It also carries a persistent poke: a register and a value the host leaves in the mailbox, written on every call
 * of the point, right after the point's own write, so ours is the last one in. The firmware writes the channel
 * word itself on each call, so a one-time write of it is overwritten and says nothing; this is the write that can
 * stand. The address is the switch -- zero, as the mailbox comes up, means no poke -- and the host keeps the
 * value inside the band; the code writes whatever it is handed.
 *
 * The free RAM beside the patch is not all free: the firmware uses a band just past the common path on a
 * transmission, and more past the mailbox at a boot, so the timed run lives in the second piece of free RAM
 * (BT_MEASURE), reached on a measure alone, while every other command stays in the low piece (BT_STUB).
 */
    .syntax unified
    .thumb
#include "bt-map.h"

/* The instruction the point itself runs, and the one after it. */
    .section .callee, "a"
the_point_callee:
    .section .callee2, "a"
the_point_next:

/* The branch the host writes over the point in the patch. */
    .section .takeover, "ax"
    b.w bt_stub_start

    .section .text, "ax"
    .globl bt_stub_start
    .type bt_stub_start, %function
bt_stub_start:
    bl the_point_callee                 /* the point's own first instruction, kept */
    push {r0, r1, r2, r3}

    movw r3, #:lower16:mbox_magic       /* one base for every word of the mailbox */
    movt r3, #:upper16:mbox_magic

    ldr r1, [r3, #MB_RUNS]              /* one more run */
    adds r1, #1
    str r1, [r3, #MB_RUNS]

    ldr r0, [r3, #MB_POKE_AT]           /* the persistent poke, after the point's own write */
    cbz r0, 1f
    ldr r1, [r3, #MB_POKE_VAL]
    str r1, [r0]
1:
    ldr r1, [r3, #MB_MAGIC]             /* armed by the host? */
    movw r2, #:lower16:MB_ARMED
    movt r2, #:upper16:MB_ARMED
    cmp r1, r2
    bne done

    ldr r2, [r3, #MB_CMD]               /* anything to do? */
    cmp r2, #MB_READ
    beq do_read
    cmp r2, #MB_WRITE
    beq do_write
    cmp r2, #MB_MEASURE
    beq do_measure
    cmp r2, #MB_TOUCH
    beq do_touch
    b done

do_write:
    ldr r1, [r3, #MB_ARG0]              /* write arg1 at arg0 */
    ldr r2, [r3, #MB_ARG1]
    str r2, [r1]
    b finish

do_read:
    ldr r1, [r3, #MB_ARG0]              /* read what is at arg0 */
    ldr r2, [r1]
    str r2, [r3, #MB_RES]

finish:
    movs r1, #0                         /* served, and the mailbox is disarmed */
    str r1, [r3, #MB_CMD]
    str r1, [r3, #MB_MAGIC]
    ldr r1, [r3, #MB_DONE]
    adds r1, #1
    str r1, [r3, #MB_DONE]

done:
    pop {r0, r1, r2, r3}
    b.w the_point_next
    .size bt_stub_start, . - bt_stub_start
    .globl bt_stub_end
bt_stub_end:

/*
 * A timed run, reached on a measure alone: the word at arg0 is written arg1 times, while the counter the firmware's
 * own delay polls (fn 0x7130 reads 0x32a004, read only here) is taken before and after, so the host gets the ticks
 * the run took. Command 3 counts a fresh value into the word, so its length shows in the word as well; command 4
 * writes the word's own value back, which is what a modem register needs -- a count would spoil it -- and the word
 * then does not move. The two share the loop, since the free RAM here is one short piece.
 */
    .section .measure, "ax"
    .globl bt_measure_start
    .type bt_measure_start, %function
bt_measure_start:
do_measure:
    movs r1, #0                         /* count a fresh value in */
    b measure_run
do_touch:
    movs r1, #1                         /* or write the word's own value back */
measure_run:
    push {r4, r5, r6, r7}
    mov r6, r1
    movw r7, #:lower16:mbox_magic
    movt r7, #:upper16:mbox_magic
    ldr r4, [r7, #MB_ARG0]              /* the word to write */
    ldr r5, [r7, #MB_ARG1]              /* how many times */
    cmp r5, #0
    bne 1f
    movs r5, #64
1:
    movw r0, #0xa004                    /* the counter the firmware's own delay polls */
    movt r0, #0x0032
    ldr r2, [r0]                        /* the value before the run */
    ldr r3, [r4]
    cbz r6, 3f
2:
    str r3, [r4]
    subs r5, #1
    bne 2b
    b 4f
3:
    adds r3, #1
    str r3, [r4]
    subs r5, #1
    bne 3b
4:
    ldr r0, [r0]                        /* the counter after the run */
    subs r2, r0, r2                     /* its ticks over the run */
    str r2, [r7, #MB_RES]
    mov r3, r7                          /* finish works from the mailbox base in r3, which the run used */
    pop {r4, r5, r6, r7}
    b finish
    .size bt_measure_start, . - bt_measure_start
    .globl bt_measure_end
bt_measure_end:

/* The mailbox itself; its offsets are bt-map.h's. */
    .section .mbox, "aw"
    . = MB_MAGIC
    .globl mbox_magic
mbox_magic: .word 0
    . = MB_CMD
mbox_cmd:   .word 0
    . = MB_ARG0
mbox_arg0:  .word 0
    . = MB_ARG1
mbox_arg1:  .word 0
    . = MB_RES
mbox_res:   .word 0
    . = MB_DONE
mbox_done:  .word 0
    . = MB_RUNS
mbox_runs:  .word 0
    . = MB_POKE_AT
mbox_poke_at:  .word 0
    . = MB_POKE_VAL
mbox_poke_val: .word 0
