# Firmware Update Over The Air Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A firmware image reaches the board over WiFi through `POST /api/system/firmware`, is verified on the device, and is flashed by the arduino-pico OTA boot stage at the reboot that follows; `make ota HOST=…` drives it with the same checks as `make upload`.

**Architecture:** `HttpServer` gains one streaming path that feeds a new static `FirmwareUpdate` class buffer by buffer; every other handler stays a pure `(method, path, body)` function. `FirmwareUpdate` writes `firmware.bin` into LittleFS, checks the MD5 and scans the file for the banner and the build stamp, then writes the OTA command through the core's `PicoOTA` and asks `SystemWebApi` for the reboot. The mock grows the same two routes so the tool and the later GUI can be developed off-target.

**Tech Stack:** arduino-pico 5.5.1 (`LittleFS`, `MD5Builder`, `PicoOTA`, the `ota.o` boot stage), ArduinoJson 7, WiFiEspAT 2.0.0, POSIX sh and curl for the tool, Python 3 `http.server` and `unittest` for the mock.

**Spec:** `docs/superpowers/specs/2026-09-23-firmware-update-design.md`

## Global Constraints

- No em dashes anywhere: prose, comments, commit messages, shell, Python, HANDOFF.
- Every new source file's header comment ends with `Invector Embedded Systems AB`.
- The build is `make` at the repo root and must stay warning-free with `--warnings default`. Never call `arduino-cli upload` by hand; flash only through `make upload` or, once it exists, `make ota`.
- Web API handlers are server-agnostic `(method, path, body) -> (code, json)`. `FirmwareUpdate` sees buffers, never a `WiFiClient`.
- The staged file name is exactly `firmware.bin`, no leading slash, because that is what the core's own `Updater` hands `PicoOTA` and what the boot stage opens.
- Size band: `minSize` 100000; `maxSize` is the smaller of 1048576 and LittleFS free space minus 65536.
- Upload deadline 120000 ms, per-piece idle timeout the existing 2000 ms, read pieces of 2048 bytes.
- The 200 reply must go out and the connection close before the reboot: use `SystemWebApi::rebootPending()`, never call `rp2040.reboot()` from `FirmwareUpdate`.
- `HANDOFF.md` at the repo root and `miniWorld_LightingController/HANDOFF.md` must stay byte-identical (copy after every edit).
- Only the Challenger on the bench may be flashed (memory: bench state 2026-09-07). Its address is 192.168.1.180.
- No new static `SceneConfig`; the two new static buffers total 6 kB.

## Review Focus

Inputs the spec implies but no task's tests would otherwise exercise, most likely to bite first. Each has a test pinned to the owning task.

1. A `Content-Length` header that is absent on the firmware POST: the device must answer 413 (length 0 is below the band), not stall waiting for a body. Pinned in Task 4 (bench step) and Task 1 (mock test `test_rejects_outside_band` with length 0).
2. An upper-case MD5 in `X-Firmware-MD5` from a hand-typed curl: must match case-insensitively. Pinned in Task 1 (`test_accepts_uppercase_md5`) and Task 3 (`equalsIgnoreCase`).
3. The build stamp straddling a 4 kB scan boundary in the received file: must still be found. Pinned in Task 1 (`test_finds_stamp_across_chunk_boundary`, the mock scans with the same 4096 plus 63 byte carry).
4. A client that connects, sends the headers, then stops: the device must free the file and be ready for the next upload, not hold `firmware.bin` open. Pinned in Task 4 (bench step with `curl -m 2`, then `GET` shows `staged:false` and a second upload succeeds).
5. A second `make ota` of the image already running: the boot stage skips every identical block and the device comes back on the same stamp, which the tool must report as success, not "did not change". Pinned in Task 5 (the tool compares the stamp, not a before/after difference) and Task 6 bench step 1.

---

### Task 1: Mock firmware routes and their tests

**Files:**
- Modify: `tools/mockserver.py` (constants near line 40, a `FirmwareState` class after `system_status_json` near line 1566, routing in `_dispatch` near line 1680, `_api` near line 1819, `main()` global)
- Modify: `tools/test_mockserver.py` (new test class at the end)

**Interfaces:**
- Produces: `mockserver.FirmwareState` with `info_json() -> dict`, `check(length, md5, build)` raising `ApiError(400|413)`, `upload(body_bytes, md5, build) -> dict` raising `ApiError(422)`, attribute `build` (str, starts `"mock"`), attribute `staged` (bool). Module global `FIRMWARE = FirmwareState()`. `system_status_json()` reports `FIRMWARE.build`.
- Consumed by: Task 2 (`apicheck.sh` against the mock), Task 5 (`ota.sh` end to end against the mock).

- [ ] **Step 1: Write the failing tests**

Append to `tools/test_mockserver.py`:

