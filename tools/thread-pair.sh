#!/bin/sh
# IEEE 802.15.4 between the two boards, user/thread/: the ESP32-C6 with i154= on a channel, 25 unless CHANNEL names one,
# and the nRF52840 once the C6's log says its driver listens.
# Each side fails if it heard nothing of the other: the C6's driver no pong, the nRF52840 no ping or no acknowledged pong;
# and this fails if none of the C6's pings was acknowledged, which only the C6's last count tells.
# The nRF52840's exchange lasts RUN seconds, 30 unless named, and the C6's run 15 more, as it starts some seconds before.
# THREAD_CONFIG names a file of the nRF52840's other keys, power= and stream= among them, see user/thread/root.c.
# The C6's port is PORT, or the first Espressif serial port by its USB id; its log is build/thread-pair-c6.log.
#
# Usage: [CHANNEL=n] [RUN=s] [THREAD_CONFIG=file] tools/thread-pair.sh
set -u
port=${PORT:-$(ls /dev/serial/by-id/*Espressif* 2>/dev/null | head -n 1)}
if [ -z "$port" ]; then
    echo "thread-pair: no ESP32-C6 found; name its port with PORT=" >&2
    exit 1
fi
make BOARD=nrf52840 build/nrf52840/kernel-thread.elf >/dev/null || exit 1
make BOARD=esp32c6 build/esp32c6/kernel-wifi-esp32c6.bin build/esp32c6/wifi-drv.bin >/dev/null || exit 1
channel=${CHANNEL:-25}
run=${RUN:-30}
conf=$(mktemp)
nrf_conf=$(mktemp)
log=build/thread-pair-c6.log
printf 'i154=%s\nrun=%s\n' "$channel" $((run + 15)) > "$conf"
printf 'channel=%s\nrun=%s\n' "$channel" "$run" > "$nrf_conf"
if [ -n "${THREAD_CONFIG:-}" ]; then
    cat "$THREAD_CONFIG" >> "$nrf_conf"
fi
make BOARD=esp32c6 PORT="$port" wifi-esp32c6 WIFI_CONFIG="$conf" > "$log" 2>&1 &
c6=$!
until grep -q "root: the driver listens" "$log"; do
    if ! kill -0 "$c6" 2>/dev/null; then
        cat "$log"
        rm -f "$conf" "$nrf_conf"
        echo "thread-pair: the ESP32-C6 ended before it listened" >&2
        exit 1
    fi
    sleep 0.5
done
make BOARD=nrf52840 thread THREAD_CONFIG="$nrf_conf"
nrf=$?
wait "$c6"
c6s=$?
rm -f "$conf" "$nrf_conf"
grep "i154: " "$log"
acked=$(grep -o "i154: sent [0-9]*, acknowledged [0-9]*" "$log" | tail -n 1 | sed "s/.* //")
echo "thread-pair: the nRF52840's run ended with $nrf, the ESP32-C6's with $c6s, ${acked:-no} pings acknowledged"
[ "$nrf" -eq 0 ] && [ "$c6s" -eq 0 ] && [ "${acked:-0}" -gt 0 ]
