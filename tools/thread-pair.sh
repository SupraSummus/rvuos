#!/bin/sh
# IEEE 802.15.4 between the two boards, user/thread/: the ESP32-C6's user/radio/esp32c6/prog/i154.c on a channel,
# 25 unless CHANNEL names one,
# and the nRF52840 once the C6's log says its driver listens.
# Each side fails if it heard nothing of the other, or none of its own frames was acknowledged.
# The nRF52840's exchange lasts RUN seconds, 30 unless named, and the C6's run 15 more, as it starts some seconds before.
# STREAM has each send the other that many frames a second besides the pings and pongs, so that the two contend;
# the C6 a few fewer, for a period at least a tick longer, as a timer's period is whole ticks,
# so that the two streams' phase turns and every run sees them contend alike.
# THREAD_CONFIG names a file of the nRF52840's other keys, power= among them, see user/thread/root.c.
# The C6's port is PORT, or the first Espressif serial port by its USB id; its log is build/thread-pair-c6.log.
#
# Usage: [CHANNEL=n] [RUN=s] [STREAM=n] [THREAD_CONFIG=file] tools/thread-pair.sh
set -u
port=${PORT:-$(ls /dev/serial/by-id/*Espressif* 2>/dev/null | head -n 1)}
if [ -z "$port" ]; then
    echo "thread-pair: no ESP32-C6 found; name its port with PORT=" >&2
    exit 1
fi
make BOARD=nrf52840 build/nrf52840/kernel-thread.elf >/dev/null || exit 1
make BOARD=esp32c6 build/esp32c6/kernel-wifi-esp32c6.bin build/esp32c6/radio-i154.bin >/dev/null || exit 1
channel=${CHANNEL:-25}
run=${RUN:-30}
stream=${STREAM:-0}
period_ms=$(( stream > 0 ? (1000 + stream - 1) / stream : 0 ))
c6_stream=$(( stream > 0 ? 1000 / (period_ms + 1) : 0 ))
conf=$(mktemp)
nrf_conf=$(mktemp)
log=build/thread-pair-c6.log
printf 'channel=%s\nstream=%s\nrun=%s\n' "$channel" "$c6_stream" $((run + 15)) > "$conf"
printf 'channel=%s\nstream=%s\nrun=%s\n' "$channel" "$stream" "$run" > "$nrf_conf"
if [ -n "${THREAD_CONFIG:-}" ]; then
    cat "$THREAD_CONFIG" >> "$nrf_conf"
fi
make BOARD=esp32c6 PORT="$port" radio PROG=i154 RADIO_CONFIG="$conf" > "$log" 2>&1 &
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
echo "thread-pair: the nRF52840's run ended with $nrf, the ESP32-C6's with $c6s"
[ "$nrf" -eq 0 ] && [ "$c6s" -eq 0 ]
