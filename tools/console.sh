#!/bin/sh
# Read the board's USB console until interrupted. Finds the board by USB
# identity unless a port is given, and asserts DTR so the arduino-pico
# core actually sends (it drops output while no host is listening).
#
#   tools/console.sh [/dev/ttyACMn]
#
# Invector Embedded Systems AB

set -u
here=$(cd "$(dirname "$0")/.." && pwd)
port=${1:-}
if [ -z "$port" ]; then
    found=$("$here/tools/findboard.sh") || exit 1
    case "$found" in
        serial*) port=${found#* };;
        *) echo "console: the board is in BOOTSEL, not running firmware" >&2; exit 1;;
    esac
fi
echo "console: $port (Ctrl-C to stop)"
exec python3 - "$port" <<'PY'
import serial, sys, time
port = sys.argv[1]
while True:
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            s.dtr = True; s.rts = True
            while True:
                d = s.read(4096)
                if d:
                    sys.stdout.write(d.decode("ascii", "replace")); sys.stdout.flush()
    except KeyboardInterrupt:
        sys.exit(0)
    except Exception:
        time.sleep(0.5)
PY
