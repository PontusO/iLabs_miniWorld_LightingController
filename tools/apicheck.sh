#!/bin/sh
# apicheck.sh - curl every route of the miniWorld web API against a running
#               server (the mock, or the real device) and check the status
#               code each one returns.
#
# usage: apicheck.sh [base-url]
# env:   AUTH=user:pass, passed to curl -u when the server requires auth
#
# Invector Embedded Systems AB

BASE=${1:-http://localhost:8080}
FAIL=0

check() {
    m=$1
    p=$2
    expect=$3
    data=$4

    if [ -n "$AUTH" ]; then
        auth_opt="-u"
        auth_val="$AUTH"
    fi

    if [ -n "$data" ]; then
        if [ -n "$AUTH" ]; then
            code=$(curl -s -o /dev/null -w "%{http_code}" -u "$AUTH" -X "$m" \
                -H "Content-Type: application/json" -d "$data" "$BASE$p")
        else
            code=$(curl -s -o /dev/null -w "%{http_code}" -X "$m" \
                -H "Content-Type: application/json" -d "$data" "$BASE$p")
        fi
    else
        if [ -n "$AUTH" ]; then
            code=$(curl -s -o /dev/null -w "%{http_code}" -u "$AUTH" -X "$m" "$BASE$p")
        else
            code=$(curl -s -o /dev/null -w "%{http_code}" -X "$m" "$BASE$p")
        fi
    fi

    printf "%s %s %s\n" "$code" "$m" "$p"

    if [ "$code" != "$expect" ]; then
        FAIL=1
    fi
}

# GET a route and print the body, so a PUT can be checked with what the
# server itself last answered.
fetch() {
    if [ -n "$AUTH" ]; then
        curl -s -u "$AUTH" "$BASE$1"
    else
        curl -s "$BASE$1"
    fi
}

# All twelve buses, the same fixture the mock starts from, so the check
# leaves the configuration where it found it.
LAMPS_BODY='{"busSpeed":400000,"activeLow":false,"rgb":false,"buses":[
{"sx1503":true,"al5887":0},{"sx1503":false,"al5887":2},
{"sx1503":false,"al5887":0},{"sx1503":false,"al5887":0},
{"sx1503":false,"al5887":0},{"sx1503":false,"al5887":0},
{"sx1503":false,"al5887":0},{"sx1503":false,"al5887":0},
{"sx1503":false,"al5887":0},{"sx1503":false,"al5887":0},
{"sx1503":false,"al5887":0},{"sx1503":false,"al5887":0}]}'

check GET  /                  200
check GET  /api/lamps/config  200
check PUT  /api/lamps/config  200 "$LAMPS_BODY"
check GET  /api/lamps/status  200
check POST /api/lamps/probe   200
check POST /api/lamps/test    200 '{"level":128}'
check GET  /api/scene/config  200
check PUT  /api/scene/config  200 "$(fetch /api/scene/config)"
check GET  /api/scene/status  200
check PUT  /api/scene/clock   200 '{"mode":"manual","time":"19:40"}'
check GET  /api/scene/presets 200
check GET  /api/net/status    200
check GET  /api/net/scan      200
check GET  /api/net/config    200
check PUT  /api/net/config    200 '{"ntp":"pool.ntp.org"}'
check GET  /api/system/status 200
if [ "${APICHECK_DESTRUCTIVE:-0}" = "1" ]; then
  check POST /api/system/reboot 200
else
  echo "skip POST /api/system/reboot (set APICHECK_DESTRUCTIVE=1 to run reboot, connect and forget; they change a real device)"
fi
check GET  /nope              404
check GET  /api/net/connect   405
# A captive-portal probe is answered with the redirect that opens the
# sign-in sheet, in any mode.
check GET  /generate_204      302
# Last, because they move the network state machine: connect leaves it
# connecting, forget leaves it in the portal.
if [ "${APICHECK_DESTRUCTIVE:-0}" = "1" ]; then
  check POST /api/net/connect   202 '{"ssid":"x","pass":"y"}'
  check POST /api/net/forget    200
fi

exit $FAIL