```python
import hashlib


def _md5(data):
    return hashlib.md5(data).hexdigest()


def _image(stamp, size=120000, banner=True, stamp_at=None):
    """A synthetic firmware body: filler, the banner format string and the
    build stamp as C strings. stamp_at places the stamp at a byte offset so
    a test can straddle the scanner's 4096-byte chunk boundary."""
    banner_bytes = mockserver.FIRMWARE_BANNER + b"\0" if banner else b""
    stamp_bytes = stamp.encode() + b"\0" if stamp else b""
    if stamp_at is None:
        head = b"\x7f" * 1000
        body = head + banner_bytes + stamp_bytes
    else:
        body = b"\x7f" * stamp_at + stamp_bytes + banner_bytes
    return body + b"\x7f" * (size - len(body))


class FirmwareTest(unittest.TestCase):
    STAMP = "Sep 23 2026 10:00:00"

    def test_info_shape(self):
        fw = mockserver.FirmwareState()
        info = fw.info_json()
        self.assertEqual(set(info), {"fsTotal", "fsFree", "maxSize", "minSize", "staged"})
        self.assertEqual(info["minSize"], 100000)
        self.assertLessEqual(info["maxSize"], info["fsFree"] - 65536)
        self.assertLessEqual(info["maxSize"], 1048576)
        self.assertFalse(info["staged"])

    def test_rejects_bad_headers(self):
        fw = mockserver.FirmwareState()
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.check(120000, "nothex", self.STAMP)
        self.assertEqual(cm.exception.code, 400)
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.check(120000, "0" * 32, "")
        self.assertEqual(cm.exception.code, 400)

    def test_rejects_outside_band(self):
        fw = mockserver.FirmwareState()
        for length in (0, 16, 99999, 2000000):
            with self.assertRaises(mockserver.ApiError) as cm:
                fw.check(length, "0" * 32, self.STAMP)
            self.assertEqual(cm.exception.code, 413, length)
        fw.check(100000, "0" * 32, self.STAMP)   # the lower bound itself passes

    def test_rejects_md5_mismatch(self):
        fw = mockserver.FirmwareState()
        body = _image(self.STAMP)
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.upload(body, "0" * 32, self.STAMP)
        self.assertEqual(cm.exception.code, 422)
        self.assertEqual(cm.exception.message, "md5 mismatch")
        self.assertEqual(fw.build, "mock")
        self.assertFalse(fw.staged)

    def test_rejects_foreign_image(self):
        fw = mockserver.FirmwareState()
        body = _image(self.STAMP, banner=False)
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.upload(body, _md5(body), self.STAMP)
        self.assertEqual(cm.exception.code, 422)
        self.assertEqual(cm.exception.message, "not this sketch")

    def test_rejects_missing_stamp(self):
        fw = mockserver.FirmwareState()
        body = _image("Sep 01 2026 00:00:00")
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.upload(body, _md5(body), self.STAMP)
        self.assertEqual(cm.exception.code, 422)
        self.assertEqual(cm.exception.message, "build stamp not in image")

    def test_accepts_and_reports_build(self):
        fw = mockserver.FirmwareState()
        body = _image(self.STAMP)
        result = fw.upload(body, _md5(body), self.STAMP)
        self.assertEqual(result, {"ok": True, "size": len(body),
                                  "md5": _md5(body), "build": self.STAMP})
        self.assertEqual(fw.build, self.STAMP)
        self.assertFalse(fw.staged)   # the mock "reboots" at once

    def test_accepts_uppercase_md5(self):
        fw = mockserver.FirmwareState()
        body = _image(self.STAMP)
        result = fw.upload(body, _md5(body).upper(), self.STAMP)
        self.assertEqual(result["md5"], _md5(body))

    def test_finds_stamp_across_chunk_boundary(self):
        fw = mockserver.FirmwareState()
        # The stamp starts 5 bytes before the first 4096-byte boundary.
        body = _image(self.STAMP, stamp_at=4096 - 5)
        result = fw.upload(body, _md5(body), self.STAMP)
        self.assertEqual(result["build"], self.STAMP)
```

Also add `import hashlib` next to the other imports at the top of the file if you prefer it there; the snippet above keeps it local to the block so the diff is one hunk.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 -m unittest tools.test_mockserver.FirmwareTest -v` from the repo root.
Expected: every test errors with `AttributeError: module 'mockserver' has no attribute 'FirmwareState'` (or `FIRMWARE_BANNER`).

- [ ] **Step 3: Implement `FirmwareState` in the mock**

In `tools/mockserver.py`, after the `CAPTIVE_PROBES` tuple (about line 50), add the constants:

```python
# Firmware upload, spec 2026-09-23-firmware-update-design.md sections 3
# and 5. The numbers mirror the device: a 1 MB LittleFS with the three
# JSON documents in it, a 64 kB margin for the filesystem's own needs.
FIRMWARE_MIN_SIZE = 100000
FIRMWARE_MAX_SIZE = 1048576
FIRMWARE_FS_TOTAL = 1048576
FIRMWARE_FS_USED = 36864
FIRMWARE_FS_MARGIN = 65536
FIRMWARE_BANNER = b"miniWorld lighting controller %s (%s)"
FIRMWARE_SCAN_CHUNK = 4096
FIRMWARE_SCAN_OVERLAP = 63
```

After `system_status_json()` (about line 1576) add:

```python
class FirmwareState:
    """What the device does with an upload, minus the flash: the band and
    header checks, an MD5 over the body, the scan for the banner and the
    build stamp in 4 kB pieces with a 63-byte carry like the firmware's,
    and on success the reported build becomes the uploaded stamp, which is
    what tools/ota.sh polls for."""

    def __init__(self):
        self.build = "mock"
        self.staged = False

    def free(self):
        return FIRMWARE_FS_TOTAL - FIRMWARE_FS_USED

    def max_size(self):
        return min(FIRMWARE_MAX_SIZE, self.free() - FIRMWARE_FS_MARGIN)

    def info_json(self):
        return {
            "fsTotal": FIRMWARE_FS_TOTAL,
            "fsFree": self.free(),
            "maxSize": self.max_size(),
            "minSize": FIRMWARE_MIN_SIZE,
            "staged": self.staged,
        }

    def check(self, length, md5, build):
        """The checks the device makes from the headers alone, before it
        reads a body byte. Raises ApiError; returns None when all is well."""
        if len(md5) != 32 or any(c not in "0123456789abcdefABCDEF" for c in md5):
            raise ApiError(400, "missing X-Firmware-MD5")
        if not build or len(build) > 63:
            raise ApiError(400, "missing X-Firmware-Build")
        if length < FIRMWARE_MIN_SIZE or length > self.max_size():
            raise ApiError(413, "payload too large")

    @staticmethod
    def _contains(data, needle):
        carry = b""
        for off in range(0, len(data), FIRMWARE_SCAN_CHUNK):
            piece = carry + data[off:off + FIRMWARE_SCAN_CHUNK]
            if needle in piece:
                return True
            carry = piece[-FIRMWARE_SCAN_OVERLAP:]
        return False

    def upload(self, body, md5, build):
        self.check(len(body), md5, build)
        got = hashlib.md5(body).hexdigest()
        if got != md5.lower():
            raise ApiError(422, "md5 mismatch")
        if not self._contains(body, FIRMWARE_BANNER):
            raise ApiError(422, "not this sketch")
        if not self._contains(body, build.encode()):
            raise ApiError(422, "build stamp not in image")
        self.build = build
        self.staged = False   # the device reboots and cleans up at boot
        sys.stderr.write("mock: firmware %d bytes staged, build %s (reboot ignored)\n"
                         % (len(body), build))
        return {"ok": True, "size": len(body), "md5": got, "build": build}


FIRMWARE = FirmwareState()
```

Add `import hashlib` to the imports at the top of `mockserver.py`. Change `system_status_json()` so `"build": FIRMWARE.build` replaces `"build": "mock"`.

- [ ] **Step 4: Route the two paths in the mock**

In `Handler._dispatch`, after `if not self._check_auth(): return` and before the `/` page branch, add the streaming POST, which must not go through the JSON parse:

```python
        if path == "/api/system/firmware" and method == "POST":
            # The device answers 400 and 413 from the headers alone, before
            # the body; the mock has already read the body, which is the one
            # difference, and it checks in the same order.
            md5 = (self.headers.get("X-Firmware-MD5") or "").strip()
            build = (self.headers.get("X-Firmware-Build") or "").strip()
            length = int(self.headers.get("Content-Length", 0) or 0)
            try:
                FIRMWARE.check(length, md5, build)
                result = FIRMWARE.upload(body_bytes, md5, build)
            except ApiError as e:
                payload = {"error": e.message}
                if e.code == 413:
                    payload["max"] = FIRMWARE.max_size()
                self._send_json(payload, e.code)
                return
            self._send_json(result, 200)
            return
