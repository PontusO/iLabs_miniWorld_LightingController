# Firmware Upload From the GUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A System tab in the web GUI that shows the device, the filesystem headroom, sends a `.bin` from `./build` to the board over WiFi with the same checks `make ota` makes, waits for the board to come back on the new build, and offers a reboot button.

**Architecture:** One new view, `web/view-system.js` with `web/view-system.css`, registered with the existing hash router like the six views before it. The browser does the checking (band, banner, one build stamp, MD5) before a plain `XMLHttpRequest` streams the file to the verified device route; the firmware is not touched. The mock gains a 3 s delay before it reports the new build and an uptime reset on reboot, so both waits run off-target.

**Tech Stack:** Plain ES5 JavaScript and CSS inlined by `tools/buildweb.py` (no framework, no CDN), Python 3 `http.server` and `unittest` for the mock, the arduino-pico firmware from the previous round unchanged.

**Spec:** `docs/superpowers/specs/2026-09-23-firmware-gui-design.md`

## Global Constraints

- No em dashes anywhere: prose, comments, JavaScript, CSS, Python, HTML, HANDOFF.
- Every new source file's header comment ends with `Invector Embedded Systems AB`.
- The GUI is one gzipped file: edit `web/`, never `WebUI.gen.h`; `make web` prints the gzipped size and fails above the 40 kB budget (40960 bytes). Today it is 29927 bytes.
- Nothing served from the device fetches anything from outside: the MD5 routine is inline in `view-system.js`, no web fonts, no CDN.
- The device contract is unchanged and is the authority: `POST /api/system/firmware` with `X-Firmware-MD5` (32 hex digits) and `X-Firmware-Build`, answers 200 `{"ok":true,"size":n,"md5":"...","build":"..."}`, 400 `missing X-Firmware-MD5` | `missing X-Firmware-Build` | `X-Firmware-Build longer than 63 characters`, 413 `{"error":"payload too large","max":n}`, 422 `md5 mismatch` | `not this sketch` | `build stamp not in image` | `short body`, 500 `commit failed`. `GET /api/system/firmware` answers `{"fsTotal","fsFree","maxSize","minSize","staged"}`. `GET /api/system/status` answers `{"firmware","build","uptime","heap","time","timeValid"}`. `POST /api/system/reboot` answers 200 and reboots 200 ms after the reply.
- The wait after an upload: first poll after 3000 ms, then every 2000 ms, 90000 ms at most; after a reboot 60000 ms at most. The send timeout is 180000 ms.
- The build stamp regular expression is exactly `/[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}/g` and must match exactly once.
- The banner string is exactly `miniWorld lighting controller %s (%s)`.
- The status line texts are the ones in the spec's section 4 and in Task 2's code; do not reword them.
- `make` at the repo root must stay warning-free (it is the lint for the embedded header too).
- Only the Challenger on the bench at 192.168.1.180 may be flashed.

## Review Focus

1. An image of another sketch that happens to contain a stamp-shaped string: the banner check must refuse it before anything is sent. Pinned in Task 3 (the doctored file with the banner bytes overwritten).
2. A build with a single-digit day, `Sep  6 2026 07:08:09` with two spaces: the regex must still find exactly one stamp. Pinned in Task 2's verify (the node check of `STAMP_RE` against that string).
3. Leaving the System tab during a send or a wait and coming back: the status line must show the current state and `Send` must still be disabled. Pinned in Task 3.
4. A board that takes longer than the first 3 s poll but less than 90 s, answering nothing meanwhile: the wait must keep polling through failed requests, not stop at the first error. Pinned in Task 2's code (`waitFor` calls `again` on rejection) and Task 4's bench run where the board takes about 9 s.
5. A second `Send` pressed while a wait is running: it must do nothing. Pinned in Task 2's `send()` guard and Task 3's step that clicks it.

---

### Task 1: The mock's two waits

**Files:**
- Modify: `tools/mockserver.py` (`FirmwareState`, `system_status_json()`, the `/api/system/reboot` branch of `_api`)
- Test: `tools/test_mockserver.py` (two tests appended to `FirmwareTest`, before the `if __name__ == "__main__":` guard)

**Interfaces:**
- Produces: `FirmwareState.reported_build(now=None) -> str`, the build the status reports at time `now` (default `time.time()`): the previous build until `build_at`, the uploaded one after. `FirmwareState.build_at` (float, epoch seconds, 0 at start), `FirmwareState.previous` (str). `mockserver.mock_reboot()` resets `BOOT_TIME` so the uptime starts again. `FIRMWARE_REBOOT_S = 3`.
- Consumed by: Task 2's page, which waits for the stamp and for the uptime to drop; Task 3's browser run.

- [ ] **Step 1: Write the failing tests**

