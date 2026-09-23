# Firmware update over the air (FOTA): design

Date: 2026-09-23. Owner: Pontus, Invector Embedded Systems AB.
Status: approved in conversation, implementation follows this document.

## 1. Goal

Let a new firmware image reach the miniWorld lighting controller over
WiFi, so a board built into a layout no longer needs its USB port
reached for every change. The image is pushed to the device over the
existing HTTP API, by a make target for the bench and later by the GUI.

In scope, first round:

- `POST /api/system/firmware`: a streaming upload of the plain `.bin`
  into LittleFS, verified on the device, then handed to the core's OTA
  boot stage and a reboot.
- `GET /api/system/firmware`: filesystem headroom and the accepted
  size band, for the tool's preflight and the GUI round that follows.
- `make ota HOST=<address>`: the air equivalent of `make upload`, with
  the same image checks before and the same build-stamp check after.
- Mock, apicheck and HANDOFF coverage.

Out of scope: the GUI upload page (next round), a pull model where the
device fetches from a URL, signed images, rollback to the previous image,
updating the filesystem contents over the air, keeping the scene running
during the upload.

## 2. How the core's OTA works, and what we lean on

Every image built with arduino-pico 5.5.1 already carries a 12 kB
"stage 3" boot loader (`lib/rp2040/ota.o`, linked into every sketch by
`platform.txt`). At boot it mounts LittleFS, looks for
`otacommand.bin`, and if that file carries the signature `Pico OTA` and
a valid CRC it copies the named file from LittleFS into application
flash 4 kB block by block, skipping blocks already identical, then
erases the command file and reboots. Power loss during the copy restarts
it from the beginning at the next boot. The stage inflates gzip on the
fly, but we do not use that (section 8). Its debug UART is compiled out
of the shipped object, so it drives no GPIO.

The application's side is two calls from the core's `PicoOTA` library:
`picoOTA.begin(); picoOTA.addFile("firmware.bin"); picoOTA.commit();`.
Nothing about the flash layout changes: the board keeps
`flash=8388608_1048576`, 8 MB with a 1 MB filesystem, and the staged
image lives in that filesystem next to the three JSON documents.

The core's `Update` class (`libraries/Updater`) wraps the file write and
an MD5, but its `end()` commits the OTA command with no hook before it.
We want an identity check between the last byte and the commit, so we
write the file ourselves with `LittleFS`, take `MD5Builder` from the
core, and call `PicoOTA` directly.

## 3. The HTTP contract

### 3.1 `POST /api/system/firmware`

Body: the raw image, `build/miniWorld_LightingController.ino.bin`, as
`application/octet-stream`. `Content-Length` is required. Two request
headers carry what `tools/checkimage.sh` and `tools/flash.sh` check on
the host today:

| Header | Value | Required |
|---|---|---|
| `X-Firmware-MD5` | 32 hex digits, MD5 of the body | yes |
| `X-Firmware-Build` | the image's `MINIWORLD_BUILD` string, for example `Sep 22 2026 09:35:04` | yes |

Basic auth applies exactly as on every other route, and it is checked
before a single body byte is read (section 4.2).

Replies, all `application/json`:

| Code | Body | When |
|---|---|---|
| 200 | `{"ok":true,"size":247836,"md5":"…","build":"…"}` | staged and verified; the device reboots about 200 ms after the response, through the existing reboot-pending hook, and comes back on the new image |
| 400 | `{"error":"missing X-Firmware-MD5"}` or `…-Build` or `"missing Content-Length"` | a header is absent or malformed |
| 405 | `{"error":"method not allowed"}` | any method other than GET or POST |
| 409 | `{"error":"upload in progress"}` | a second upload while one is open; cannot happen with one client served at a time, kept so the state can never be corrupted |
| 413 | `{"error":"payload too large","max":N}` | `Content-Length` is above `maxSize` (section 3.3) or below the lower bound of a real build |
| 422 | `{"error":"md5 mismatch"}` / `{"error":"not this sketch"}` / `{"error":"build stamp not in image"}` / `{"error":"short body"}` | the upload arrived but failed verification; the staged file is deleted |
| 500 | `{"error":"filesystem write failed"}` / `{"error":"commit failed"}` | LittleFS refused the file or the command |

A 413 and a 400 are answered without reading the body. `Connection:
close` on every reply, as today, so the client sees the answer whether
or not the body was consumed.

### 3.2 `GET /api/system/firmware`