```

In `_api`, before the `/api/system/status` branch:

```python
        if path == "/api/system/firmware":
            if method == "GET":
                return 200, FIRMWARE.info_json()
            raise ApiError(405, "method not allowed")
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `python3 -m unittest tools/test_mockserver.py -v` from the repo root.
Expected: all previous 16 tests plus the 9 new ones pass.

- [ ] **Step 6: Smoke the routes over HTTP**

Run in one shell: `make mock`. In another:

```
curl -s localhost:8080/api/system/firmware
curl -s -o /dev/null -w '%{http_code}\n' -X POST --data-binary 'nope' \
  -H 'X-Firmware-MD5: 00000000000000000000000000000000' -H 'X-Firmware-Build: none' \
  localhost:8080/api/system/firmware
```

Expected: the JSON of `info_json()`, then `413`.

- [ ] **Step 7: Commit**

```bash
git add tools/mockserver.py tools/test_mockserver.py
git commit -m "Mock: firmware upload routes with the device's checks

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019ZqMmutmHXQhskefHF1Hu9"
```

---

### Task 2: apicheck.sh covers the firmware routes

**Files:**
- Modify: `tools/apicheck.sh` (after the `/api/system/status` line, about line 84)

**Interfaces:**
- Consumes: the mock routes from Task 1; against a device, Task 3 and 4.

Note on the spec: section 6.2 puts the POST under `APICHECK_DESTRUCTIVE=1`. A 16-byte body is refused from the headers alone and nothing on the device changes, so it runs in every mode; the spec's caution was about state, and there is none.

- [ ] **Step 1: Add the checks**

Insert after `check GET  /api/system/status 200`:

```sh
check GET  /api/system/firmware 200
# A body far below the band is refused from the headers alone, before
# any byte of it is stored, so the streaming route is proven without a
# real image and without changing anything on a device.
if [ -n "$AUTH" ]; then
    code=$(curl -s -o /dev/null -w "%{http_code}" -u "$AUTH" -X POST \
        -H "Content-Type: application/octet-stream" \
        -H "X-Firmware-MD5: 00000000000000000000000000000000" \
        -H "X-Firmware-Build: none" --data-binary "not a firmware" \
        "$BASE/api/system/firmware")
else
    code=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
        -H "Content-Type: application/octet-stream" \
        -H "X-Firmware-MD5: 00000000000000000000000000000000" \
        -H "X-Firmware-Build: none" --data-binary "not a firmware" \
        "$BASE/api/system/firmware")
fi
printf "%s %s %s\n" "$code" POST /api/system/firmware
[ "$code" = "413" ] || FAIL=1
```

- [ ] **Step 2: Run against the mock**

With `make mock` running: `make check`.
Expected: the two new lines print `200 GET /api/system/firmware` and `413 POST /api/system/firmware`; exit status 0.

- [ ] **Step 3: Commit**

```bash
git add tools/apicheck.sh
git commit -m "apicheck: the firmware routes, a 413 proves the streaming path

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019ZqMmutmHXQhskefHF1Hu9"
```

---

### Task 3: FirmwareUpdate on the device, and the boot cleanup

**Files:**
- Create: `miniWorld_LightingController/FirmwareUpdate.h`
- Create: `miniWorld_LightingController/FirmwareUpdate.cpp`
- Modify: `miniWorld_LightingController/SystemWebApi.h` (header comment and one method)
- Modify: `miniWorld_LightingController/SystemWebApi.cpp` (one function)
- Modify: `miniWorld_LightingController/miniWorld_LightingController.ino` (one include, one call after the banner)

**Interfaces:**
- Produces, for Task 4:
  - `static bool FirmwareUpdate::owns(const String &path)`: exactly `/api/system/firmware`.
  - `static int FirmwareUpdate::handle(const String &method, const String &path, const String &requestBody, String &body)`: GET answers 200 with the info object; anything else 405.
  - `static size_t FirmwareUpdate::minSize()` and `maxSize()`.
  - `static int FirmwareUpdate::begin(size_t length, const String &md5, const String &build, String &body)`: 0 to go on, else an HTTP code with `body` filled.
  - `static bool FirmwareUpdate::write(const uint8_t *data, size_t len)`.
  - `static int FirmwareUpdate::end(String &body)`: 200, 422 or 500, `body` filled.
  - `static void FirmwareUpdate::abort()`.
  - `static void FirmwareUpdate::cleanupAtBoot()`.
  - `static void SystemWebApi::requestReboot()`.

There is no off-target test for device code (CLAUDE.md: `make` is the lint step). The test cycle here is: the file compiles cleanly, then the bench in Task 6.

- [ ] **Step 1: Write `FirmwareUpdate.h`**

```cpp
/*
    FirmwareUpdate - a firmware image uploaded over HTTP into LittleFS,
                     verified, then handed to the arduino-pico OTA boot
                     stage at the reboot that follows.

    Routes, spec docs/superpowers/specs/2026-09-23-firmware-update-design.md:

      GET  /api/system/firmware   {"fsTotal":N,"fsFree":N,"maxSize":N,
                                   "minSize":N,"staged":bool}
      POST /api/system/firmware   body: the raw .bin (application/octet-stream)
                                  headers: X-Firmware-MD5 (32 hex digits over
                                  the body), X-Firmware-Build (the image's
                                  MINIWORLD_BUILD string)
        200 {"ok":true,"size":N,"md5":"..","build":".."}  staged; reboot follows
        400 missing X-Firmware-MD5 | missing X-Firmware-Build
        409 upload in progress
        413 payload too large (Content-Length outside the band; the server
            answers this from the headers, begin() repeats it as a guard)
        422 md5 mismatch | not this sketch | build stamp not in image | short body
        500 filesystem write failed | commit failed

    The GET is a plain (method, path, body) handler like every other web
    API. The POST is streamed by HttpServer through begin(), write() and
    end(), because a 248 kB image cannot live in a String; this class sees
    buffers only, never the client.

    How it works: the body is written to LittleFS as "firmware.bin" (that
    exact name, the one the core's Updater uses and the boot stage opens)
    while an MD5 accumulates. end() compares the MD5 with the header,
    rescans the file for the boot banner format string and the given build
    stamp, which is tools/checkimage.sh's identity test run against the
    bytes that will be flashed, and only then writes the OTA command page
    through PicoOTA and asks SystemWebApi for the reboot. The boot stage
    (lib/rp2040/ota.o, in every image) copies the file into application
    flash and erases the command. It does not erase the file, so
    cleanupAtBoot() does.

    Anything short of a 200 removes the file. There is no rollback: an
    image that passes every check but breaks the network needs USB.

    Dependencies: LittleFS, MD5Builder and PicoOTA from the core,
    ArduinoJson, SystemWebApi for the reboot flag.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>

class FirmwareUpdate {
public:
    static bool owns(const String &path) { return path == "/api/system/firmware"; }

    // The GET. Any other method is 405.
    static int handle(const String &method, const String &path,
                      const String &requestBody, String &body);

    // The band a Content-Length must fall in. maxSize() asks LittleFS for
    // its free space, one filesystem call.
    static size_t minSize();
    static size_t maxSize();

    // The streaming POST, driven by HttpServer. begin() returns 0 to go
    // on, or an HTTP code with body filled. write() returns false once a
    // write has failed; end() then answers 500. end() returns 200, 422 or
    // 500 with body filled. abort() drops a partial upload with no reply.
    static int begin(size_t length, const String &md5, const String &build, String &body);
    static bool write(const uint8_t *data, size_t len);
    static int end(String &body);
    static void abort();

    // Removes a leftover firmware.bin: the image just copied, or a half
    // upload that never got a command. Call once from setup().
    static void cleanupAtBoot();
};
```