Add to `class FirmwareTest` in `tools/test_mockserver.py`, after `test_finds_stamp_across_chunk_boundary` and before the `if __name__ == "__main__":` guard:

```python
    def test_status_reports_the_new_build_after_the_reboot_delay(self):
        fw = mockserver.FirmwareState()
        body = _image(self.STAMP)
        fw.upload(body, _md5(body), self.STAMP)
        # The mock "reboots" for FIRMWARE_REBOOT_S: the status keeps the old
        # build until then, so a page that waits for the stamp has
        # something to wait for.
        self.assertEqual(fw.reported_build(fw.build_at - 0.1), "mock")
        self.assertEqual(fw.reported_build(fw.build_at), self.STAMP)
        self.assertGreaterEqual(fw.build_at - time.time(), mockserver.FIRMWARE_REBOOT_S - 1)

    def test_reboot_resets_the_uptime(self):
        was = mockserver.BOOT_TIME
        try:
            mockserver.BOOT_TIME = time.time() - 500
            self.assertGreaterEqual(mockserver.system_status_json()["uptime"], 499)
            mockserver.mock_reboot()
            self.assertLessEqual(mockserver.system_status_json()["uptime"], 1)
        finally:
            mockserver.BOOT_TIME = was
```

The test file does not import `time` yet: add `import time` to the top-level imports, after `import sys`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 -m unittest tools.test_mockserver.FirmwareTest -v 2>&1 | tail -6`
Expected: the two new tests error with `AttributeError` (`reported_build`, `mock_reboot`), the other nine pass.

- [ ] **Step 3: Implement the delay and the reset**

In `tools/mockserver.py`, after `FIRMWARE_SCAN_OVERLAP = 63` add:

```python
FIRMWARE_REBOOT_S = 3   # the status keeps the old build this long after an upload
```

In `FirmwareState.__init__` replace the body with:

```python
        self.build = "mock"
        self.previous = "mock"
        self.build_at = 0.0
        self.staged = False
```

Add this method after `info_json()`:

```python
    def reported_build(self, now=None):
        """The build /api/system/status shows: the previous one until
        build_at, then the uploaded one. The device is rebooting in that
        gap, and a page that waits for the new stamp must see the gap."""
        if now is None:
            now = time.time()
        return self.build if now >= self.build_at else self.previous
```

In `upload()`, replace the two lines

```python
        self.build = build
        self.staged = False   # the device reboots and cleans up at boot
```

with

```python
        self.previous = self.reported_build()
        self.build = build
        self.build_at = time.time() + FIRMWARE_REBOOT_S
        self.staged = False   # the device reboots and cleans up at boot
```

In `system_status_json()` change `"build": FIRMWARE.build,` to `"build": FIRMWARE.reported_build(),`.

After `system_status_json()` add:

```python
def mock_reboot():
    """What the reboot route does here: the uptime starts again, so a
    page that waits for a smaller uptime sees one at once."""
    global BOOT_TIME
    BOOT_TIME = time.time()
```

In `_api`, the `/api/system/reboot` branch, replace

```python
            if method == "POST":
                sys.stderr.write("mock: reboot requested (ignored)\n")
                return 200, {"ok": True}
```

with

```python
            if method == "POST":
                mock_reboot()
                sys.stderr.write("mock: reboot requested, uptime reset\n")
                return 200, {"ok": True}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `python3 -m unittest tools/test_mockserver.py 2>&1 | tail -3`
Expected: `Ran 27 tests`, `OK`.

- [ ] **Step 5: The tool still works against the mock**

Run: `python3 tools/mockserver.py --port 8102 >/dev/null 2>&1 & MOCK=$!; sleep 1; tools/ota.sh build localhost:8102; echo "ota exit=$?"; kill $MOCK`
Expected: `ota: verified, board at localhost:8102 runs build ...` after about three seconds of polling, `ota exit=0`. (If `./build` has no image, run `make` first.)

- [ ] **Step 6: Commit**

```bash
git add tools/mockserver.py tools/test_mockserver.py
git commit -m "Mock: the status lags an upload by 3 s and a reboot resets the uptime

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01VV9r86jJQiHpXtoW9xA7sE"
```

---

### Task 2: The System view

**Files:**
- Create: `web/view-system.js`
- Create: `web/view-system.css`
- Modify: `web/app.js` (add `fmtUptime` next to `fmtTime`, export it on `window.App`)
- Modify: `web/view-home.js` (drop its private `fmtUptime`, use `App.fmtUptime`)
- Modify: `web/index.html` (one nav line, "six" becomes "seven" in the header comment)

**Interfaces:**
- Consumes: `App.el`, `App.api`, `App.toast`, `App.register` from `web/app.js`; the mock or device routes listed in Global Constraints.
- Produces: `App.fmtUptime(seconds) -> string` (`"3d 2h"`, `"2h 5m"`, `"7m"`, `"12s"`); the view registered as `"system"`; the CSS class names `.view-system`, `.status`, `.bar`, `.fill`, `.room`, `.file-row`, `.ask`, `.btn-row`, `.btn.danger` used only inside `.view-system`.