```
{"fsTotal":1048576,"fsFree":1011712,"maxSize":900000,"minSize":100000,
 "staged":false}
```

`fsTotal` and `fsFree` come from `LittleFS.info()`. `maxSize` is the
largest image `POST` accepts right now (section 3.3). `staged` is true
when a `firmware.bin` exists in the filesystem, which after a normal
boot is never (section 5.4).

### 3.3 The size band

The lower bound is 100000 bytes: a build of this sketch is about 248 kB
and nothing under 100 kB can be one. The upper bound is the smaller of
1048576 (the filesystem is 1 MB, and the flash image must also fit
below the filesystem, which starts 7 MB in) and `fsFree` less a 64 kB
margin for LittleFS metadata and the command file. With the three JSON
documents present that leaves roughly 900 kB, so the band is never the
limit in practice; it exists so a wrong file is refused before the
transfer, not after.

### 3.4 `/api/system/status`

Unchanged. Its `build` field is what `make ota` polls after the reboot.

## 4. HttpServer: one streaming path

### 4.1 The rule that stays

Handlers never see a socket, a header or a client. That rule is kept:
`HttpServer` does the streaming, and the new `FirmwareUpdate` class sees
only buffers.

### 4.2 What changes in `readRequest` and `tick`

- `Request` grows two fields, `firmwareMd5` and `firmwareBuild`, filled
  from the two new headers. No other header handling changes.
- The `Content-Length` bound is chosen by path. For every path it stays
  `BODY_MAX` (16384). For the firmware path it is `FirmwareUpdate::maxSize()`,
  and a length below `FirmwareUpdate::minSize()` also sets `tooLarge`
  (the 413 reads "outside the band" in that case). `readRequest` already
  has the request line before it reads the headers, so the path is known
  when `Content-Length` arrives.
- For a `POST` to the firmware path, `readRequest` returns after the
  blank line and reads no body. `tick()` then runs its existing chain
  (400, 431, 413, portal redirect, auth) and only when the request would
  otherwise be dispatched does it call a new private `streamFirmware(c, r)`.
- `streamFirmware` calls `FirmwareUpdate::begin(length, md5, build, out)`;
  a non-zero code from `begin` is sent as is. Otherwise it reads the body
  in up to 2048-byte pieces into a static buffer and passes each to
  `FirmwareUpdate::write(buf, n)`, with the existing 2 s idle timeout per
  piece and a new `UPLOAD_TIMEOUT_MS = 120000` whole-upload deadline in
  place of the 5 s `REQUEST_TIMEOUT_MS`. When the count reaches
  `Content-Length` it calls `FirmwareUpdate::end(out)` and sends the
  code that returns. If the client goes away, a read fails, or a deadline
  passes, it calls `FirmwareUpdate::abort()` and closes the connection
  with no reply, as the server does today for a dropped request.
- The reboot after a 200 uses the existing `SystemWebApi::rebootPending()`
  path: `FirmwareUpdate::end()` sets the same flag through a new
  `SystemWebApi::requestReboot()`, so the response goes out and the
  connection closes before `rp2040.reboot()`.
- Every other route, including the rest of `/api/system/*`, is untouched.
  `SystemWebApi::owns()` still claims the `/api/system` prefix;
  `FirmwareUpdate::owns()` claims exactly `/api/system/firmware` and is
  asked first, so the GET lands in `FirmwareUpdate::handle()` (a normal
  pure handler that produces section 3.2) and the POST in `streamFirmware`.

### 4.3 Why 2048-byte reads

`WiFiEspAT` hands a read straight to `AT+CIPRECVDATA` when the caller's
buffer is larger than its own 64-byte one, so one AT round trip moves one
full frame, which the AT firmware caps at about 2 kB. The 64-byte stack
buffer the JSON body path uses would make a 248 kB image cost 4000
round trips; the 2 kB buffer makes it about 120. No `WIFIESPAT_*` size
is changed.

### 4.4 The scene during an upload

`Http.tick()` serves one client to completion, so `Scene.tick()` does not
run while the body streams in: about 10 s at 921600 baud, about 30 s if
the link stayed at 115200. The lamps hold their levels; nothing is
re-sent to the buses. This is accepted for the first round and written
in HANDOFF. An idle hook that calls `Scene.tick()` between pieces is the
obvious follow-up if it matters.

## 5. FirmwareUpdate

New files `FirmwareUpdate.h` and `FirmwareUpdate.cpp` in the sketch
folder, system band, next to `SystemWebApi`. Static class, no instance,
like the web API classes.

