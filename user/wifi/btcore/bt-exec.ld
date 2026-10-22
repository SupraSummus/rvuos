/*
 * Where the executor goes, bt-exec.c: where the stub goes, free RAM beside the patch that the core fetches code from;
 * the two never share a run, the executor running on a core parked before the patch's own code.
 */
#include "bt-map.h"

SECTIONS
{
    . = BT_STUB; .text : { *(.start) *(.text*) *(.rodata*) }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) *(.data*) *(.bss*) }
}
/*
 * It runs past BT_MBOX, into the band the firmware uses, as no firmware runs on the core parked;
 * how far that RAM takes code is not known, so the executor stays within 0x200 bytes of it, to 0xd0f00.
 */
ASSERT(SIZEOF(.text) <= 0x200, "bt-exec: past the 0x200 bytes it is kept within")