- [ ] **Step 1: Move the uptime formatter into the shell**

In `web/app.js`, after the `pad2()` function add:

```javascript
    // Seconds of uptime as a short readout: days and hours, hours and
    // minutes, minutes, or seconds, whichever two units matter.
    function fmtUptime(seconds) {
        var s = Math.max(0, Math.floor(seconds || 0));
        var d = Math.floor(s / 86400);
        var h = Math.floor((s % 86400) / 3600);
        var m = Math.floor((s % 3600) / 60);
        if (d > 0) return d + "d " + h + "h";
        if (h > 0) return h + "h " + m + "m";
        if (m > 0) return m + "m";
        return s + "s";
    }
```

In the `window.App = { ... }` object at the end of `app.js`, add `fmtUptime: fmtUptime,` after `fmtTime: fmtTime,`.

In `web/view-home.js`, delete the whole `function fmtUptime(seconds) { ... }` (about lines 125 to 134, ten lines) and change the one call `fmtUptime(s.uptime)` to `App.fmtUptime(s.uptime)`.

- [ ] **Step 2: The nav line**

In `web/index.html`, after `        <a href="#houses" data-view="houses">Houses</a>` add:

```html
        <a href="#system" data-view="system">System</a>
```

and in the header comment change `the header, the six view` to `the header, the seven view`.

- [ ] **Step 3: Write `web/view-system.js`**