### 5.1 Interface

```
class FirmwareUpdate {
public:
    static bool   owns(const String &path);          // exactly /api/system/firmware
    static int    handle(const String &method, const String &path,
                         const String &requestBody, String &body);   // GET, 405 otherwise
    static size_t minSize();                          // 100000
    static size_t maxSize();                          // section 3.3, from LittleFS.info()

    // Streaming upload, driven by HttpServer. begin() returns 0 to go on
    // or an HTTP code with body filled.
    static int    begin(size_t length, const String &md5, const String &build, String &body);
    static bool   write(const uint8_t *data, size_t len);
    static int    end(String &body);                  // 200, 422 or 500, body filled
    static void   abort();                            // drop a partial upload

    static void   cleanupAtBoot();                    // remove a leftover firmware.bin
};
```

### 5.2 `begin`

Rejects a malformed MD5 (not 32 hex digits) or an empty build with 400,
a length outside the band with 413, an upload already in progress with
409 (cannot happen with one client at a time, but the state must not be
corrupted if it ever does). Mounts LittleFS if needed, removes any old
`firmware.bin`, opens it for writing, resets `MD5Builder`, stores the
expected MD5 lower-cased and the expected build, returns 0.

### 5.3 `write` and `end`

`write` appends to the file and feeds the MD5; a short write fails the
upload and `end` answers 500. `end`:

1. Closes the file. A byte count short of the declared length is 422
   `short body` (the server only calls `end` at the full count, so this
   is a guard).
2. Finalises the MD5 and compares it, case-insensitive, with the header.
   Mismatch is 422 `md5 mismatch`.
3. Reopens the file and scans it for two byte strings: the banner format
   `miniWorld lighting controller %s (%s)` and the expected build stamp.
   The scan reads 4 kB pieces and carries the last 63 bytes over so a
   match across a boundary is found. The banner missing is 422
   `not this sketch`; the stamp missing is 422 `build stamp not in image`.
   This is `checkimage.sh` lines 46 and 52 run on the device, against the
   bytes that will be flashed.
4. `picoOTA.begin(); picoOTA.addFile("firmware.bin"); picoOTA.commit();`.
   A false return is 500 `commit failed` and the file is removed.
5. Calls `SystemWebApi::requestReboot()`, fills the 200 body, returns 200.

On any 422 or 500 the file is removed so the filesystem is left as it
was.

### 5.4 Boot

`setup()` calls `FirmwareUpdate::cleanupAtBoot()` right after the banner.
The OTA stage erases only the command file, so a `firmware.bin` found at
boot is either the image now running (copy done) or a half upload that
never got a command (power lost mid-transfer). Both are deleted; the log
line says `firmware: removed staged image`. That keeps 248 kB of
filesystem free for the next upload and keeps `staged` in section 3.2
honest.

### 5.5 RAM and flash

A 2 kB static receive buffer in `HttpServer`, a 4 kB static scan buffer
in `FirmwareUpdate` (could share; kept separate for clarity, both are
far below the 12 kB `SceneConfig` the RAM rule watches), an `MD5Builder`
and a `File`. No new static `SceneConfig`, no heap in the steady state.
The `PicoOTA` page is allocated for the commit and freed with the
object. The build figures go into HANDOFF as with every round.

## 6. Tooling

### 6.1 `make ota HOST=<address>`

New target, `ota: compile` then `tools/ota.sh $(BUILD_PATH) "$(HOST)"`.
The script:

1. Runs `tools/checkimage.sh` on the build directory, so a stale,
   foreign or odd-sized image is refused before it leaves the host,
   exactly as `flash.sh` does. Reads the stamp from `.stamp`.
2. Computes `md5sum` of the `.bin`.
3. `GET /api/system/firmware` as a preflight: refuses if the image is
   above `maxSize`, and reports `fsFree`.
4. `curl --data-binary @<bin> -H 'Content-Type: application/octet-stream'
   -H 'X-Firmware-MD5: …' -H 'X-Firmware-Build: …' -H 'Expect:'
   -m 180` to `http://$HOST/api/system/firmware`. `Expect:` is emptied
   because the server does not answer `100-continue` and curl would
   otherwise wait a second before sending. The password comes from
   `OTA_PASSWORD` in the environment and goes to `curl -u ":$OTA_PASSWORD"`;
   no password in the Makefile, none on the command line.
