#!/bin/sh
# Flash the image in a build directory to the board, with checks before
# and after. Called by `make upload`; usable by hand.
#
#   tools/flash.sh <build-dir> [port-or-drive] [expect-marker]
#
# Before: tools/checkimage.sh must pass (fresh image, right sketch, sane
# size, optional marker). Then the board is found by USB identity unless a
# port or an RPI-RP2 path is given. A busy serial port is reported with the
# process that holds it, and nothing is sent. A board in BOOTSEL gets the
# UF2 copied straight onto the drive; a running board gets the 1200 baud
# reset through arduino-cli, which then copies to the drive itself.
#
# After: the USB console is read for up to 30 s and the boot banner's build
# stamp must equal the stamp found in the image. A mismatch is reported as
# a failure so a wrong image can never pass unnoticed again.
#
# Invector Embedded Systems AB

set -u
dir=${1:?build dir}
target=${2:-}
marker=${3:-}
here=$(cd "$(dirname "$0")/.." && pwd)
uf2="$dir/miniWorld_LightingController.ino.uf2"
cli=${ARDUINO_CLI:-$HOME/bin/arduino-cli}
fqbn=${FQBN:-rp2040:rp2040:challenger_nb_2040_wifi:flash=8388608_1048576}

fail() { echo "flash: $*" >&2; exit 1; }

"$here/tools/checkimage.sh" "$dir" "$marker" || exit 1
stamp=$(cat "$dir/.stamp")

if [ -z "$target" ]; then
    found=$("$here/tools/findboard.sh") || exit 1
    kind=${found%% *}; target=${found#* }
elif [ -d "$target" ]; then
    kind=bootsel
else
    kind=serial
fi

if [ "$kind" = serial ]; then
    holder=$(fuser "$target" 2>/dev/null | tr -s ' ')
    if [ -n "$holder" ]; then
        for p in $holder; do
            echo "flash: $target is held open by pid $p: $(ps -o comm= -p "$p" 2>/dev/null)" >&2
        done
        fail "close that program (a serial monitor, most likely) and retry"
    fi
    echo "flash: $target, image build $stamp"
    "$cli" upload --fqbn "$fqbn" -p "$target" --input-dir "$dir" "$here/miniWorld_LightingController" || fail "arduino-cli upload failed"
else
    echo "flash: BOOTSEL drive $target, image build $stamp"
    cp "$uf2" "$target/NEW.UF2" && sync || fail "copy to $target failed"
fi

# Post-flash verification through the USB console. The board re-enumerates
# after the copy; wait for it, then read until the banner appears.
echo "flash: waiting for the board to come back and print its banner"
port=""
i=0
while [ $i -lt 30 ]; do
    found=$("$here/tools/findboard.sh" 2>/dev/null) && case "$found" in serial*) port=${found#* }; break;; esac
    sleep 1; i=$((i+1))
done
[ -n "$port" ] || fail "board did not re-enumerate as a serial device within 30 s"

banner=$(python3 - "$port" <<'PY'
import serial, sys, time
port = sys.argv[1]
end = time.time() + 30
buf = b""
while time.time() < end:
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            s.dtr = True; s.rts = True
            while time.time() < end:
                buf += s.read(4096)
                if b"miniWorld lighting controller" in buf:
                    line = [l for l in buf.decode("ascii", "replace").splitlines() if "miniWorld lighting controller" in l]
                    if line:
                        print(line[0]); sys.exit(0)
    except Exception:
        time.sleep(0.5)
sys.exit(1)
PY
) || fail "no boot banner on $port within 30 s; the image may not be running"

case "$banner" in
    *"($stamp)"*) echo "flash: verified, board runs build $stamp";;
    *) fail "banner '$banner' does not carry build stamp '$stamp': the board is not running this image";;
esac