```javascript
// view-system.js - System view: the device line, the filesystem headroom
//                  and the firmware upload, and a reboot button. The
//                  upload does in the browser what tools/ota.sh does on
//                  the host: band, banner, one build stamp and MD5 before
//                  a byte is sent, then the POST with progress and the
//                  wait for the board to come back on the new stamp.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el, api = App.api;

    var BANNER = "miniWorld lighting controller %s (%s)";
    var STAMP_RE = /[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}/g;
    var SEND_TIMEOUT_MS = 180000;
    var WAIT_FIRST_MS = 3000;
    var WAIT_STEP_MS = 2000;
    var WAIT_MAX_MS = 90000;
    var REBOOT_MAX_MS = 60000;

    // The upload outlives the view: leaving the tab does not cancel the
    // request, and coming back shows where it stands. Phases: idle,
    // checking, sending, waiting, rebooting, done, failed.
    var up = { phase: "idle", msg: "", sent: 0, total: 0, timer: null };
    var s = {};   // per-mount state: status, info, file, confirm
    var r = {};   // dom references, empty while unmounted

    function kb(n) {
        return Math.round((n || 0) / 1024) + " kB";
    }

    function busy() {
        return up.phase === "checking" || up.phase === "sending" ||
            up.phase === "waiting" || up.phase === "rebooting";
    }

    // md5 begin
    // MD5 of a Uint8Array as 32 lower-case hex digits. Browsers offer no
    // MD5 in Web Crypto and this page loads nothing from outside the
    // device, so RFC 1321 is written out: the message is padded to a
    // multiple of 64 bytes with its bit length at the end, and each
    // block goes through the four rounds of sixteen steps.
    function md5Hex(bytes) {
        var K = [], i, j, t;
        var S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
        for (i = 0; i < 64; i++) {
            K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) | 0;
        }
        var n = bytes.length;
        var padded = new Uint8Array(((n + 8) >> 6 << 6) + 64);
        padded.set(bytes);
        padded[n] = 0x80;
        var dv = new DataView(padded.buffer);
        dv.setUint32(padded.length - 8, (n * 8) >>> 0, true);
        dv.setUint32(padded.length - 4, Math.floor(n / 536870912), true);
        var a0 = 0x67452301, b0 = 0xefcdab89 | 0, c0 = 0x98badcfe | 0, d0 = 0x10325476;
        var M = new Int32Array(16);
        for (var off = 0; off < padded.length; off += 64) {
            for (j = 0; j < 16; j++) M[j] = dv.getInt32(off + j * 4, true);
            var a = a0, b = b0, c = c0, d = d0;
            for (t = 0; t < 64; t++) {
                var f, g, round = t >> 4;
                if (round === 0) { f = (b & c) | (~b & d); g = t; }
                else if (round === 1) { f = (d & b) | (~d & c); g = (5 * t + 1) & 15; }
                else if (round === 2) { f = b ^ c ^ d; g = (3 * t + 5) & 15; }
                else { f = c ^ (b | ~d); g = (7 * t) & 15; }
                var x = (a + f + K[t] + M[g]) | 0;
                var sh = S[round * 4 + (t & 3)];
                var rot = (x << sh) | (x >>> (32 - sh));
                a = d; d = c; c = b; b = (b + rot) | 0;
            }
            a0 = (a0 + a) | 0; b0 = (b0 + b) | 0; c0 = (c0 + c) | 0; d0 = (d0 + d) | 0;
        }
        return hex32(a0) + hex32(b0) + hex32(c0) + hex32(d0);
    }

    // The four bytes of a word, least significant first, as hex.
    function hex32(v) {
        var out = "";
        for (var i = 0; i < 4; i++) {
            var byte = (v >>> (i * 8)) & 0xff;
            out += (byte < 16 ? "0" : "") + byte.toString(16);
        }
        return out;
    }
    // md5 end

    // The bytes as a Latin-1 string, one char per byte, so indexOf and a
    // regular expression can search them. Built in pieces: apply() has
    // an argument limit far below a quarter megabyte.
    function latin1(u8) {
        var parts = [];
        for (var i = 0; i < u8.length; i += 8192) {
            parts.push(String.fromCharCode.apply(null, u8.subarray(i, i + 8192)));
        }
        return parts.join("");
    }

    // What tools/ota.sh and checkimage.sh check before sending, in the
    // device's order. Returns { err } to refuse, else { stamp, md5, size }.
    function inspectImage(buf, info) {
        var u8 = new Uint8Array(buf), n = u8.length;
        if (!info) {
            return { err: "the board has not reported its filesystem yet" };
        }
        if (n < info.minSize || n > info.maxSize) {
            return { err: "this file is " + kb(n) + "; an image is " +
                kb(info.minSize) + " to " + kb(info.maxSize) };
        }
        var text = latin1(u8);
        if (text.indexOf(BANNER) < 0) {
            return { err: "not a miniWorld lighting controller image" };
        }
        var stamps = text.match(STAMP_RE) || [];
        if (stamps.length !== 1) {
            return { err: "cannot find one build stamp in this file" };
        }
        return { stamp: stamps[0], md5: md5Hex(u8), size: n };
    }

    // Phase and status line -------------------------------------------

    function setPhase(phase, msg) {
        up.phase = phase;
        up.msg = msg;
        if (phase !== "sending") {
            up.sent = 0;
            up.total = 0;
        }
        renderUpload();
    }

    function renderUpload() {
        if (!r.send) return;
        r.send.disabled = busy() || !s.file;
        r.file.disabled = busy();
        r.bar.hidden = up.phase !== "sending";
        r.fill.style.width = up.total ? Math.round(100 * up.sent / up.total) + "%" : "0%";
        r.msg.textContent = up.msg;
        r.msg.className = "status " + up.phase;
        r.msg.hidden = !up.msg;
        renderReboot();
    }

    // Polls the status until test(status) is true or maxMs have passed.
    // A request that fails (the board is rebooting) counts as "not yet",
    // never as the end: the board is expected to be away for a while.
    function waitFor(test, maxMs, onOk, onTimeout) {
        var end = Date.now() + maxMs;
        clearTimeout(up.timer);
        function step() {
            api("GET", "/api/system/status").then(function (st) {
                if (st && test(st)) {
                    s.status = st;
                    renderDevice();
                    onOk();
                    return;
                }
                again();
            }, again);
        }
        function again() {
            if (Date.now() >= end) {
                onTimeout();
                return;
            }
            up.timer = setTimeout(step, WAIT_STEP_MS);
        }
        up.timer = setTimeout(step, WAIT_FIRST_MS);
    }

    // The upload ----------------------------------------------------------

    function chooseFile(ev) {
        s.file = (ev.target.files && ev.target.files[0]) || null;
        if (!busy()) {
            setPhase("idle", s.file ? s.file.name + ", " + kb(s.file.size) : "");
        }
    }

    function send() {
        var file = s.file;
        if (!file || busy()) return;
        setPhase("checking", "checking " + file.name);
        var reader = new FileReader();
        reader.onerror = function () {
            setPhase("failed", "could not read " + file.name);
        };
        reader.onload = function () {
            var buf = reader.result;
            var image = inspectImage(buf, s.info);
            if (image.err) {
                setPhase("failed", image.err);
                return;
            }
            setPhase("sending", "sending " + kb(image.size));
            post(buf, image);
        };
        reader.readAsArrayBuffer(file);
    }

    // A plain XMLHttpRequest, not App.api: the body is binary, the two
    // firmware headers go with it, and the upload progress event drives
    // the bar. The browser sets Content-Length and reuses the basic-auth
    // credentials it already holds for this origin.
    function post(buf, image) {
        var xhr = new XMLHttpRequest();
        xhr.open("POST", "/api/system/firmware");
        xhr.timeout = SEND_TIMEOUT_MS;
        xhr.setRequestHeader("Content-Type", "application/octet-stream");
        xhr.setRequestHeader("X-Firmware-MD5", image.md5);
        xhr.setRequestHeader("X-Firmware-Build", image.stamp);
        xhr.upload.onprogress = function (ev) {
            if (!ev.lengthComputable) return;
            up.sent = ev.loaded;
            up.total = ev.total;
            up.msg = "sending " + kb(ev.loaded) + " of " + kb(ev.total);
            renderUpload();
        };
        xhr.onload = function () { answered(xhr, image); };
        xhr.onerror = xhr.ontimeout = function () {
            // No reply at all. The board may have taken the image and be
            // rebooting, so look for the stamp before calling it a failure.
            setPhase("waiting", "no reply from the board; it may be rebooting");
            waitForBuild(image.stamp);
        };
        xhr.send(buf);
    }

    function answered(xhr, image) {
        var body = {};
        try { body = JSON.parse(xhr.responseText || "{}"); } catch (e) { body = {}; }
        var err = body.error || ("status " + xhr.status);
        if (xhr.status === 200) {
            setPhase("waiting", "staged, the board is rebooting");
            waitForBuild(image.stamp);
            return;
        }
        if (xhr.status === 401) {
            location.reload();          // the browser's own login prompt, as App.api does
            return;
        }
        if (xhr.status === 413) {
            setPhase("failed", "the board takes images up to " + body.max +
                " bytes; this one is " + image.size);
        } else if (xhr.status === 422) {
            setPhase("failed", "the board rejected the image: " + err);
        } else if (xhr.status === 400) {
            setPhase("failed", "the board rejected the headers: " + err);
        } else {
            setPhase("failed", "the board could not stage the image: " + err);
        }
        loadInfo();                     // a refusal deleted the partial file
    }

    function waitForBuild(stamp) {
        waitFor(function (st) { return st.build === stamp; }, WAIT_MAX_MS,
            function () {
                setPhase("done", "board is back on build " + stamp);
                loadInfo();
            },
            function () {
                setPhase("failed", "the board did not come back on the new build; " +
                    "if it stays unreachable, flash over USB with make upload");
            });
    }

    // Reboot ----------------------------------------------------------

    function renderReboot() {
        if (!r.rbody) return;
        r.rbody.innerHTML = "";
        if (!s.confirm) {
            r.rbody.appendChild(el("button", { class: "btn secondary wide", type: "button",
                disabled: busy(),
                onclick: function () { s.confirm = true; renderReboot(); } }, "Reboot"));
            return;
        }
        r.rbody.appendChild(el("p", { class: "ask" }, "Reboot the controller?"));
        r.rbody.appendChild(el("div", { class: "btn-row" },
            el("button", { class: "btn danger", type: "button", onclick: reboot }, "Yes, reboot"),
            el("button", { class: "btn secondary", type: "button",
                onclick: function () { s.confirm = false; renderReboot(); } }, "Not now")));
    }

    function reboot() {
        var before = s.status && typeof s.status.uptime === "number" ? s.status.uptime : Infinity;
        var t0 = Date.now();
        s.confirm = false;
        setPhase("rebooting", "rebooting");
        api("POST", "/api/system/reboot").then(function () {
            waitFor(function (st) { return typeof st.uptime === "number" && st.uptime < before; },
                REBOOT_MAX_MS,
                function () {
                    setPhase("idle", "back after " + Math.round((Date.now() - t0) / 1000) + " s");
                    loadInfo();
                },
                function () {
                    setPhase("failed", "the board did not answer within 60 s; check it over USB");
                });
        }).catch(function (e) {
            setPhase("failed", (e && e.message) || "reboot request failed");
        });
    }

    // Cards -----------------------------------------------------------

    function setRows(box, rows) {
        box.innerHTML = "";
        for (var i = 0; i < rows.length; i++) {
            box.appendChild(el("div", { class: "row" },
                el("span", { class: "k" }, rows[i][0]),
                el("span", { class: "v tnum" }, rows[i][1])));
        }
    }

    function renderDevice() {
        var st = s.status;
        if (!r.dev || !st) return;
        setRows(r.dev, [
            ["Firmware", st.firmware || "?"],
            ["Build", st.build || "?"],
            ["Up", App.fmtUptime(st.uptime)],
            ["Free heap", kb(st.heap)],
            ["Clock", st.timeValid && st.time ? String(st.time).replace("T", " ") : "not set"]
        ]);
    }

    function renderInfo() {
        var info = s.info;
        if (!r.room || !info) return;
        var text = kb(info.fsFree) + " free, images up to " + kb(info.maxSize);
        if (info.staged) text += "; an image is staged for the next boot";
        r.room.textContent = text;
    }

    function loadInfo() {
        api("GET", "/api/system/firmware").then(function (info) {
            s.info = info;
            renderInfo();
        }).catch(function () { /* the headroom line keeps its last text */ });
    }

    function build() {
        r.dev = el("div", { class: "rows" });
        r.room = el("p", { class: "room" }, "reading the filesystem");
        r.file = el("input", { id: "sys-file", type: "file", accept: ".bin",
            onchange: chooseFile });
        r.send = el("button", { class: "btn primary", type: "button", onclick: send,
            disabled: true }, "Send");
        r.fill = el("div", { class: "fill" });
        r.bar = el("div", { class: "bar", role: "progressbar", "aria-label": "upload",
            hidden: true }, r.fill);
        r.msg = el("p", { class: "status idle", role: "status", "aria-live": "polite",
            hidden: true });
        r.rbody = el("div", {});

        return el("section", { class: "view-system" },
            el("div", { class: "card" },
                el("div", { class: "head" }, el("h2", {}, "Device")), r.dev),
            el("div", { class: "card" },
                el("div", { class: "head" }, el("h2", {}, "Firmware")),
                r.room,
                el("div", { class: "file-row" },
                    el("label", { for: "sys-file", class: "sr" }, "Image file"),
                    r.file, r.send),
                r.bar, r.msg),
            el("div", { class: "card" },
                el("div", { class: "head" }, el("h2", {}, "Reboot")), r.rbody));
    }

    function mount(root) {
        s = { status: null, info: null, file: null, confirm: false };
        r = {};
        root.appendChild(build());
        renderUpload();
        api("GET", "/api/system/status").then(function (st) {
            s.status = st;
            renderDevice();
        }).catch(function () { /* the header badge already says it */ });
        loadInfo();
    }

    // The router polls every 2 s. While an upload or a reboot is in
    // flight the wait loop does its own polling and the device is busy
    // with the body, so the routine poll stands down.
    function poll() {
        if (busy()) return;
        api("GET", "/api/system/status").then(function (st) {
            s.status = st;
            renderDevice();
        }).catch(function () { /* keep the last known state on screen */ });
    }

    function unmount() {
        s = {};
        r = {};
    }

    App.register("system", { title: "System", mount: mount, unmount: unmount, poll: poll });
})();
```