5. On 200 it waits for the reboot and polls `/api/system/status` for up
   to 90 s (the copy of a 248 kB image plus the ESP bring-up) until
   `build` equals the stamp, the same test as `flash.sh`'s API fallback.
   Any other outcome is a failure with the device's JSON error printed.

`HOST` accepts an IP or `miniworld.local`. `MARKER` is not needed on the
air path: the erase-net build is a bench-only USB flow.

### 6.2 `tools/apicheck.sh`

`check GET /api/system/firmware 200` always. Under
`APICHECK_DESTRUCTIVE=1` a `POST` with a 16-byte body and valid-looking
headers expecting 413 (below the band, never stored), so the streaming
route is proven to answer without a real image. Against the mock both
run in every case.

### 6.3 `tools/mockserver.py` and `tools/test_mockserver.py`

The mock answers the GET with fixed numbers and the POST with the same
band and header checks, an MD5 over the received body, and a scan for
the banner and stamp strings. It never reboots. One test class covers
the 400, 413, 422 and 200 paths with synthetic bodies. The mock stays
the place the GUI round develops against.

### 6.4 Documents

- `HANDOFF.md` (both copies, byte-identical): a §5 entry "5.9 Firmware
  over the air" recording what was done and what was verified on the
  board; inventory rows for `FirmwareUpdate.h/.cpp`, `tools/ota.sh`
  and the changed `HttpServer` row; a §6 note that the core's OTA stage
  had never run on this board before this round; the RAM and flash
  figures; a §8 paragraph on why the plain `.bin` and not gzip, and why
  not the core's `Update` class.
- `CLAUDE.md` make target list gains `make ota`.
- `SystemWebApi.h` header comment lists the two firmware routes with a
  pointer to `FirmwareUpdate.h`, which carries the contract of
  section 3.

## 7. Failure model

| Event | Result |
|---|---|
| Power lost during upload | half `firmware.bin`, no command file; deleted at next boot; the old image runs |
| Client drops mid-upload | `abort()` deletes the file; the old image runs; no reply |
| Wrong file, wrong stamp, bit error on the wire | 422, file deleted, old image runs |
| Power lost during the flash copy | the OTA stage restarts the copy at next boot (its design) |
| Image passes every check but breaks the network | no rollback; USB flash with `make upload` recovers it. Written in HANDOFF and in `ota.sh`'s final line |
| Second client during an upload | waits in the AT accept backlog (three connections) and most likely times out; harmless |
| Upload at 115200 (baud raise failed) | about 30 s, inside the 120 s deadline |

## 8. Decisions worth the reason

- **Plain `.bin`, not gzip.** gzip takes the image from 248 kB to 180 kB,
  a 28 percent saving worth a few seconds, but it hides the banner and
  stamp strings from the device-side identity check, and the MD5 would
  cover the archive rather than the bytes that reach flash. The check is
  worth more than the seconds.
- **Not the core's `Update` class.** It commits inside `end()`; the
  identity scan needs the closed file before the commit. `MD5Builder`
  and `PicoOTA` are used directly, which is all `Update` does on RP2040
  apart from a 4 kB buffer.
- **Headers, not query parameters.** The stamp has spaces and colons;
  headers avoid the encoding, and the server already strips the query
  from the path.
- **Auth before the body.** The upload path is the only one where the
  body is large, so it is the only place the order matters. A 401 or a
  413 is answered from the headers alone.
- **The same checks as the USB path.** Image identity on the host
  (`checkimage.sh`), image identity on the device (banner and stamp in
  the received bytes), stamp on the running board afterwards. A FOTA
  that could be less careful than `make upload` would undo the reason
  `make upload` exists.

## 9. Verification

1. `make` warning-free; `make test-mock` and `make check` against the
   mock pass, with the new firmware cases.
2. On the board, from the bench, over USB console and the API:
   - `make ota HOST=192.168.1.180` with the image already running: the
     stage finds every block identical, the board is back in a few
     seconds, `build` unchanged.
   - Touch a source, `make ota` again: the console shows the new banner
     stamp, `/api/system/status` reports it, `make ota` exits 0.
   - Negative: a wrong `X-Firmware-MD5` (422, `fsFree` back to its
     starting value), a truncated body via `curl -m 2` (no reply, file
     gone at the next `GET`), a 200 kB text file (422 `not this sketch`),
     `Content-Length` of 2000000 (413 before any transfer).
   - The USB path still works afterwards: `make upload` once, to prove
     the air path did not disturb it.
3. Record the upload duration and the observed baud rate in HANDOFF §5.9.
