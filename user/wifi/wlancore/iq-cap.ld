/* Where iq-cap.c goes in the WLAN processor's RAM, its CODE, in RAM the firmware had cleared; its first 64 bytes, the vectors, go to 0 as well. */
SECTIONS
{
    . = 0x20000; .text : { KEEP(*(.vectors)) . = ALIGN(0x40); *(.text*) *(.rodata*) }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.data*) *(.bss*) }
}
ASSERT(SIZEOF(.text) <= 0x1000, "iq-cap: past the 4 KiB kept for it")
