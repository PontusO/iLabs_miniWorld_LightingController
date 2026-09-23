# Firmware upload from the GUI: design

Date: 2026-09-23. Follows `2026-09-23-firmware-update-design.md`, which
put the upload page in a later round. This is that round.

## 1. Goal and scope

A System tab in the web GUI from which a person at the bench sends a
`.bin` from `./build` to the board over WiFi, with the same guarantees
`make ota` gives: the file is checked for identity before a byte is sent,
the device checks it again before it reboots, and the page reports the
build the board came back on.

In scope: the System tab (device status, filesystem headroom, the upload,
a reboot button), the browser-side checks, the upload with progress, the
wait for the board, the mock additions that let all of it run off-target,
and the HANDOFF update after the bench run.

Out of scope: release images from GitHub on a phone (the page will work
there too, but nothing is designed for it), a GUI log or factory reset
(the tab gives them a home for later), any change to the firmware. The
device side is the verified contract from the previous round and is not
touched.

Intended use, in the user's words: bench work, a `.bin` from `./build`,
picked on the laptop next to the board.

## 2. The System tab

A seventh nav entry, `System`, after `Houses`. Two new files,
`web/view-system.js` and `web/view-system.css`, picked up by
`tools/buildweb.py`'s sorted glob without any change to the build script.
`web/index.html` gains the nav line and its header comment says seven
views. The view registers as `App.register("system", { title, mount,
unmount, poll })` like the others and is written against `App.el` and
`App.api`.

Three cards, top to bottom:

**Device.** Firmware version, build stamp, uptime, free heap and whether
the clock is valid, from `GET /api/system/status`. Read on mount and
refreshed by `poll()`, since uptime and heap move. Home keeps its own
firmware line unchanged.

**Firmware.** One line of headroom from `GET /api/system/firmware`:
`1011 kB free, images up to 944 kB`. A file input, a `Send` button, a
progress bar shown only while bytes move, and one status line that
carries every state in words (section 4). The headroom line is re-read
after every upload attempt, so a refusal that deleted a partial file is
visible as free space coming back.

**Reboot.** One button. First press asks `Reboot the controller?` and
turns into `Yes, reboot`; the second press sends `POST /api/system/reboot`
and enters the same wait as the upload, with "the status answers again
with a smaller uptime than before" as its success test, 60 s at most.

## 3. The upload flow

The page does, in the browser, exactly what `tools/ota.sh` does on the
host, in the device's order, so a wrong file is refused before it crosses
the UART:

1. **Read** the chosen file with `FileReader.readAsArrayBuffer`. It is a
   quarter megabyte and stays in memory only; nothing is written to disk
   or to browser storage.
2. **Band.** `size` must satisfy `minSize <= size <= maxSize` from the
   last firmware GET. Refused: `this file is 12 kB; an image is 100 kB to
   944 kB`.
3. **Banner.** The bytes must contain
   `miniWorld lighting controller %s (%s)`. Refused: `not a miniWorld
   lighting controller image`.
4. **Stamp.** Over the bytes as a Latin-1 string, the regular expression
   `/[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}/g` must
   match exactly once; that match is the build stamp. A build on 2026-09-23
   carries exactly one such string (`strings` on the image confirmed it).
   Zero or several: `cannot find one build stamp in this file`. The
   single-digit day form with two spaces, `Sep  6 2026`, is matched by
   the `[ 0-9]` class.
5. **MD5** over the bytes, from a self-contained routine inside
   `view-system.js` (browsers offer no MD5 in Web Crypto, and the GUI
   loads nothing from outside the device). Lower-case hex, 32 digits.
6. **Send** with `XMLHttpRequest`, not `App.api`, because the body is
   binary and the shared helper is JSON-only: `POST
   /api/system/firmware`, `Content-Type: application/octet-stream`,
   `X-Firmware-MD5`, `X-Firmware-Build`, the `ArrayBuffer` as the body,
   `timeout` 180000 ms. `xhr.upload.onprogress` drives the bar. The
   browser sets `Content-Length` itself and reuses the basic-auth
   credentials it already holds for the origin. `Send` and `Reboot` are
   disabled while the request runs.
7. **Answer.** 200 means staged: the status line says `staged, the board
   is rebooting` and the wait begins. Every other code shows the device's
   words (section 4). No answer at all (network error, or `status` 0)
   shows `no reply from the board; it may be rebooting` and goes to the
   wait anyway, since the board may have taken the image. The page never
   resends on its own: a person is present, and `Send` is the retry.
