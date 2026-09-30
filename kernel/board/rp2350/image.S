/*
 * The block RP2350's bootrom looks for before it runs an image: see the datasheet, "Image definitions".
 * The kernel linker script puts .board_header at RAM_BASE, the start of the region
 * the bootrom is told to search when it reboots into a RAM image,
 * so it finds this within its first 4 KiB, as it must.
 * One block, linking to itself, holding two items:
 * the image type, an executable for RP2350's RISC-V cores or for its Arm ones in the Secure state,
 * and where it starts.
 * On RISC-V that is the entry point, _start, with a stack pointer the kernel sets again at once;
 * on Arm it is the vector table, which the bootrom points VTOR at and takes the stack pointer and _start from.
 * Values from the pico-sdk's boot/picobin.h.
 */

#define BLOCK_MARKER_START 0xffffded3
#define BLOCK_MARKER_END   0xab123579
#define ITEM_IMAGE_TYPE    0x42
#define ITEM_VECTOR_TABLE  0x03
#define ITEM_ENTRY_POINT   0x44
#define ITEM_LAST          0xff
/* An executable, type 1, for RP2350, chip 1 at bit 12: for the RISC-V CPU, 1 at bit 8, or for Arm, 0, Secure, 2 at bit 4. */
#define IMAGE_TYPE_EXE_RISCV_RP2350 0x1101
#define IMAGE_TYPE_EXE_ARM_S_RP2350 0x1021

    .section .board_header, "a"
    .p2align 2
block:
    .word   BLOCK_MARKER_START
    .byte   ITEM_IMAGE_TYPE, 1
#ifdef __riscv
    .hword  IMAGE_TYPE_EXE_RISCV_RP2350
    .byte   ITEM_ENTRY_POINT, 3, 0, 0
    .word   _start
    .word   __kernel_stack_top
#else
    .hword  IMAGE_TYPE_EXE_ARM_S_RP2350
    .byte   ITEM_VECTOR_TABLE, 2, 0, 0
    .word   trap_vectors
#endif
last:
    .byte   ITEM_LAST
    .hword  (last - block - 4) / 4 /* the words of the items before this one */
    .byte   0
    .word   0                      /* the next block is this one */
    .word   BLOCK_MARKER_END
