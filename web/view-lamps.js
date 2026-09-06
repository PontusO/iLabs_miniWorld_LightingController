// view-lamps.js - Lamps view: what is fitted on each of the twelve I2C
// buses, what the controller found, and a test card.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el;
    var NB = 12;
    // Fixed by the board: the SDA/SCL pair each bus runs on.
    var GPIO = ["0/1", "26/27", "2/3", "6/7", "8/9", "14/15",
        "22/23", "24/25", "16/17", "10/18", "20/21", "28/29"];

    var form = null;   // the edited configuration
    var saved = "";    // JSON of it as last loaded or saved
    var probed = null; // last probe result, as a configuration
    var rows = [];     // per bus: { row, sw, val, minus, plus, stat }
    var ui = {};       // the single controls and the panels they write to
    var mounted = false; // false between unmount and the next mount
    var testAt = 0, testTimer = null, testWant = 0, testErrAt = 0;

    function blank() {
        var b = [];
        for (var i = 0; i < NB; i++) b.push({ sx1503: false, al5887: 0 });
        return { busSpeed: 400000, activeLow: false, rgb: false, buses: b };
    }

    function fromJson(c) {
        var f = blank(), list = (c && c.buses) || [], i, b;
        if (!c) return f;
        // A speed this page does not offer is kept as it is, not folded to
        // 400 kHz: Save would otherwise halve a 1 MHz bus without saying so.
        f.busSpeed = (typeof c.busSpeed === "number" && c.busSpeed > 0) ? c.busSpeed : 400000;
        f.activeLow = !!c.activeLow;
        f.rgb = !!c.rgb;
        for (i = 0; i < NB && i < list.length; i++) {
            b = list[i] || {};
            f.buses[i].sx1503 = !!b.sx1503;
            f.buses[i].al5887 = Math.max(0, Math.min(4, b.al5887 | 0));
        }
        return f;
    }

    function tally(lamps, devices) {
        return lamps + (lamps === 1 ? " lamp" : " lamps") + " on " +
            devices + (devices === 1 ? " device" : " devices");
    }

    function fit(buses) {
        var lamps = 0, dev = 0;
        for (var i = 0; i < buses.length; i++) {
            lamps += (buses[i].sx1503 ? 16 : 0) + 36 * buses[i].al5887;
            dev += (buses[i].sx1503 ? 1 : 0) + buses[i].al5887;
        }
        return tally(lamps, dev);
    }

    function num(v) { var n = parseInt(v, 10); return isNaN(n) ? 0 : n; }

    // 400000 -> "400 kHz", 1000000 -> "1 MHz".
    function speedLabel(hz) {
        if (hz >= 1000000) return (Math.round(hz / 100000) / 10) + " MHz";
        return Math.round(hz / 1000) + " kHz";
    }

    // The select offers the two speeds the product ships with. When the
    // controller reports another one, it is added as a disabled option
    // labelled with its value, so it stays selected and Save round-trips it.
    function syncSpeed() {
        var extra = (form.busSpeed === 100000 || form.busSpeed === 400000) ? 0 : form.busSpeed;
        if (ui.speedExtra && ui.speedExtra.value !== String(extra)) {
            ui.speed.removeChild(ui.speedExtra);
            ui.speedExtra = null;
        }
        if (extra && !ui.speedExtra) {
            ui.speedExtra = el("option", { value: String(extra), disabled: true },
                speedLabel(extra) + " (not offered)");
            ui.speed.appendChild(ui.speedExtra);
        }
        ui.speed.value = String(form.busSpeed);
    }

    function sw(input, cls) {
        return el("label", { class: "switch " + cls }, input,
            el("span", { class: "track" }), el("span", { class: "thumb" }));
    }

    // ---- the bus plate --------------------------------------------------

    function busCard() {
        ui.summary = el("p", { class: "sum tnum" }, "Reading the controller");
        var plate = el("div", { class: "plate" }, el("div", { class: "bus-row head" },
            el("span", { class: "c-id" }, "Bus"), el("span", { class: "c-sw" }, "SX1503"),
            el("span", { class: "c-step" }, "AL5887"), el("span", { class: "c-stat" }, "Found")));
        for (var i = 0; i < NB; i++) plate.appendChild(busRow(i));
        return el("section", { class: "card" }, el("h2", null, "Buses"), ui.summary, plate);
    }

    function busRow(i) {
        var r = rows[i] = {};
        r.sw = el("input", { type: "checkbox", "aria-label": "SX1503 on bus " + i,
            onchange: function () { form.buses[i].sx1503 = r.sw.checked; renderForm(); } });
        r.val = el("span", { class: "val tnum" }, "0");
        r.minus = step("−", "One AL5887 fewer on bus " + i, i, -1);
        r.plus = step("+", "One AL5887 more on bus " + i, i, 1);
        r.stat = el("div", { class: "c-stat stat" });
        r.row = el("div", { class: "bus-row" },
            el("div", { class: "c-id id" }, el("span", { class: "n tnum" }, "Bus " + i),
                el("span", { class: "gp tnum" }, "GPIO " + GPIO[i])),
            sw(r.sw, "c-sw"),
            el("div", { class: "stepper c-step" }, r.minus, r.val, r.plus), r.stat);
        return r.row;
    }

    function step(glyph, label, bus, delta) {
        return el("button", { type: "button", "aria-label": label, onclick: function () {
            var v = form.buses[bus].al5887 + delta;
            form.buses[bus].al5887 = Math.max(0, Math.min(4, v));
            renderForm();
        } }, glyph);
    }

    // Paints every control from the form, so the changed marker, the diff
    // and the row highlights always agree with what is in the form.
    function renderForm() {
        var was = JSON.parse(saved), n = 0, i, b, r;
        for (i = 0; i < NB; i++) {
            b = form.buses[i];
            r = rows[i];
            if (r.sw.checked !== b.sx1503) r.sw.checked = b.sx1503;
            r.val.textContent = String(b.al5887);
            r.minus.disabled = b.al5887 === 0;
            r.plus.disabled = b.al5887 === 4;
            r.row.classList[b.sx1503 || b.al5887 ? "add" : "remove"]("fitted");
            if (b.sx1503 !== was.buses[i].sx1503 || b.al5887 !== was.buses[i].al5887) n++;
        }
        syncSpeed();
        ui.activeLow.checked = form.activeLow;
        ui.rgb.checked = form.rgb;
        if (form.busSpeed !== was.busSpeed) n++;
        if (form.activeLow !== was.activeLow) n++;
        if (form.rgb !== was.rgb) n++;
        ui.mark.hidden = n === 0;
        ui.mark.textContent = "changed (" + n + ")";
        ui.after.textContent = n ? "Save fits " + fit(form.buses) + "." : "";
        renderDiff();
    }

    function showStatus(s) {
        if (!s || !mounted) return;
        ui.summary.textContent = tally(s.lamps | 0, s.devices | 0) + ", reported by the controller";
        var list = s.buses || [], i, j, b, al, stat;
        for (i = 0; i < NB; i++) {
            b = list[i] || {};
            al = b.al5887 || [];
            stat = rows[i].stat;
            stat.innerHTML = "";
            if (b.sx1503 && b.sx1503 !== "none") stat.appendChild(pip("SX", b.sx1503));
            for (j = 0; j < al.length; j++) stat.appendChild(pip("A" + (j + 1), al[j]));
        }
    }

    // A device mark: a disc, the device name and the state as a word, so it
    // reads without colour. A fault gets a diamond instead of a disc.
    function pip(name, state) {
        var ok = state === "ok";
        return el("span", { class: "pip " + (ok ? "ok" : "fault") }, el("span", { class: "dot" }),
            name, " ", el("span", { class: "w" }, ok ? "ok" : "fault"));
    }

    // ---- global settings and save ----------------------------------------

    function globalCard() {
        ui.speed = el("select", { id: "lamps-speed", onchange: function () {
            form.busSpeed = num(ui.speed.value);
            renderForm();
        } }, el("option", { value: "100000" }, "100 kHz"), el("option", { value: "400000" }, "400 kHz"));
        ui.activeLow = toggle("Active low outputs", "activeLow");
        ui.rgb = toggle("RGB lamps", "rgb");
        ui.mark = el("span", { class: "mark", hidden: true });
        ui.after = el("p", { class: "after tnum" });
        ui.save = el("button", { class: "btn primary", type: "button", onclick: save }, "Save");
        return el("section", { class: "card" }, el("h2", null, "All buses"),
            el("div", { class: "field" }, el("label", { for: "lamps-speed" }, "Bus speed"), ui.speed),
            el("div", { class: "row" }, el("span", null, "Active low outputs"), sw(ui.activeLow, "")),
            el("div", { class: "row" }, el("span", null, "RGB lamps"), sw(ui.rgb, "")),
            el("div", { class: "save" }, ui.save, ui.mark),
            ui.after);
    }

    function toggle(label, key) {
        return el("input", { type: "checkbox", "aria-label": label, onchange: function () {
            form[key] = this.checked;
            renderForm();
        } });
    }

    // Disables the buttons that must not fire again while a request is in
    // flight, and returns the function that puts them back.
    function hold(list) {
        for (var i = 0; i < list.length; i++) list[i].disabled = true;
        return function () {
            for (var j = 0; j < list.length; j++) list[j].disabled = false;
        };
    }

    function load() {
        return App.api("GET", "/api/lamps/config").then(function (c) {
            if (!mounted) return;
            form = fromJson(c);
            saved = JSON.stringify(form);
            renderForm();
        }, function (e) {
            if (mounted) App.toast(e.message || "Could not read the configuration", "error");
        });
    }

    // Save and Probe both talk to the hardware, so each holds the other for
    // the length of its request.
    function save() {
        var release = hold([ui.save, ui.probe]);
        App.api("PUT", "/api/lamps/config", form).then(load).then(function () {
            if (!mounted) return;
            App.toast("Saved", "ok");
            view.poll();
        }, function (e) {
            if (mounted) App.toast(e.message || "Save failed", "error");
        }).then(release, release);
    }

    // ---- probe -------------------------------------------------------------

    function probeCard() {
        ui.diff = el("div", { class: "diff" });
        ui.use = el("button", { class: "btn secondary", type: "button", hidden: true,
            onclick: useProbe }, "Use what was found");
        ui.probe = el("button", { class: "btn secondary", type: "button", onclick: probe },
            "Probe hardware");
        return el("section", { class: "card" }, el("h2", null, "Probe"),
            el("p", { class: "hint" }, "Ask the controller which devices answer on each bus."),
            ui.probe, ui.diff, ui.use);
    }

    function probe() {
        var release = hold([ui.probe, ui.save]);
        App.toast("Probing, lamps will blink briefly", "info");
        App.api("POST", "/api/lamps/probe").then(function (c) {
            if (!mounted) return;
            probed = fromJson(c);
            renderDiff();
        }, function (e) {
            if (mounted) App.toast(e.message || "Probe failed", "error");
        }).then(release, release);
    }

    function describe(b) {
        var p = [];
        if (b.sx1503) p.push("SX1503");
        if (b.al5887) p.push(b.al5887 + " × AL5887");
        return p.length ? p.join(" + ") : "nothing";
    }

    function renderDiff() {
        if (!ui.diff) return;
        ui.diff.innerHTML = "";
        ui.use.hidden = true;
        if (!probed) return;
        var any = false, i, f, c, tags;
        for (i = 0; i < NB; i++) {
            f = probed.buses[i];
            c = form.buses[i];
            if (f.sx1503 === c.sx1503 && f.al5887 === c.al5887) continue;
            any = true;
            tags = el("span", { class: "tags" });
            if ((f.sx1503 && !c.sx1503) || f.al5887 > c.al5887) {
                tags.appendChild(el("span", { class: "tag found" }, "found but not configured"));
            }
            if ((c.sx1503 && !f.sx1503) || c.al5887 > f.al5887) {
                tags.appendChild(el("span", { class: "tag gone" }, "configured but not found"));
            }
            ui.diff.appendChild(el("p", { class: "line" }, el("span", { class: "txt tnum" },
                "Bus " + i + ": found " + describe(f) + ", configured " + describe(c)), tags));
        }
        if (!any) ui.diff.appendChild(el("p", { class: "line match" }, "Matches the configuration"));
        ui.use.hidden = !any;
    }

    function useProbe() {
        if (!probed) return;
        for (var i = 0; i < NB; i++) {
            form.buses[i].sx1503 = probed.buses[i].sx1503;
            form.buses[i].al5887 = probed.buses[i].al5887;
        }
        renderForm();
        App.toast("Copied into the form, Save to apply it", "ok");
    }

    // ---- test ---------------------------------------------------------------

    function testCard() {
        ui.level = el("input", { type: "range", min: "0", max: "255", value: "0", step: "1",
            class: "level", id: "lamps-level", oninput: function () {
                ui.levelOut.textContent = ui.level.value;
                sendLevel(num(ui.level.value));
            } });
        ui.levelOut = el("span", { class: "out tnum" }, "0");
        ui.lamp = el("input", { type: "number", min: "0", value: "0", inputmode: "numeric",
            id: "lamps-lamp" });
        ui.set = el("button", { class: "btn secondary", type: "button", onclick: setOne }, "Set");
        return el("section", { class: "card" }, el("h2", null, "Test"),
            el("div", { class: "lvl" }, el("label", { for: "lamps-level" }, "All lamps"), ui.levelOut),
            ui.level,
            el("div", { class: "one" }, el("label", { for: "lamps-lamp" }, "Lamp"), ui.lamp, ui.set),
            el("p", { class: "hint" }, "Set drives that one lamp at the level above."));
    }

    // One request per 150 ms while the slider moves: send at once when the
    // last one is old enough, otherwise keep the newest value for a timer.
    function sendLevel(level) {
        testWant = level;
        var wait = 150 - (Date.now() - testAt);
        if (wait <= 0) {
            testAt = Date.now();
            App.api("POST", "/api/lamps/test", { level: testWant }).then(null, testFail);
        } else if (!testTimer) {
            testTimer = setTimeout(function () {
                testTimer = null;
                testAt = Date.now();
                App.api("POST", "/api/lamps/test", { level: testWant }).then(null, testFail);
            }, wait);
        }
    }

    function testFail(e) {
        if (!mounted || Date.now() - testErrAt < 3000) return;
        testErrAt = Date.now();
        App.toast(e.message || "Test failed", "error");
    }

    function setOne() {
        var lamp = parseInt(ui.lamp.value, 10), level = num(ui.level.value);
        if (isNaN(lamp) || lamp < 0) { App.toast("Enter a lamp number", "error"); return; }
        var release = hold([ui.set]);
        App.api("POST", "/api/lamps/test", { lamp: lamp, level: level }).then(function () {
            if (mounted) App.toast("Lamp " + lamp + " set to " + level, "ok");
        }, function (e) {
            if (mounted) App.toast(e.message || "Test failed", "error");
        }).then(release, release);
    }

    var view = {
        title: "Lamps",
        mount: function (root) {
            mounted = true;
            form = blank();
            saved = JSON.stringify(form);
            probed = null;
            rows = [];
            ui = {};
            root.appendChild(el("div", { class: "view-lamps cols" }, busCard(),
                el("div", null, globalCard(), probeCard(), testCard())));
            renderForm();
            load();
        },
        unmount: function () {
            mounted = false;
            if (testTimer) clearTimeout(testTimer);
            testTimer = null;
        },
        poll: function () {
            App.api("GET", "/api/lamps/status").then(showStatus, function () {});
        }
    };

    App.register("lamps", view);
})();
