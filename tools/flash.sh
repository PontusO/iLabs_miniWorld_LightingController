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
stamp=$(head -1 "$dir/.stamp")

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
while [ $i -lt 300 ]; do
    found=$("$here/tools/findboard.sh" 2>/dev/null) && case "$found" in serial*) port=${found#* }; break;; esac
    sleep 0.1; i=$((i+1))
done
[ -n "$port" ] || fail "board did not re-enumerate as a serial device within 30 s"

# Read the banner, then keep listening for the network state so the
# address ends up in the make output. Stops at "net: online" or "net:
# portal", or after 45 s.
log=$(python3 - "$port" <<'PY'
import serial, sys, time
port = sys.argv[1]
end = time.time() + 45
buf = b""
seen_banner = False
while time.time() < end:
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            s.dtr = True; s.rts = True
            while time.time() < end:
                buf += s.read(4096)
                text = buf.decode("ascii", "replace")
                if "miniWorld lighting controller" in text:
                    seen_banner = True
                if seen_banner and ("net: online" in text or "net: portal" in text or "net: nomodule" in text):
                    break
    except Exception:
        time.sleep(0.5)
sys.stdout.write(buf.decode("ascii", "replace"))
sys.exit(0 if seen_banner else 1)
PY
) || true

banner=$(printf '%s\n' "$log" | grep 'miniWorld lighting controller' | head -1)
printf '%s\n' "$log" | grep -E '^(miniWorld|lamps|scene|net|http):' | sed 's/^/flash: board says: /'
ok=0
if [ -n "$banner" ]; then
    while read -r st; do
        case "$banner" in *"($st)"*) ok=1;; esac
    done < "$dir/.stamp"
    [ "$ok" -eq 1 ] || fail "banner '$banner' matches none of the image's build stamps: the board is not running this image"
    echo "flash: verified through the console, board runs this image"
else
    # The banner is printed 1.5 s after boot and USB CDC drops output while
    # no host listens, so it can be missed. Fall back to the API, which
    # reports the same build stamp, once the board says where it is.
    ip=$(printf '%s\n' "$log" | sed -n 's/^net: online [^ ]* \([0-9.]*\).*/\1/p' | tail -1)
    [ -n "$ip" ] || ip=$(python3 - "$port" <<'PY'
import serial, sys, time, re
end = time.time() + 40
buf = b""
try:
    with serial.Serial(sys.argv[1], 115200, timeout=0.5) as s:
        s.dtr = True; s.rts = True
        while time.time() < end:
            buf += s.read(4096)
            m = re.search(rb"net: online \S+ ([0-9.]+)", buf)
            if m:
                print(m.group(1).decode()); break
except Exception:
    pass
PY
)
    [ -n "$ip" ] || fail "no boot banner and no 'net: online' line on $port; cannot verify what the board runs"
    build=$(curl -s -m 8 "http://$ip/api/system/status" | sed -n 's/.*"build":"\([^"]*\)".*/\1/p')
    [ -n "$build" ] || fail "board is at $ip but /api/system/status did not answer; cannot verify"
    while read -r st; do
        [ "$build" = "$st" ] && ok=1
    done < "$dir/.stamp"
    [ "$ok" -eq 1 ] || fail "board at $ip reports build '$build', not this image's"
    echo "flash: verified through the API at $ip, board runs this image (build $build)"
fi