- [ ] **Step 4: Write `web/view-system.css`**

```css
/* view-system.css - the System view: three cards, key and value rows,
   the file row, the progress bar and the one status line.

   Invector Embedded Systems AB */

.view-system [hidden] { display: none !important; }
.view-system h2 { margin: 0; font-size: 1.05rem; font-weight: 600; }
.view-system .head {
    display: flex; align-items: center; justify-content: space-between;
    gap: 8px; margin-bottom: 10px;
}
.view-system .row .k { color: var(--muted); }
.view-system .room { color: var(--muted); margin: 0 0 10px; }

/* The file row: the picker takes the width, Send keeps its size. */
.view-system .file-row { display: flex; align-items: center; gap: 8px; }
.view-system .file-row input[type="file"] { flex: 1 1 auto; min-width: 0; color: var(--muted); }
.view-system .sr {
    position: absolute; width: 1px; height: 1px; overflow: hidden;
    clip: rect(0 0 0 0); white-space: nowrap;
}

/* The bar is only in the page while bytes move. */
.view-system .bar {
    height: 6px; margin-top: 12px; border-radius: 3px;
    background: var(--line); overflow: hidden;
}
.view-system .fill { height: 100%; width: 0; background: var(--accent); transition: width 0.2s; }

/* One status line; its class is the phase. */
.view-system .status { margin: 10px 0 0; color: var(--muted); }
.view-system .status.sending, .view-system .status.waiting,
.view-system .status.checking, .view-system .status.rebooting { color: var(--accent); }
.view-system .status.done { color: var(--ok); }
.view-system .status.failed { color: #f08a8a; }

.view-system .ask { margin: 0 0 10px; }
.view-system .btn-row { display: flex; gap: 8px; }
.view-system .btn.wide { width: 100%; }
.view-system .btn.danger { background: #a13b3b; border-color: #a13b3b; color: #fff; }
.view-system .btn:disabled { opacity: 0.5; cursor: default; }
```