- [ ] **Step 2: Write `FirmwareUpdate.cpp`**

```cpp
/*
    FirmwareUpdate - see FirmwareUpdate.h

    Invector Embedded Systems AB
*/

#include "FirmwareUpdate.h"
#include "SystemWebApi.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <MD5Builder.h>
#include <PicoOTA.h>

// The name the core's Updater stages under and the boot stage opens. No
// leading slash, deliberately: PicoOTA::addFile() opens it as given and
// puts the same string in the command page.
static const char IMAGE_FILE[] = "firmware.bin";

// The band, spec section 3.3. A build of this sketch is about 248 kB.
static const size_t MIN_SIZE  = 100000;
static const size_t MAX_SIZE  = 1048576;
static const size_t FS_MARGIN = 65536;

// What checkimage.sh greps for on the host. The literal below is also in
// the image once more, which is harmless.
static const char BANNER_FORMAT[] = "miniWorld lighting controller %s (%s)";

// The scan reads the file in pieces and carries the tail of one into the
// next, so a needle straddling a boundary is still found. OVERLAP must be
// at least one less than the longest needle: the banner is 37 bytes and a
// build string is capped at 63.
static const size_t SCAN_CHUNK   = 4096;
static const size_t SCAN_OVERLAP = 63;
static const size_t BUILD_MAX    = 64;

static File       s_file;
static MD5Builder s_md5;
static size_t     s_expected    = 0;
static size_t     s_received    = 0;
static bool       s_open        = false;
static bool       s_writeFailed = false;
static char       s_expectMd5[33];
static char       s_expectBuild[BUILD_MAX];
static uint8_t    s_scan[SCAN_CHUNK + SCAN_OVERLAP];

static void mountFs() {
    static bool begun = false;
    if (!begun) {
        LittleFS.begin();
        begun = true;
    }
}

static int reply(int code, const char *msg, String &body) {
    JsonDocument doc;
    doc["error"] = msg;
    body = "";
    serializeJson(doc, body);
    return code;
}

static bool isHex32(const String &s) {
    if (s.length() != 32) {
        return false;
    }
    for (unsigned i = 0; i < 32; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

// Closes and removes the staged file and forgets the upload.
static void discard() {
    if (s_file) {
        s_file.close();
    }
    LittleFS.remove(IMAGE_FILE);
    s_open = false;
    s_expected = s_received = 0;
}

static bool contains(const uint8_t *hay, size_t hayLen, const char *needle, size_t n) {
    if (n == 0 || hayLen < n) {
        return false;
    }
    for (size_t i = 0; i + n <= hayLen; i++) {
        if (hay[i] == (uint8_t)needle[0] && memcmp(hay + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool fileContains(File &f, const char *needle) {
    size_t n = strlen(needle);
    size_t carry = 0;
    f.seek(0);
    for (;;) {
        int got = f.read(s_scan + carry, SCAN_CHUNK);
        if (got <= 0) {
            return false;
        }
        size_t total = carry + (size_t)got;
        if (contains(s_scan, total, needle, n)) {
            return true;
        }
        carry = (total < SCAN_OVERLAP) ? total : SCAN_OVERLAP;
        memmove(s_scan, s_scan + total - carry, carry);
    }
}

size_t FirmwareUpdate::minSize() {
    return MIN_SIZE;
}

size_t FirmwareUpdate::maxSize() {
    mountFs();
    FSInfo info;
    if (!LittleFS.info(info) || info.totalBytes <= info.usedBytes) {
        return 0;
    }
    uint64_t free = info.totalBytes - info.usedBytes;
    if (free <= FS_MARGIN) {
        return 0;
    }
    free -= FS_MARGIN;
    return (free < MAX_SIZE) ? (size_t)free : MAX_SIZE;
}

int FirmwareUpdate::handle(const String &method, const String &path,
                           const String &requestBody, String &body) {
    (void)path;
    (void)requestBody;
    if (method != "GET") {
        return reply(405, "method not allowed", body);
    }
    mountFs();
    FSInfo info;
    JsonDocument doc;
    if (LittleFS.info(info)) {
        doc["fsTotal"] = (uint32_t)info.totalBytes;
        doc["fsFree"]  = (uint32_t)(info.totalBytes > info.usedBytes
                                    ? info.totalBytes - info.usedBytes : 0);
    } else {
        doc["fsTotal"] = 0;
        doc["fsFree"]  = 0;
    }
    doc["maxSize"] = (uint32_t)maxSize();
    doc["minSize"] = (uint32_t)MIN_SIZE;
    doc["staged"]  = LittleFS.exists(IMAGE_FILE);
    body = "";
    serializeJson(doc, body);
    return 200;
}

int FirmwareUpdate::begin(size_t length, const String &md5, const String &build, String &body) {
    if (s_open) {
        return reply(409, "upload in progress", body);
    }
    if (!isHex32(md5)) {
        return reply(400, "missing X-Firmware-MD5", body);
    }
    if (build.length() == 0 || build.length() >= BUILD_MAX) {
        return reply(400, "missing X-Firmware-Build", body);
    }
    size_t max = maxSize();
    if (length < MIN_SIZE || length > max) {
        JsonDocument doc;
        doc["error"] = "payload too large";
        doc["max"] = (uint32_t)max;
        body = "";
        serializeJson(doc, body);
        return 413;
    }
    mountFs();
    LittleFS.remove(IMAGE_FILE);
    s_file = LittleFS.open(IMAGE_FILE, "w");
    if (!s_file) {
        return reply(500, "filesystem write failed", body);
    }
    s_md5.begin();
    strncpy(s_expectMd5, md5.c_str(), 32);
    s_expectMd5[32] = 0;
    strncpy(s_expectBuild, build.c_str(), BUILD_MAX - 1);
    s_expectBuild[BUILD_MAX - 1] = 0;
    s_expected = length;
    s_received = 0;
    s_writeFailed = false;
    s_open = true;
    Serial.printf("firmware: upload of %u bytes, build %s\n", (unsigned)length, s_expectBuild);
    return 0;
}

bool FirmwareUpdate::write(const uint8_t *data, size_t len) {
    if (!s_open || s_writeFailed) {
        return false;
    }
    if (s_file.write(data, len) != len) {
        s_writeFailed = true;
        return false;
    }
    s_md5.add(data, (uint16_t)len);     // pieces are 2 kB, well inside uint16_t
    s_received += len;
    return true;
}

int FirmwareUpdate::end(String &body) {
    if (!s_open) {
        return reply(409, "no upload in progress", body);
    }
    if (s_writeFailed) {
        discard();
        Serial.println("firmware: rejected, filesystem write failed");
        return reply(500, "filesystem write failed", body);
    }
    s_file.close();
    if (s_received != s_expected) {
        discard();
        Serial.println("firmware: rejected, short body");
        return reply(422, "short body", body);
    }
    s_md5.calculate();
    String got = s_md5.toString();      // lower-case hex
    if (!got.equalsIgnoreCase(s_expectMd5)) {
        discard();
        Serial.println("firmware: rejected, md5 mismatch");
        return reply(422, "md5 mismatch", body);
    }
    File f = LittleFS.open(IMAGE_FILE, "r");
    if (!f) {
        discard();
        return reply(500, "filesystem write failed", body);
    }
    bool banner = fileContains(f, BANNER_FORMAT);
    bool stamp  = banner && fileContains(f, s_expectBuild);
    f.close();
    if (!banner) {
        discard();
        Serial.println("firmware: rejected, not this sketch");
        return reply(422, "not this sketch", body);
    }
    if (!stamp) {
        discard();
        Serial.println("firmware: rejected, build stamp not in image");
        return reply(422, "build stamp not in image", body);
    }
    picoOTA.begin();
    if (!picoOTA.addFile(IMAGE_FILE) || !picoOTA.commit()) {
        discard();
        Serial.println("firmware: rejected, commit failed");
        return reply(500, "commit failed", body);
    }
    Serial.printf("firmware: staged %u bytes, build %s, rebooting into it\n",
                  (unsigned)s_received, s_expectBuild);
    JsonDocument doc;
    doc["ok"]    = true;
    doc["size"]  = (uint32_t)s_received;
    doc["md5"]   = got;
    doc["build"] = s_expectBuild;
    body = "";
    serializeJson(doc, body);
    s_open = false;
    SystemWebApi::requestReboot();
    return 200;
}

void FirmwareUpdate::abort() {
    if (s_open) {
        Serial.println("firmware: upload dropped");
    }
    discard();
}

void FirmwareUpdate::cleanupAtBoot() {
    mountFs();
    if (LittleFS.exists(IMAGE_FILE)) {
        LittleFS.remove(IMAGE_FILE);
        Serial.println("firmware: removed staged image");
    }
}
```

