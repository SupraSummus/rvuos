/*
 * Machine-mode entry and trap glue.
 *
 * While user mode runs, mscratch points at the current thread's trap frame.
 * While the kernel runs, mscratch is zero.
 * One CSR swap therefore yields both the frame to save into
 * and the knowledge of which mode trapped.
 *
 * Between the swap and the second csrrw in trap_entry,
 * mscratch holds the user's sp.
 * A trap in that window would save registers through it in machine mode.
 * None can happen: the hardware clears MIE on trap and the kernel
 * never sets it again, so interrupts wait for the mret,
 * the frame is RAM so the stores cannot fault,
 * and no PMP entry is locked, so PMP does not apply to machine mode.
 * Revisit when Smepmp arrives.
 */

#define FRAME_MEPC   128
#define FRAME_MCAUSE 132
#define FRAME_MTVAL  136

    .section .text.start, "ax"
    .globl _start
_start:
    /*
     * Trap vector in vectored mode; the low two bits select the mode.
     * The ESP32-C6 has no direct mode, and QEMU has both,
     * so every board takes the vectored table below.
     */
    la      t0, trap_vectors
    ori     t0, t0, 1
    csrw    mtvec, t0

    /* No interrupts until the kernel asks for them. */
    csrw    mie, zero
    csrci   mstatus, 0x8

    la      sp, __kernel_stack_top
    csrw    mscratch, zero

    /* Clear .bss so C code sees zeroed statics. */
    la      t0, __bss_start
    la      t1, __bss_end
1:
    bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
    call    kmain
    /* kmain must not return; spin if it does. */
3:
    wfi
    j       3b

    /*
     * An exception enters at the table's base and interrupt n at base + 4n.
     * trap_entry reads mcause either way, so every entry jumps there.
     * The ESP32-C6 takes only the top 24 bits of the base, hence the alignment,
     * and 32 entries cover every interrupt either board raises.
     */
    .section .text, "ax"
    .balign 256
trap_vectors:
    .option push
    .option norvc
    .rept 32
    j       trap_entry
    .endr
    .option pop

    .balign 4
    .globl trap_entry
trap_entry:
    /* sp <- frame pointer (user trap) or zero (kernel trap); mscratch <- sp. */
    csrrw   sp, mscratch, sp
    beqz    sp, kernel_trap

    sw      x1,  1*4(sp)
    sw      x3,  3*4(sp)
    sw      x4,  4*4(sp)
    sw      x5,  5*4(sp)
    sw      x6,  6*4(sp)
    sw      x7,  7*4(sp)
    sw      x8,  8*4(sp)
    sw      x9,  9*4(sp)
    sw      x10, 10*4(sp)
    sw      x11, 11*4(sp)
    sw      x12, 12*4(sp)
    sw      x13, 13*4(sp)
    sw      x14, 14*4(sp)
    sw      x15, 15*4(sp)
    sw      x16, 16*4(sp)
    sw      x17, 17*4(sp)
    sw      x18, 18*4(sp)
    sw      x19, 19*4(sp)
    sw      x20, 20*4(sp)
    sw      x21, 21*4(sp)
    sw      x22, 22*4(sp)
    sw      x23, 23*4(sp)
    sw      x24, 24*4(sp)
    sw      x25, 25*4(sp)
    sw      x26, 26*4(sp)
    sw      x27, 27*4(sp)
    sw      x28, 28*4(sp)
    sw      x29, 29*4(sp)
    sw      x30, 30*4(sp)
    sw      x31, 31*4(sp)

    /* The user sp is in mscratch; take it and mark "in kernel". */
    csrrw   t0, mscratch, zero
    sw      t0, 2*4(sp)

    csrr    t0, mepc
    sw      t0, FRAME_MEPC(sp)
    csrr    t0, mcause
    sw      t0, FRAME_MCAUSE(sp)
    csrr    t0, mtval
    sw      t0, FRAME_MTVAL(sp)

    mv      a0, sp
    la      sp, __kernel_stack_top
    call    trap_handler
    /* Fall through into trap_return with the frame to resume in a0. */

    .globl trap_return
trap_return:
    csrw    mscratch, a0
    mv      sp, a0

    lw      t0, FRAME_MEPC(sp)
    csrw    mepc, t0

    lw      x1,  1*4(sp)
    lw      x3,  3*4(sp)
    lw      x4,  4*4(sp)
    lw      x5,  5*4(sp)
    lw      x6,  6*4(sp)
    lw      x7,  7*4(sp)
    lw      x8,  8*4(sp)
    lw      x9,  9*4(sp)
    lw      x10, 10*4(sp)
    lw      x11, 11*4(sp)
    lw      x12, 12*4(sp)
    lw      x13, 13*4(sp)
    lw      x14, 14*4(sp)
    lw      x15, 15*4(sp)
    lw      x16, 16*4(sp)
    lw      x17, 17*4(sp)
    lw      x18, 18*4(sp)
    lw      x19, 19*4(sp)
    lw      x20, 20*4(sp)
    lw      x21, 21*4(sp)
    lw      x22, 22*4(sp)
    lw      x23, 23*4(sp)
    lw      x24, 24*4(sp)
    lw      x25, 25*4(sp)
    lw      x26, 26*4(sp)
    lw      x27, 27*4(sp)
    lw      x28, 28*4(sp)
    lw      x29, 29*4(sp)
    lw      x30, 30*4(sp)
    lw      x31, 31*4(sp)
    lw      x2,  2*4(sp)
    mret

kernel_trap:
    /* A trap while the kernel runs is a kernel bug; report and halt. */
    la      sp, __kernel_stack_top
    call    kernel_trap_panic
