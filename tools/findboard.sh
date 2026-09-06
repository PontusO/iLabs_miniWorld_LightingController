#!/bin/sh
# Locate the Challenger NB 2040 WiFi to flash, by USB device id, never by
# a /dev/ttyACM number.
#
#   tools/findboard.sh            prints one of
#                                   serial /dev/ttyACMn
#                                   bootsel /path/to/RPI-RP2
#                                 and exits 0, or explains and exits 1
#
# The scan starts from the USB device list in sysfs and matches idVendor
# and idProduct, then derives the tty (running firmware, 2e8a:100d, cdc_acm
# on interface 0) or the RPI-RP2 drive (bootrom, 2e8a:0003) from that
# device. The ids come from the board's upload_port list in the core's
# boards.txt. Exactly one board must be present; two is an error, so the
# wrong desk neighbour never gets flashed. A bootrom device that is not
# mounted is reported as such instead of being missed.
#
# BOARD_VID and BOARD_PID can be overridden for testing against another
# board on the bus.
#
# Invector Embedded Systems AB

set -u
vid=${BOARD_VID:-2e8a}
pid=${BOARD_PID:-100d}
boot_pid=${BOARD_BOOT_PID:-0003}

serials=""
boots=""
unmounted=""
for dev in /sys/bus/usb/devices/*; do
    [ -f "$dev/idVendor" ] || continue
    [ "$(cat "$dev/idVendor")" = "$vid" ] || continue
    product=$(cat "$dev/idProduct")
    name=$(basename "$dev")
    if [ "$product" = "$pid" ]; then
        tty=""
        for iface in "$dev"/"$name":*; do
            [ -d "$iface/tty" ] || continue
            tty=$(ls "$iface/tty" | head -1)
            [ -n "$tty" ] && break
        done
        if [ -n "$tty" ] && [ -e "/dev/$tty" ]; then
            serials="$serials /dev/$tty"
        else
            echo "findboard: $name is the board (id $vid:$product) but has no tty yet" >&2
        fi
    elif [ "$product" = "$boot_pid" ]; then
        mount=$(ls -d /media/*/RPI-RP2 /run/media/*/RPI-RP2 2>/dev/null | head -1)
        if [ -n "$mount" ]; then
            boots="$boots $mount"
        else
            unmounted="$unmounted $name"
        fi
    fi
done

nser=$(echo $serials | wc -w)
nboot=$(echo $boots | wc -w)

if [ "$nser" -eq 1 ] && [ "$nboot" -eq 0 ]; then
    echo "serial$serials"
    exit 0
fi
if [ "$nboot" -eq 1 ] && [ "$nser" -eq 0 ]; then
    echo "bootsel$boots"
    exit 0
fi
if [ -n "$unmounted" ] && [ "$nser" -eq 0 ] && [ "$nboot" -eq 0 ]; then
    echo "findboard: an RP2040 in BOOTSEL is on the bus ($unmounted) but its RPI-RP2 drive is not mounted." >&2
    echo "findboard: mount it (a file manager usually does), or pass PORT=/path/to/RPI-RP2 once it is." >&2
    exit 1
fi
if [ "$nser" -eq 0 ] && [ "$nboot" -eq 0 ]; then
    echo "findboard: no Challenger NB 2040 WiFi on USB (id $vid:$pid) and no RPI-RP2 drive (id $vid:$boot_pid)." >&2
    echo "findboard: if the board is stuck, hold BOOT and tap RESET so it shows up as RPI-RP2." >&2
    exit 1
fi
echo "findboard: more than one candidate, refusing to guess: serial[$serials ] bootsel[$boots ]" >&2
echo "findboard: unplug the others or pass PORT=/dev/ttyACMn explicitly." >&2
exit 1
