#!/bin/sh
# Send the image in a build directory to a running board over WiFi, with
# the same checks before and after as tools/flash.sh. Called by
# `make ota HOST=...`; usable by hand.
#
#   tools/ota.sh <build-dir> <host>
#   env: OTA_PASSWORD   the GUI password, when the device has one
#
# Before: tools/checkimage.sh must pass (fresh image, right sketch, sane
# size). Then GET /api/system/firmware says whether the device has room.
# The .bin is POSTed with its MD5 and build stamp in two headers; the
# device stores it, checks both, scans the bytes for the banner and the
# stamp, and reboots into it through the arduino-pico OTA boot stage.
#
# After: /api/system/status is polled for up to 90 s until its build
# stamp equals the image's. Sending the image that already runs is a
# success too: the boot stage skips identical blocks and the stamp
# matches at once.
#
# There is no rollback. An image that passes every check but breaks the
# network is recovered over USB with make upload.
#
# Invector Embedded Systems AB

set -u
dir=${1:?build dir}
host=${2:?host, an IP address or miniworld.local}
here=$(cd "$(dirname "$0")/.." && pwd)
bin="$dir/miniWorld_LightingController.ino.bin"

fail() { echo "ota: $*" >&2; exit 1; }

# curl with the device password when one is set. It goes in through a
# config on stdin (curl -K -), so it is never on any command line, not
# even curl's; a double quote or backslash in it is escaped for curl's
# config syntax. stdin is free: the image goes in as @file.
curl_auth() {
    if [ -n "${OTA_PASSWORD:-}" ]; then
        printf 'user = ":%s"\n' "$(printf '%s' "$OTA_PASSWORD" | sed 's/[\\"]/\\&/g')" \
            | curl -K - "$@"
    else
        curl "$@"
    fi
}

json_field() {   # json_field <name> reads stdin, prints the value of "name"
    sed -n 's/.*"'"$1"'": *"\{0,1\}\([^",}]*\)"\{0,1\}.*/\1/p'
}

"$here/tools/checkimage.sh" "$dir" || exit 1
stamp=$(head -1 "$dir/.stamp")
[ -f "$bin" ] || fail "no image at $bin"
md5=$(md5sum "$bin" | cut -d' ' -f1)
size=$(stat -c %s "$bin")

pre=$(curl_auth -s -m 8 "http://$host/api/system/firmware") \
    || fail "no answer from http://$host/api/system/firmware"
max=$(printf '%s' "$pre" | json_field maxSize)
free=$(printf '%s' "$pre" | json_field fsFree)
case "$pre" in *unauthori[sz]ed*) fail "device answered 401: set OTA_PASSWORD to the GUI password";; esac
[ -n "$max" ] || fail "unexpected answer from the device: $pre"
[ "$size" -le "$max" ] || fail "image is $size bytes, the device takes at most $max ($free bytes free)"
echo "ota: sending $size bytes, build $stamp, to $host ($free bytes free)"

# Expect: is emptied because the device does not answer 100-continue and
# curl would otherwise wait a second before sending the body.
post_image() {
    curl_auth -s -m 180 -w '\n%{http_code}' -X POST --data-binary "@$bin" \
        -H 'Content-Type: application/octet-stream' \
        -H "X-Firmware-MD5: $md5" -H "X-Firmware-Build: $stamp" -H 'Expect:' \
        "http://$host/api/system/firmware"
}

# status_is <stamp> <seconds>: polls /api/system/status until its build
# equals the stamp or the time is up. Returns 0 on a match, with the
# last build seen in $build.
status_is() {
    end=$(( $(date +%s) + $2 ))
    build=""
    while [ "$(date +%s)" -lt "$end" ]; do
        build=$(curl_auth -s -m 5 "http://$host/api/system/status" | json_field build)
        [ "$build" = "$1" ] && return 0
        sleep 2
    done
    return 1
}

resp=$(post_image)
code=$(printf '%s\n' "$resp" | tail -1)
body=$(printf '%s\n' "$resp" | sed '$d')
if [ -z "$code" ] || [ "$code" = "000" ]; then
    # No answer at all. Once on the bench the POST right after a dropped
    # upload got none although the board was fine, and a lost reply can
    # also mean the 200 itself was lost while the board reboots. So look
    # for the stamp first, and only then send the image once more; a
    # second body into a board that is mid-reboot helps nobody.
    echo "ota: no answer from the device, checking whether it took the image"
    if status_is "$stamp" 30; then
        echo "ota: verified, board at $host runs build $build"
        exit 0
    fi
    echo "ota: sending once more"
    resp=$(post_image)
    code=$(printf '%s\n' "$resp" | tail -1)
    body=$(printf '%s\n' "$resp" | sed '$d')
    if [ -z "$code" ] || [ "$code" = "000" ]; then
        fail "no answer from the device at $host on two attempts; if it stays unreachable, recover over USB with make upload"
    fi
fi
[ "$code" != "401" ] || fail "device answered 401: set OTA_PASSWORD to the GUI password"
[ "$code" = "200" ] || fail "device answered $code: $body"
echo "ota: device staged the image: $body"

# The reboot, the copy (a 253 kB image is about a second) and the ESP
# bring-up. The stamp is the test, not a before/after difference, so
# re-sending the running image passes too.
sleep 3
if status_is "$stamp" 90; then
    echo "ota: verified, board at $host runs build $build"
    exit 0
fi
fail "board at $host did not report build '$stamp' within 90 s (last seen '${build:-nothing}'); if it stays unreachable, recover over USB with make upload"