- [ ] **Step 3: Add `requestReboot()` to SystemWebApi**

In `SystemWebApi.h`, under `rebootPending()`:

```cpp
    // Sets the same flag from outside a handler; FirmwareUpdate uses it
    // after staging an image so the 200 goes out before the reboot.
    static void requestReboot();
```

And in the header comment's route list, after the reboot line:

```
      GET  /api/system/firmware  filesystem headroom for an upload
      POST /api/system/firmware  a firmware image over the air; both are
                                 handled by FirmwareUpdate, see its header
```

In `SystemWebApi.cpp`, after `rebootPending()`:

```cpp
void SystemWebApi::requestReboot() {
    reboot_pending = true;
}
```

- [ ] **Step 4: Call the cleanup from `setup()`**

In `miniWorld_LightingController.ino`, add `#include "FirmwareUpdate.h"` after `#include "HttpServer.h"`, and right after the banner `Serial.printf(...)` line:

```cpp
    FirmwareUpdate::cleanupAtBoot();   // the image just copied in, or a half upload
```

- [ ] **Step 5: Build**

Run: `make` from the repo root.
Expected: the compile ends with `Sketch uses ... bytes` and `Global variables use ... bytes`, no warning lines. Note both numbers for HANDOFF (Task 7). If `PicoOTA.h` is not found, the include path is `<PicoOTA.h>` from the core's `libraries/PicoOTA/src`; arduino-cli picks it up from the include alone.

- [ ] **Step 6: Commit**

```bash
git add miniWorld_LightingController/FirmwareUpdate.h miniWorld_LightingController/FirmwareUpdate.cpp \
        miniWorld_LightingController/SystemWebApi.h miniWorld_LightingController/SystemWebApi.cpp \
        miniWorld_LightingController/miniWorld_LightingController.ino
git commit -m "FirmwareUpdate: stage, verify and commit an image for the OTA boot stage

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019ZqMmutmHXQhskefHF1Hu9"
```

---

### Task 4: HttpServer streams the upload

**Files:**
- Modify: `miniWorld_LightingController/HttpServer.h` (header comment, `Request`, one private method)
- Modify: `miniWorld_LightingController/HttpServer.cpp` (limits near line 22, include near line 12, `readRequest` near lines 224 and 249, the body read near line 273, `tick()` dispatch near line 481, new `streamFirmware`)

**Interfaces:**
- Consumes: every `FirmwareUpdate` static from Task 3.
- Produces: the live `POST` and `GET /api/system/firmware` that Task 5's tool and Task 6's bench use.

- [ ] **Step 1: Header changes**

In `HttpServer.h`, add to `Request`:

```cpp
    struct Request {
        String method, path, host, auth, ifNoneMatch, body;
        String firmwareMd5, firmwareBuild;   // X-Firmware-MD5, X-Firmware-Build
        size_t contentLength = 0;
        bool tooLarge = false, headerTooLong = false, badRequest = false;
        bool firmwareUpload = false;         // POST to the firmware path: body streamed, not read here
    };
```

Add after `void sendUi(WiFiClient &c, const Request &r);`:

```cpp
    // The one route whose body is not a String: reads it in pieces into
    // FirmwareUpdate and sends the code end() returns.
    void streamFirmware(WiFiClient &c, const Request &r);
```