8. **Wait.** `GET /api/system/status` every 2 s, first after 3 s, for at
   most 90 s, until `build` equals the stamp found in step 4. Success
   refreshes the Device card and says `board is back on build <stamp>`.
   Timeout: `the board did not come back on the new build; if it stays
   unreachable, flash over USB with make upload`.

The upload state (phase, stamp, bytes sent, message) lives in the view's
module scope, not in the DOM, so leaving the tab does not cancel the
request and coming back shows where it stands, the way the WiFi view
keeps its connect state. `unmount()` drops the DOM references only.

Every check before step 6 leaves `Send` enabled so another file can be
picked at once.

## 4. Errors and the status line

One status line, always in words a person at the bench can act on. The
progress bar is visible only during step 6.

| Situation | Status line |
|---|---|
| Browser refusals, steps 2 to 4 | The messages in section 3; nothing was sent |
| 400 | `the board rejected the headers: <error>` (the page cannot produce this, shown for completeness) |
| 413 | `the board takes images up to <max> bytes; this one is <size>` using `max` from the reply |
| 422 | `the board rejected the image: <error>`, where error is `md5 mismatch`, `not this sketch`, `build stamp not in image` or `short body` |
| 500 | `the board could not stage the image: <error>` |
| 401 | The same as every view: `App.api`'s rule, `location.reload()` so the browser's login prompt appears; the XHR path calls the same |
| No reply during the send | `no reply from the board; it may be rebooting`, then the wait |
| Wait timeout | The USB recovery message from section 3 |
| Reboot button, back | `back after <n> s` |
| Reboot button, timeout | `the board did not answer within 60 s; check it over USB` |

After any device answer, the headroom line is re-read. A refusal has
deleted the partial file on the device, and the free space shows it.

## 5. Off-target: the mock

`tools/mockserver.py` already serves the three routes with the device's
checks on the uploaded bytes, so the page can be driven end to end
against a real image from `./build`. Two additions so the page's waits
are exercised rather than skipped:

- After a successful upload the status keeps reporting the previous
  build for 3 s, then the new one. `FirmwareState` gains `build_at`, the
  time at which `build` takes effect, and `system_status_json()` reads
  through it.
- `POST /api/system/reboot` resets the mock's uptime origin, so the
  status reports a smaller uptime afterwards and the Reboot wait ends
  in `back after <n> s`. The mock still does not go away for those
  seconds; the page's "answers again" test is satisfied at once, which
  is fine for a mock.

Both get a unit test in `tools/test_mockserver.py`: the delayed build and
the uptime reset. The mock's `MODEL_TEMPLATES` and the rest are untouched.

## 6. Verification

- **Unit**: `make test-mock`, 27 tests.
- **Bundle**: `make web` prints the gzipped size and fails above the
  40 kB budget. Expected: about 4 kB over today's 29927 bytes, from the
  view, its stylesheet and the MD5 routine.
- **Browser over the mock**: the page at desktop and at 390 px width; a
  real `.bin` accepted and verified with the 3 s wait shown; each browser
  refusal provoked with a doctored file (a too-small file, a file without
  the banner, a file with the stamp removed). The 422 path cannot be
  provoked from a page whose checks mirror the device's, so it is
  verified by reading; the 401 path by starting the mock with
  `--password`.
- **Bench**: from the System tab on the laptop, the current `./build`
  image sent to the Challenger at 192.168.1.180, the board back on the
  new stamp; the Reboot button once; the wait timeout is not provoked on
  the board (it would need the network cut) and is judged by reading.
- **HANDOFF**: the inventory rows for `web/` and the GUI, and a section
  5.10 with the bench result, both copies byte-identical.

## 7. Conventions and constraints carried over

- No em dashes. Header comments end with `Invector Embedded Systems AB`.
- Nothing served from the device fetches anything from outside: the MD5
  is inline, no web fonts, no CDN.
- The device contract is unchanged: headers, codes and messages are the
  ones in `FirmwareUpdate.h`, and the mock mirrors them.
- The GUI is one gzipped file built by `make web`; edit `web/`, never
  `WebUI.gen.h`.
- Only the Challenger on the bench may be flashed.

## 8. Decisions worth recording

- **The browser does the checking, not the device.** The alternative,
  letting the device find any stamp-shaped string and making the MD5
  optional, would have weakened the identity check the previous round
  rests on and let the tool and the page drift apart. Two kilobytes of
  JavaScript are cheaper.
- **No automatic resend from the page.** The host tool retries once
  because it runs unattended in `make ota`. In the GUI a person is
  watching and can press `Send` again; the page only makes sure a lost
  reply does not hide a successful update, by waiting for the stamp.
- **A separate System tab rather than a card on Home.** Home is about
  the scene. Device-level things get one place, and later rounds (a log,
  a factory reset) have somewhere to go.
