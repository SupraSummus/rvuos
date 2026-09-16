/*
 * The single user program of the skeleton.
 * Code and read-only data live in the read-execute region,
 * zero-initialised data and the stack in the read-write region.
 * Initialised data is forbidden for now
 * because nobody would copy it into the data region.
 */

ENTRY(_start)

USER_CODE_BASE = 0x80100000;
USER_CODE_SIZE = 0x10000;
USER_DATA_BASE = 0x80200000;
USER_DATA_SIZE = 0x10000;

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

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }
    ASSERT(SIZEOF(.data) == 0, "user programs may not have initialised data yet")
    ASSERT(. <= USER_CODE_BASE + USER_CODE_SIZE, "user code does not fit its region")

    . = USER_DATA_BASE;

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    ASSERT(. <= USER_DATA_BASE + USER_DATA_SIZE, "user data does not fit its region")

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note.GNU-stack)
    }
}