In the header comment, change the "Body up to 16384 bytes" bullet to:

```
      - Body up to 16384 bytes. More gives 413 and the body is not read.
        The one exception is POST /api/system/firmware, whose body is a
        firmware image: its Content-Length must fall in FirmwareUpdate's
        band, and after the routing chain has passed (so a 401 or 413 is
        answered from the headers alone) the body is streamed in 2 kB
        pieces into FirmwareUpdate under a 120 s deadline. Scene.tick()
        does not run for the duration; the lamps hold their levels.
```

And in the routing list, item 7:

```
      7. /api/system/firmware to FirmwareUpdate (GET as a plain handler,
         POST streamed), then /api/lamps/*, /api/scene/*, /api/net/*,
         /api/system/* to the handler whose owns() matches, answered as
         application/json.
```

- [ ] **Step 2: Limits, include and buffer in the cpp**

After `#include "SystemWebApi.h"` add `#include "FirmwareUpdate.h"`.

After `static const size_t UI_CHUNK = 1024;` add:

```cpp
// The firmware upload: pieces of this size go straight to
// AT+CIPRECVDATA, one AT round trip per piece (WiFiEspAT hands a read
// larger than its own 64-byte buffer to the driver directly), and the
// whole body may take this long at 115200 with room to spare.
static const size_t   UPLOAD_CHUNK      = 2048;
static const uint32_t UPLOAD_TIMEOUT_MS = 120000;
static uint8_t s_uploadBuf[UPLOAD_CHUNK];
```

- [ ] **Step 3: readRequest knows the firmware path**

Right after the request line is parsed (after the `r.path.remove(q)` block, still inside the `else`), add:

```cpp
            r.firmwareUpload = (r.method == "POST") && FirmwareUpdate::owns(r.path);
```

Replace the `Content-Length` branch:

```cpp
        if (headerValue(line, "Content-Length", value)) {
            long n = value.toInt();
            bool inBand;
            if (r.firmwareUpload) {
                // The band of a real build, and what the filesystem can
                // take: a wrong file is refused before it is sent.
                inBand = n >= (long)FirmwareUpdate::minSize()
                      && (size_t)n <= FirmwareUpdate::maxSize();
            } else {
                inBand = n >= 0 && (size_t)n <= BODY_MAX;
            }
            if (!inBand) {
                r.tooLarge = true;
            } else {
                r.contentLength = (size_t)n;
            }
        } else if (headerValue(line, "X-Firmware-MD5", value)) {
            r.firmwareMd5 = value;
        } else if (headerValue(line, "X-Firmware-Build", value)) {
            r.firmwareBuild = value;
        } else if (headerValue(line, "Authorization", value)) {
```

(the rest of the chain unchanged). Then after the headers loop, a firmware POST with no `Content-Length` at all must also be 413, not a wait for a body that never comes:

```cpp
    if (r.firmwareUpload && r.contentLength == 0) {
        r.tooLarge = true;                  // no Content-Length, or zero: below the band
    }
    if (r.badRequest || r.headerTooLong || r.tooLarge) {
        return true;                        // the body is never read
    }

    if (r.contentLength > 0 && !r.firmwareUpload) {
```

- [ ] **Step 4: Dispatch and the streaming loop**

In `tick()`, the 413 for a firmware upload carries the ceiling, spec section 3.1. Replace the `r.tooLarge` branch:

```cpp
    } else if (r.tooLarge) {
        if (r.firmwareUpload) {
            out = "{\"error\":\"payload too large\",\"max\":";
            out += (unsigned)FirmwareUpdate::maxSize();
            out += '}';
            sendStatus(c, 413, JSON_TYPE, out);
        } else {
            sendStatus(c, 413, JSON_TYPE, "{\"error\":\"payload too large\"}");
        }
```

Then insert before `} else if (LampWebApi::owns(r.path)) {`:

```cpp
    } else if (r.firmwareUpload) {
        streamFirmware(c, r);
    } else if (FirmwareUpdate::owns(r.path)) {
        sendStatus(c, FirmwareUpdate::handle(r.method, r.path, r.body, out), JSON_TYPE, out);
```

Add the method, after `sendUi`:

```cpp
// The routing chain has passed: the client is authorised and the length
// is in the band. Feed the body to FirmwareUpdate and answer with what
// end() says. A client that goes quiet or away gets no reply, like any
// other dropped request, and the partial file is removed.
void HttpServer::streamFirmware(WiFiClient &c, const Request &r) {
    String out;
    int code = FirmwareUpdate::begin(r.contentLength, r.firmwareMd5, r.firmwareBuild, out);
    if (code != 0) {
        sendStatus(c, code, JSON_TYPE, out);
        return;
    }
    _requestDeadline = millis() + UPLOAD_TIMEOUT_MS;
    uint32_t idle = millis() + READ_TIMEOUT_MS;
    uint32_t nextPoll = millis() + CONN_POLL_MS;
    size_t received = 0;
    while (received < r.contentLength) {
        if (!waitReadable(c, idle, nextPoll)) {
            FirmwareUpdate::abort();
            return;
        }
        size_t want = r.contentLength - received;
        if (want > UPLOAD_CHUNK) {
            want = UPLOAD_CHUNK;
        }
        int got = c.read(s_uploadBuf, want);
        if (got <= 0) {
            if (expired(_requestDeadline)) {
                FirmwareUpdate::abort();
                return;
            }
            yield();
            continue;
        }
        if (!FirmwareUpdate::write(s_uploadBuf, (size_t)got)) {
            break;                          // end() answers 500; the rest is not read
        }
        received += (size_t)got;
        idle = millis() + READ_TIMEOUT_MS;
    }
    sendStatus(c, FirmwareUpdate::end(out), JSON_TYPE, out);
}
```

- [ ] **Step 5: Build**

