/*
 * The single user program of the skeleton.
 * Code and read-only data live in the read-execute region,
 * data and the stack in the read-write region.
 * Initialised data runs at the base of the data region and is loaded right behind the read-only data,
 * so the flat image is the code region alone, and start.S copies it over before main.
 * The Makefile runs this through the preprocessor with the board's board.h;
 * see kernel/layout.h for the addresses.
 */

#include "layout.h"

ENTRY(_start)

/* For the escape suite, see user/escape-*.c: where the kernel starts, which no program may reach. */
__kernel_start = RAM_BASE;

SECTIONS
{
    . = USER_CODE_BASE;

    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    . = ALIGN(4);
    __data_load = .;

    . = USER_DATA_BASE;

    .data : AT(__data_load) {
        __data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
    }
    ASSERT(__data_load + SIZEOF(.data) <= USER_CODE_BASE + USER_CODE_SIZE, "user code does not fit its region")

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    ASSERT(. <= USER_DATA_BASE + USER_DATA_SIZE, "user data does not fit its region")

    /* Nothing unwinds, and ARM's compiler marks every function as not unwinding all the same. */
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note.GNU-stack .ARM.exidx* .ARM.extab*)
    }
}
