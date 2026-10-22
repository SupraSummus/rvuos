/*
 * A radio program's image, see drv.h: code and constants in the window onto flash, from its header on,
 * and data, the zeroed data and the heap in the program's block of RAM, short of the ROM's data at its top.
 * .data's image follows the constants in flash, where the entry copies it from.
 * The block is the station's unless the build names another, DRV_RAM_BASE and DRV_RAM_SIZE;
 * the header tells the root task which, drv_ram_start and drv_ram_size.
 */

#include "drv.h"

MEMORY
{
    flash (rx) : ORIGIN = 0x42000000, LENGTH = DRV_IMAGE_MAX
    ram (rw)   : ORIGIN = DRV_RAM_BASE, LENGTH = DRV_HEAP_END - DRV_RAM_BASE
}

ENTRY(drv_main)

SECTIONS
{
    .header : { KEEP(*(.header)) } > flash
    .text : {
        *(.text .text.*)
        *(.iram1 .iram1.*)
        *(.wifi0iram .wifi0iram.* .wifirxiram .wifirxiram.* .wifislpiram .wifislpiram.*)
        *(.wifislprxiram .wifislprxiram.* .wifiextrairam .wifiextrairam.* .wifiorslpiram .wifiorslpiram.*)
    } > flash
    .rodata : {
        *(.rodata .rodata.* .srodata .srodata.*)
        *(.rodata_wlog_error.* .rodata_wlog_warning.* .rodata_wlog_info.* .rodata_wlog_debug.* .rodata_wlog_verbose.*)
    } > flash
    .data : ALIGN(8) {
        drv_data_start = .;
        *(.data .data.* .sdata .sdata.* .dram1 .dram1.*)
        . = ALIGN(8);
        drv_data_end = .;
    } > ram AT > flash
    drv_data_load = LOADADDR(.data);
    .bss (NOLOAD) : ALIGN(8) {
        drv_bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(8);
        drv_bss_end = .;
    } > ram
    drv_heap_start = .;
    drv_heap_end = DRV_HEAP_END;
    drv_ram_start = DRV_RAM_BASE;
    drv_ram_size = DRV_RAM_SIZE;
    /DISCARD/ : { *(.comment .note .note.* .riscv.attributes .eh_frame*) }
}
