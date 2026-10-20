/*
 * The bytes of our code for the Bluetooth core, built from btcore/bt-stub.S and linked where the core sees it;
 * bt.c uploads them. Guarded, so an image without WIFI_BT carries nothing of the experiment.
 */
#ifdef WIFI_BT
    .section .rodata
    .globl bt_takeover
bt_takeover:
    .incbin "build/cyw43/bt-takeover.bin"
    .globl bt_code
bt_code:
    .incbin "build/cyw43/bt-stub.bin"
    .globl bt_code_end
bt_code_end:
    .globl bt_measure
bt_measure:
    .incbin "build/cyw43/bt-measure.bin"
    .globl bt_measure_end
bt_measure_end:
#endif
