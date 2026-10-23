/*
 * Where our code for the Bluetooth core goes, and the mailbox it serves; one source for bt-stub.S, bt-stub.ld and user/wifi/bt.c.
 * The linker script is run through the preprocessor with this, the assembly includes it, and user/wifi/bt.c includes it, so the
 * addresses live here alone. See user/wifi/BT.md.
 */
#define BT_CALLEE     0x001b1e6   /* the address the point's own first instruction calls */
#define BT_POINT_NEXT 0x000d0204  /* the point's second instruction */
#define BT_POINT      0x000d0200  /* the ROM's branch into the vendor patch, which we take over */
#define BT_STUB       0x000d0d00  /* free RAM beside the patch: the path every command runs */
/*
 * The mailbox, just above the band the firmware uses beside the patch (0xd0d9a on), and above it a second piece of
 * free RAM for the timed run, which is longer but reached on a measure alone. The patch's own writes end at 0xd0cd4
 * and a word here takes the host's writes. It used to sit in the controller's RAM at 0x2003f8, where the counter at
 * +0x18 landed on 0x200410 -- a word the firmware itself uses -- so our runs were mixed into the firmware's own.
 */
#define BT_MBOX       0x000d0e00  /* BT_STUB + 0x100, so the common path has room up to the band */
#define BT_MEASURE    0x000d0e24  /* just above the mailbox, below the band the firmware uses at ~0xd0e78 */

/* The mailbox's words, in bytes from BT_MBOX. */
#define MB_MAGIC      0x00
#define MB_CMD        0x04
#define MB_ARG0       0x08
#define MB_ARG1       0x0c
#define MB_RES        0x10
#define MB_DONE       0x14
#define MB_RUNS       0x18
#define MB_POKE_AT    0x1c        /* the register the poke goes to; 0 means no poke */
#define MB_POKE_VAL   0x20        /* what goes there, written on every call of the point */
#define MB_ARMED      0x42424242  /* written to MB_MAGIC to arm a command */

/* Commands, in MB_CMD. */
#define MB_READ       1           /* res = the word at arg0 */
#define MB_WRITE      2           /* arg1 goes to arg0 */
#define MB_MEASURE    3           /* arg0 counts up arg1 times (0 means 64); res = the counter's ticks */
#define MB_TOUCH      4           /* arg0's own value goes back to arg0 arg1 times; res = the counter's ticks */
