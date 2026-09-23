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