- [ ] **Step 5: Verify the pieces that can run without a browser**

Run, from the repo root:

```bash
node --check web/view-system.js && node --check web/app.js && node --check web/view-home.js && echo "syntax ok"
sed -n '/\/\/ md5 begin/,/\/\/ md5 end/p' web/view-system.js > /tmp/md5.js
node -e '
eval(require("fs").readFileSync("/tmp/md5.js","utf8"));
var enc = function (s) { return new Uint8Array(Buffer.from(s, "latin1")); };
var crypto = require("crypto");
var ref = function (s) { return crypto.createHash("md5").update(Buffer.from(s, "latin1")).digest("hex"); };
var checks = [["", "d41d8cd98f00b204e9800998ecf8427e"], ["abc", "900150983cd24fb0d6963f7d28e17f72"]];
["The quick brown fox jumps over the lazy dog", "a".repeat(55), "a".repeat(56), "a".repeat(63), "a".repeat(64), "a".repeat(65)]
  .forEach(function (s) { checks.push([s, ref(s)]); });
var bad = checks.filter(function (c) { return md5Hex(enc(c[0])) !== c[1]; });
var big = crypto.randomBytes(300001);
var bigRef = crypto.createHash("md5").update(big).digest("hex");
if (md5Hex(new Uint8Array(big)) !== bigRef) bad.push(["300001 random bytes", bigRef]);
var re = /[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}/g;
if ((("x Sep  6 2026 07:08:09 y").match(re) || []).length !== 1) bad.push(["two-space day stamp", "1 match"]);
if ((("Sep 23 2026 17:09:03 and Sep 24 2026 08:00:00").match(re) || []).length !== 2) bad.push(["two stamps", "2 matches"]);
console.log(bad.length ? "MD5/STAMP FAIL: " + JSON.stringify(bad) : "md5 and stamp ok");
'
make web 2>&1 | grep -i "webui:"
grep -c $'\xe2\x80\x94' web/view-system.js web/view-system.css web/app.js web/view-home.js web/index.html
```

