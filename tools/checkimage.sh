#!/bin/sh
# Refuse to flash an image that is not the one just built from this tree.
#
#   tools/checkimage.sh <build-dir> [expect-marker]
#
# Checks, in order: the UF2 exists; it is newer than every file that shapes
# it (sketch sources, web sources, buildweb.py, Makefile); it carries the firmware banner and
# the MINIWORLD_VERSION string from Version.h; its size is inside the band
# a real build of this sketch lands in; and, when a marker is given, that
# string is present (used for the MINIWORLD_ERASE_NET build). Prints the
# build stamp it found so the boot banner can be compared against it.
#
# Exit 0 only when everything holds. This is the guard that would have
# stopped a stale image from a foreign cache directory reaching the board.
#
# Invector Embedded Systems AB

set -u
dir=${1:?build dir}
marker=${2:-}
here=$(cd "$(dirname "$0")/.." && pwd)
uf2="$dir/miniWorld_LightingController.ino.uf2"
elf="$dir/miniWorld_LightingController.ino.elf"

fail() { echo "checkimage: $*" >&2; exit 1; }

[ -f "$uf2" ] || fail "no image at $uf2 (run make compile first)"
[ -f "$elf" ] || fail "no elf at $elf"

# Only files that shape the image count: sources in the sketch folder, the
# web sources and their build script, and the Makefile. Documentation and
# the flashing tools do not.
newer=$(find "$here/miniWorld_LightingController" "$here/web" \
          -type f -newer "$uf2" \
          \( -name '*.h' -o -name '*.cpp' -o -name '*.c' -o -name '*.ino' -o -name '*.pio' \
             -o -name '*.html' -o -name '*.css' -o -name '*.js' \) 2>/dev/null | head -5)
for f in "$here/tools/buildweb.py" "$here/Makefile"; do
    [ "$f" -nt "$uf2" ] && newer="$newer $f"
done
[ -z "$newer" ] || fail "image is older than: $(echo "$newer" | tr '\n' ' ')"

version=$(sed -n 's/^#define MINIWORLD_VERSION *"\([^"]*\)".*/\1/p' "$here/miniWorld_LightingController/Version.h")
[ -n "$version" ] || fail "cannot read MINIWORLD_VERSION from Version.h"

strings "$elf" > "$dir/.strings" || fail "strings failed on $elf"
grep -q 'miniWorld lighting controller %s (%s)' "$dir/.strings" || fail "image lacks the firmware banner: not this sketch"
grep -qx "$version" "$dir/.strings" || fail "image lacks version string $version"
stamp=$(grep -E '^[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}$' "$dir/.strings" | head -1)
[ -n "$stamp" ] || fail "image lacks a build stamp"

size=$(stat -c %s "$uf2")
[ "$size" -ge 400000 ] || fail "image is only $size bytes; a build of this sketch is above 400000"
[ "$size" -le 2000000 ] || fail "image is $size bytes, larger than any build of this sketch"

if [ -n "$marker" ]; then
    grep -q "$marker" "$dir/.strings" || fail "image lacks the expected marker '$marker'"
fi

echo "checkimage: ok, $size bytes, version $version, build $stamp${marker:+, marker present}"
echo "$stamp" > "$dir/.stamp"
