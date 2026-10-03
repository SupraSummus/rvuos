/*
 * The harness: libphy.a and its glue in one block of RAM, code, data and stack together,
 * which the ROM loads as one flat image; see phytrace.h.
 * Symbols the library takes from the ROM come from ESP-IDF's ROM linker scripts,
 * which the Makefile passes after this one.
 */

#include "phytrace.h"

ENTRY(_start)

SECTIONS
{
    . = PHYBLOB_BASE;

    .text : {
        KEEP(*(.text.header))
        *(.text .text.*)
        *(.iram1 .iram1.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
        *(.dram1 .dram1.*)
    }

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /* The stack takes the rest of the block. */
    __stack_top = PHYBLOB_BASE + PHYBLOB_SIZE;
    ASSERT(__stack_top - __bss_end >= 0x4000, "the harness leaves its stack less than 16 KiB")
    ASSERT(PHYBLOB_BASE + PHYBLOB_SIZE <= 0x4086ad08, "the harness reaches the ROM's buffers")

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note.GNU-stack)
    }
}