Expected: `syntax ok`; `md5 and stamp ok`; `webui: ... bytes gzipped` at most 40960 and about 4 kB above 29927; every em dash count 0.

- [ ] **Step 6: The whole build**

Run: `make 2>&1 | grep -E "warning|error|Error|Sketch uses|Global variables"; echo "make exit=${PIPESTATUS[0]}"`
Expected: no warning or error lines, `make exit=0`. The flash figure grows by the gzipped delta.

- [ ] **Step 7: Commit**

```bash
git add web/view-system.js web/view-system.css web/app.js web/view-home.js web/index.html
git commit -m "GUI: a System tab with the device, the headroom, the firmware upload and a reboot button

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01VV9r86jJQiHpXtoW9xA7sE"
```

---

### Task 3: The page over the mock, in a browser

**Files:** none changed unless a step fails; this task produces the evidence that the page behaves as the spec says.

**Interfaces:**
- Consumes: Task 1's mock and Task 2's page.

This task is run by the controller with a browser it can drive (the Chrome tools), not by a headless worker. If the browser tool cannot set a file input, the file-picker steps are handed to the person at the bench, who reports what the status line said.

- [ ] **Step 1: Start the mock with a fresh image**

Run: `make >/dev/null 2>&1; python3 tools/mockserver.py --port 8080 > /tmp/mock.log 2>&1 & echo $! > /tmp/mock.pid; sleep 1; head -1 build/.stamp 2>/dev/null || tools/checkimage.sh build | tail -1`
Note the stamp of `build/miniWorld_LightingController.ino.bin`.

- [ ] **Step 2: Prepare four doctored files**

```bash
B=build/miniWorld_LightingController.ino.bin
head -c 12000 $B > /tmp/small.bin
python3 - "$B" <<'PY'
import sys, re
b = open(sys.argv[1], "rb").read()
open("/tmp/nobanner.bin", "wb").write(b.replace(b"miniWorld lighting controller %s (%s)", b"someOther lighting controller %s (%s)"))
m = re.search(rb"[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}", b)
open("/tmp/nostamp.bin", "wb").write(b[:m.start()] + b"x" * (m.end() - m.start()) + b[m.end():])
open("/tmp/twostamps.bin", "wb").write(b + b"\0Jan 01 2000 00:00:00\0")
PY
ls -la /tmp/small.bin /tmp/nobanner.bin /tmp/nostamp.bin /tmp/twostamps.bin
```

`/tmp/nobanner.bin` still contains the stamp: it is Review Focus 1, another sketch that carries a stamp-shaped string.

- [ ] **Step 3: The tab and its cards**

Open `http://localhost:8080/#system` at a desktop width and at 390 px. Expected: the nav shows `System` as the seventh entry and marks it active; the Device card lists Firmware `0.1.0`, Build `mock`, Up, Free heap `176 kB`, Clock; the Firmware card's headroom line reads `988 kB free, images up to 924 kB`; `Send` is disabled; the Reboot card shows one `Reboot` button. Nothing overflows at 390 px.

- [ ] **Step 4: The four refusals, nothing sent**

Pick each doctored file and press `Send`. Expected status line, in the failed colour, and `/tmp/mock.log` gains no `POST /api/system/firmware` line for any of them:
- `/tmp/small.bin`: `this file is 12 kB; an image is 98 kB to 924 kB`
- `/tmp/nobanner.bin`: `not a miniWorld lighting controller image`
- `/tmp/nostamp.bin`: `cannot find one build stamp in this file`
- `/tmp/twostamps.bin`: `cannot find one build stamp in this file`
After each, `Send` is enabled again as soon as a file is picked.

