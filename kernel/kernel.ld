/*
 * Kernel image layout for QEMU virt, RV32.
 *
 * RAM starts at 0x80000000 and QEMU enters there in machine mode.
 * The kernel owns the first megabyte.
 * The user program's code is placed at USER_CODE_BASE
 * and its data and stack region lives at USER_DATA_BASE;
 * both addresses are shared with the user linker script
 * and with the PMP setup in the kernel.
 */

ENTRY(_start)

RAM_BASE       = 0x80000000;
USER_CODE_BASE = 0x80100000;
USER_CODE_SIZE = 0x10000;

KERNEL_STACK_SIZE = 16K;
BOOT_POOL_SIZE    = 4K;
/* The kernel's log ends where the user code begins; kernel.h has KLOG_BASE. */
KLOG_REGION_SIZE  = 32 + 4K;

SECTIONS
{
    . = RAM_BASE;

    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    .stack (NOLOAD) : {
        . = ALIGN(16);
        __kernel_stack_bottom = .;
        . += KERNEL_STACK_SIZE;
        __kernel_stack_top = .;
    }

    /* Memory for the root task's kernel objects; see boot.c. */
    .bootpool (NOLOAD) : {
        . = ALIGN(16);
        __boot_pool_start = .;
        . += BOOT_POOL_SIZE;
        __boot_pool_end = .;
    }

    ASSERT(. <= USER_CODE_BASE - KLOG_REGION_SIZE, "kernel does not fit below its log")

    . = USER_CODE_BASE;
    .user_code : {
        KEEP(*(.user_code))
    }
    ASSERT(. <= USER_CODE_BASE + USER_CODE_SIZE, "user code does not fit its region")

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note.GNU-stack)
    }
}