Run: `make`.
Expected: warning-free; note the RAM figure (about 2 kB above Task 3's).

- [ ] **Step 6: Commit**

```bash
git add miniWorld_LightingController/HttpServer.h miniWorld_LightingController/HttpServer.cpp
git commit -m "HttpServer: stream POST /api/system/firmware into FirmwareUpdate

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019ZqMmutmHXQhskefHF1Hu9"
```

---

### Task 5: tools/ota.sh and make ota

**Files:**
- Create: `tools/ota.sh`
- Modify: `Makefile` (comment block near line 10, `.PHONY` near line 68, a target after `upload` near line 103, `HOST ?=` near `PORT ?=`)
- Modify: `CLAUDE.md` (the make target list near line 40; the file is git-ignored globally, edit it anyway)

**Interfaces:**
- Consumes: Task 1's mock (for the off-target run), Task 3 and 4 on the device.
- Produces: `make ota HOST=<address>`; `tools/ota.sh <build-dir> <host>` with `OTA_PASSWORD` in the environment.

- [ ] **Step 1: Write `tools/ota.sh`**

```sh
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

# curl with the device password when one is set; the password never
# appears on a command line of this script's own.
curl_auth() {
    if [ -n "${OTA_PASSWORD:-}" ]; then
        curl -u ":$OTA_PASSWORD" "$@"
    else
        curl "$@"
    fi
}

json_field() {   # json_field <name> reads stdin, prints the value of "name"
    sed -n 's/.*"'"$1"'":"\{0,1\}\([^",}]*\)"\{0,1\}.*/\1/p'
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
[ -n "$max" ] || fail "unexpected answer from the device: $pre"
[ "$size" -le "$max" ] || fail "image is $size bytes, the device takes at most $max ($free bytes free)"
echo "ota: sending $size bytes, build $stamp, to $host ($free bytes free)"

# Expect: is emptied because the device does not answer 100-continue and
# curl would otherwise wait a second before sending the body.
resp=$(curl_auth -s -m 180 -w '\n%{http_code}' -X POST --data-binary "@$bin" \
    -H 'Content-Type: application/octet-stream' \
    -H "X-Firmware-MD5: $md5" -H "X-Firmware-Build: $stamp" -H 'Expect:' \
    "http://$host/api/system/firmware")
code=$(printf '%s\n' "$resp" | tail -1)
body=$(printf '%s\n' "$resp" | sed '$d')
[ "$code" = "200" ] || fail "device answered $code: $body"
echo "ota: device staged the image: $body"

# The reboot, the copy (a 248 kB image is about a second) and the ESP
# bring-up. The stamp is the test, not a before/after difference, so
# re-sending the running image passes too.
end=$(( $(date +%s) + 90 ))
build=""
sleep 3
while [ "$(date +%s)" -lt "$end" ]; do
    build=$(curl_auth -s -m 5 "http://$host/api/system/status" | json_field build)
    if [ "$build" = "$stamp" ]; then
        echo "ota: verified, board at $host runs build $build"
        exit 0
    fi
    sleep 2
done
fail "board at $host did not report build '$stamp' within 90 s (last seen '${build:-nothing}'); if it stays unreachable, recover over USB with make upload"
```

Then `chmod +x tools/ota.sh`.

Note on `json_field`: the status JSON is flat and the stamp holds spaces and colons but no comma, quote or brace, so the sed is enough; the same sed shape is what `flash.sh` uses.

- [ ] **Step 2: Makefile target**

In the comment block after the `make upload` lines:

```
#   make ota HOST=x   compile, check the image, send it over WiFi to the
#                     board at HOST, verify the build stamp through the API
```

After `PORT        ?=` add `HOST        ?=`. Add `ota` to `.PHONY`. After the `upload:` rule:

```make
# The air path. OTA_PASSWORD in the environment when the device has a
# GUI password. The same checkimage guard runs first, and the same
# build-stamp test runs afterwards through the API.
ota: compile
	@test -n "$(HOST)" || { echo "make ota needs HOST=<ip or miniworld.local>"; exit 1; }
	tools/ota.sh $(BUILD_PATH) "$(HOST)"
```

In `CLAUDE.md`'s make list after `make erase-net`:

```
make ota HOST=x   # compile, check, send over WiFi, verify the stamp through the API
```

- [ ] **Step 3: Run the tool end to end against the mock**

With `make mock` running and a fresh `make` done (so `checkimage.sh` passes on `./build`):

Run: `tools/ota.sh build localhost:8080`
Expected: `ota: sending 24xxxx bytes, build <stamp>, to localhost:8080 (...)`, then `ota: device staged the image: {"ok": true, ...}`, then after about three seconds `ota: verified, board at localhost:8080 runs build <stamp>`; exit 0. This proves the MD5, the banner scan and the stamp scan against a real `.bin`, since the mock runs the same checks.

Run: `make ota` with no HOST.
Expected: `make ota needs HOST=<ip or miniworld.local>`, exit 1.

- [ ] **Step 4: Commit**

```bash
git add tools/ota.sh Makefile
git commit -m "make ota: the air path, guarded like make upload

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019ZqMmutmHXQhskefHF1Hu9"
```

(`CLAUDE.md` is git-ignored and stays local.)

---

### Task 6: Bench verification on the Challenger

**Files:** none changed; this task produces the facts Task 7 writes down.

**Interfaces:**
- Consumes: everything above, flashed to the board.

The board is on the bench at 192.168.1.180 (memory: bench state 2026-09-07). Only that board may be flashed. `OTA_PASSWORD` is needed if the GUI has a password set; check with `curl -s -o /dev/null -w '%{http_code}' http://192.168.1.180/api/system/status` (401 means yes).

- [ ] **Step 1: Put the new image on over USB, once**

Run: `make upload` with the board on USB.
Expected: `flash: verified through the console` (or the API). The boot log shows the banner; there is no `firmware: removed staged image` line yet since nothing was staged.

- [ ] **Step 2: GET reports the filesystem**

Run: `curl -s http://192.168.1.180/api/system/firmware`
Expected: `fsTotal` 1048576, `fsFree` near 1 MB, `maxSize` around 900000, `staged:false`.

- [ ] **Step 3: Re-send the running image (Review Focus 5)**

Run: `time make ota HOST=192.168.1.180`
Expected: `ota: verified ...` with the same stamp; the console (in `make console` in another terminal, opened before) shows `firmware: upload of ... bytes`, `firmware: staged ...`, the reboot, and at boot `firmware: removed staged image`. Note the wall time of the upload phase and the `net: esp link` baud in the boot log.

- [ ] **Step 4: A real change over the air**

Touch a comment in any source (for example add a blank line to `FirmwareUpdate.cpp` and remove it again, then `make` gives a new stamp anyway since the stamp header is rewritten on every compile). Run: `make ota HOST=192.168.1.180`.
Expected: `ota: verified ... runs build <new stamp>`; the console banner carries the new stamp; `curl -s http://192.168.1.180/api/system/status` shows it.

- [ ] **Step 5: Negative cases**

Each from the repo root with `B=build/miniWorld_LightingController.ino.bin` and `S=$(head -1 build/.stamp)`:

1. Wrong MD5:
   `curl -s -w '\n%{http_code}\n' -X POST --data-binary @$B -H 'Expect:' -H 'X-Firmware-MD5: 00000000000000000000000000000000' -H "X-Firmware-Build: $S" http://192.168.1.180/api/system/firmware`
   Expected: `{"error":"md5 mismatch"}` and 422; then `GET /api/system/firmware` shows `staged:false` and `fsFree` back where it was.
2. Absent `Content-Length` (Review Focus 1):
   `curl -s -w '\n%{http_code}\n' -X POST -H 'Transfer-Encoding: chunked' --data-binary @$B -H 'Expect:' -H 'X-Firmware-MD5: 00000000000000000000000000000000' -H "X-Firmware-Build: $S" http://192.168.1.180/api/system/firmware`
   Expected: 413 at once, no long wait.
3. Truncated body (Review Focus 4):
   `curl -s -m 2 -X POST --data-binary @$B -H 'Expect:' -H "X-Firmware-MD5: $(md5sum $B | cut -d' ' -f1)" -H "X-Firmware-Build: $S" http://192.168.1.180/api/system/firmware; echo "exit $?"`
   Expected: curl exits 28 (timeout), the console says `firmware: upload dropped`, `GET` shows `staged:false`, and a following `make ota HOST=192.168.1.180` succeeds.
4. A foreign file of the right size:
   `head -c 200000 /dev/urandom > /tmp/notfw.bin; curl -s -w '\n%{http_code}\n' -X POST --data-binary @/tmp/notfw.bin -H 'Expect:' -H "X-Firmware-MD5: $(md5sum /tmp/notfw.bin | cut -d' ' -f1)" -H "X-Firmware-Build: $S" http://192.168.1.180/api/system/firmware`
   Expected: `{"error":"not this sketch"}` and 422.
5. Oversize length:
   `curl -s -w '\n%{http_code}\n' -X POST -H 'Content-Length: 2000000' -H 'Expect:' -H 'X-Firmware-MD5: 00000000000000000000000000000000' -H "X-Firmware-Build: $S" --data-binary @$B http://192.168.1.180/api/system/firmware`
   Expected: 413 before any transfer (curl may report a write error after the reply, which is fine).
6. Upper-case MD5 (Review Focus 2):
   `make ota` cannot produce one, so: `curl -s -w '\n%{http_code}\n' -X POST --data-binary @$B -H 'Expect:' -H "X-Firmware-MD5: $(md5sum $B | cut -d' ' -f1 | tr a-f A-F)" -H "X-Firmware-Build: $S" http://192.168.1.180/api/system/firmware`
   Expected: 200 and a reboot into the same image.

Add `-u ":$OTA_PASSWORD"` to each if the device has a password.

- [ ] **Step 6: USB still works**

Run: `make upload`.
Expected: verified as in step 1. Record: upload duration from step 3, the baud line, the RAM and flash figures from the last `make`, and anything that did not behave as this plan says.

---

### Task 7: HANDOFF and the file inventory

**Files:**
- Modify: `HANDOFF.md` (§2 layering table near line 36, §3 inventory near lines 85 to 111, a new §5.9 after §5.8 near line 297, §6 near line 327, §8 at the end)
- Copy: `miniWorld_LightingController/HANDOFF.md` (byte-identical)

**Interfaces:**
- Consumes: Task 6's measurements.

- [ ] **Step 1: Layering table**

Under `SystemWebApi            /api/system/*` add:

```
  FirmwareUpdate          /api/system/firmware: an image into LittleFS, verified, then the core's OTA boot stage
```

- [ ] **Step 2: Inventory rows**

Change the `HttpServer.h/.cpp` row's purpose to `Parsing, basic auth, routing, the SPA from flash, and the one streamed body: the firmware upload`. Change the `SystemWebApi.h/.cpp` row's purpose to `` `/api/system/status, reboot`; `firmware` is FirmwareUpdate's ``. Add after it:

```
| `FirmwareUpdate.h/.cpp` | `/api/system/firmware`: GET reports filesystem headroom; POST is streamed by HttpServer into `firmware.bin`, MD5 and the banner and build stamp checked, then PicoOTA's command page and a reboot | Done, verified on the board 2026-09-23, see §5.9 |
```

Change the `tools/` row to include `ota.sh`; change the `tools/flash.sh, ...` row to add `ota.sh: the air path with the same checks`; update the `tools/test_mockserver.py` row count (nine becomes the new total, with "the firmware upload checks" in the list); the `Makefile` row gains `make ota`.

- [ ] **Step 3: §5.9**

After §5.8:

```
### 5.9 Firmware over the air (done 2026-09-23: <the facts from Task 6: upload time at <baud>, the re-send of the running image, a real change, the six negative cases, USB afterwards>)

Spec: `docs/superpowers/specs/2026-09-23-firmware-update-design.md`. The
push model: `make ota HOST=<address>` runs `checkimage.sh`, POSTs the
`.bin` with its MD5 and build stamp in two headers, and polls the status
for the stamp. The device stores the image in LittleFS, checks the MD5,
scans the bytes for the banner and the stamp, writes the OTA command
through the core's PicoOTA and reboots; the arduino-pico boot stage in
every image does the copy. The scene freezes for the upload; there is no
rollback, USB recovers a bad image. GUI upload page: a later round.
```

Fill the angle brackets with the measured facts; no placeholders remain.

- [ ] **Step 4: §6 and §8**

In §6, after the 921600 bullet:

```
- **The OTA boot stage.** Every image links the core's `ota.o`, and
  before 2026-09-23 it had never been exercised on this board. It is now:
  the copy of a 248 kB image and the reboot took <n> s on the bench. What
  is still unobserved is power loss during the copy, which the stage's
  own design says restarts the copy at the next boot.
```

Update the RAM bullet's figures with the last build's `Sketch uses` and `Global variables use` lines, and add: `FirmwareUpdate adds a 4 kB scan buffer and HttpServer a 2 kB receive buffer, both static.`

At the end of §8:

```
- **Why the firmware goes over the air as a plain .bin and not gzip.**
  gzip takes the image from 248 kB to 180 kB, a few seconds on the UART,
  but hides the banner and stamp strings from the device's identity
  check and makes the MD5 cover the archive rather than the bytes that
  reach flash. The check is worth more than the seconds. And why not the
  core's `Update` class: it commits the OTA command inside `end()` with
  no room for that check, so `FirmwareUpdate` writes the file itself and
  uses `MD5Builder` and `PicoOTA` directly, which is all `Update` does on
  RP2040 apart from a buffer.
```

- [ ] **Step 5: Copy and check**

```bash
cp HANDOFF.md miniWorld_LightingController/HANDOFF.md
diff -q HANDOFF.md miniWorld_LightingController/HANDOFF.md && grep -c "—" HANDOFF.md
```

Expected: no diff output, and `0` em dashes.

- [ ] **Step 6: Commit**

```bash
git add HANDOFF.md miniWorld_LightingController/HANDOFF.md
git commit -m "HANDOFF 5.9: firmware over the air verified on the board

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019ZqMmutmHXQhskefHF1Hu9"
```