- [ ] **Step 5: A real image, the wait, and the tab switch**

Pick `build/miniWorld_LightingController.ino.bin`, press `Send`. Expected in order: `checking ...`, the bar with `sending N kB of 253 kB` (over localhost it is quick), `staged, the board is rebooting`, and after about three seconds `board is back on build <stamp>` in the done colour, the Device card's Build row showing the stamp, the headroom line re-read. While `staged, the board is rebooting` shows, click `Houses` in the nav and then `System` again: the status line still says it and `Send` is disabled (Review Focus 3). Press `Send` during the wait: nothing happens (Review Focus 5).

- [ ] **Step 6: Reboot**

Press `Reboot`, then `Yes, reboot`. Expected: `rebooting`, then `back after 3 s` and the Up row small again. `/tmp/mock.log` says `mock: reboot requested, uptime reset`.

- [ ] **Step 7: Password**

`kill $(cat /tmp/mock.pid); python3 tools/mockserver.py --port 8080 --password secret > /tmp/mock.log 2>&1 & echo $! > /tmp/mock.pid`. Reload the page: the browser asks for the password; with it, the System tab works as in step 5 (the XHR reuses the credentials). Then `kill $(cat /tmp/mock.pid)`.

- [ ] **Step 8: Record**

Write the observed status line texts and the two widths into the ledger. Anything that differs from the expected text is a finding for Task 2's fix round, not a reason to change the expected text.

---

### Task 4: The bench and HANDOFF

**Files:**
- Modify: `HANDOFF.md` (the `web/` inventory row near line 91, a new `web/view-system.js/.css` row after the `view-home` row near line 95, a new section 5.10 after 5.9 near line 340, the GUI sentence in 5.9's Spec paragraph)
- Copy: `miniWorld_LightingController/HANDOFF.md` (byte-identical)

**Interfaces:**
- Consumes: the merged page on the board.

The bench steps are the controller's (the board is not a host a worker may touch); the HANDOFF edit can go to a worker with the measured facts appended to its brief.

- [ ] **Step 1: The page onto the board over USB, once**

Run: `make upload`
Expected: `flash: verified through the API at 192.168.1.180`.

- [ ] **Step 2: A real update from the System tab**

Run `make` once more so `./build` carries a new stamp, then in a browser open `http://192.168.1.180/#system`, pick `build/miniWorld_LightingController.ino.bin`, press `Send`. Expected: the bar moves for 8 to 9 s, `staged, the board is rebooting`, then within about 15 s `board is back on build <new stamp>`, and the Device card shows it. Note the wall time from `Send` to the done line.

- [ ] **Step 3: Reboot from the tab**

Press `Reboot`, `Yes, reboot`. Expected: `back after <n> s` with n about 6 to 10.

- [ ] **Step 4: HANDOFF**

Change the `web/` row's purpose to `SPA framework: \`index.html\`, \`app.css\`, \`app.js\`, seven views`. After the `web/view-home.js/.css` row add:

```
| `web/view-system.js/.css` | System view: the device line, filesystem headroom, the firmware upload with the browser-side band, banner, stamp and MD5 checks, the wait for the new build, a reboot button | Done, verified on the board 2026-09-23, see §5.10 |
```

In 5.9's first paragraph change `GUI upload page: a later round.` to `GUI upload page: §5.10.`

After section 5.9 (before `## 6.`) add:

```
### 5.10 Firmware upload from the GUI (done 2026-09-23: <the update from the System tab, the wall time from Send to the done line, the reboot button's time>)

Spec: `docs/superpowers/specs/2026-09-23-firmware-gui-design.md`. A
System tab does in the browser what `tools/ota.sh` does on the host: it
checks the band, the banner string, exactly one stamp-shaped string and
the MD5 (RFC 1321 written out in `view-system.js`, since browsers offer
no MD5) before a byte is sent, streams the file with an XMLHttpRequest
so the bar moves, then polls the status for the new stamp. A lost reply
goes to the same wait rather than to a failure. The mock lags an upload
by 3 s and resets its uptime on reboot so both waits run off-target.
```

Fill the angle brackets with the measured facts; no placeholder remains.

- [ ] **Step 5: Copy and check**

```bash
cp HANDOFF.md miniWorld_LightingController/HANDOFF.md
diff -q HANDOFF.md miniWorld_LightingController/HANDOFF.md && grep -c $'\xe2\x80\x94' HANDOFF.md
```

Expected: no diff output, and `0`.

- [ ] **Step 6: Commit**

```bash
git add HANDOFF.md miniWorld_LightingController/HANDOFF.md
git commit -m "HANDOFF 5.10: firmware upload from the GUI verified on the board

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01VV9r86jJQiHpXtoW9xA7sE"
```
