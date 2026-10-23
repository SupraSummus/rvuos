/*
 * Where our code for the Bluetooth core goes; see bt-map.h and bt-stub.S. Run through the preprocessor with the
 * header, so the addresses live there alone.
 */
#include "bt-map.h"

SECTIONS
{
    /* In increasing address order, since the linker lays the file out as they come. */
    . = BT_CALLEE;     .callee   : { *(.callee) }
    . = BT_POINT;      .takeover : { *(.takeover) }
    . = BT_POINT_NEXT; .callee2  : { *(.callee2) }
    . = BT_STUB;       .text     : { *(.text) }
    . = BT_MBOX;       .mbox     : { *(.mbox) }
    . = BT_MEASURE;    .measure  : { *(.measure) }
    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
