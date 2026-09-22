// view-scene.js - Scene view: the town clock over a horizon scrubber and
//                 the location dusk and dawn are computed from. What the
//                 buildings do lives on the Models tab, and which lamps
//                 they are on the Houses tab.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el;
    var MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split(" ");
    var CUM = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334];

    var cfg = null;     // the scene as loaded, edited in place
    var changed = false;
    var holdUntil = 0;  // a drag owns the thumb until this moment passes
    var lastSend = 0, timeTimer = null, pendingTime = null;
    var mounted = 0;    // bumped on every mount and unmount, see mount()
    var u = {};         // the nodes poll() updates in place

    function cap(s) { return s.charAt(0).toUpperCase() + s.slice(1); }
    function pct(minutes) { return (minutes / 1440) * 100; }

    function t2m(text) {
        var p = /^(\d+):(\d+)$/.exec(String(text || ""));
        return p ? (parseInt(p[1], 10) * 60 + parseInt(p[2], 10)) % 1440 : 0;
    }

    // 249 -> "6 Sep", counted on a common year the way dayOfYear is.
    function dateLabel(doy) {
        var d = Math.max(1, Math.min(366, doy | 0)), i = 11;
        while (i > 0 && CUM[i] >= d) i--;
        return (d - CUM[i]) + " " + MONTHS[i];
    }

    // Sky stops in ascending order, clamped so they never run backwards:
    // night, the dawn glow, day, the dusk glow, night again.
    function skyGradient(s) {
        var night = "var(--sky-night)", glow = "var(--sky-dusk)", day = "var(--sky-day)";
        var dusk = t2m(s.dusk), dawn = t2m(s.dawn);
        var stops = [[0, night], [dawn - 60, night], [dawn, glow],
                     [t2m(s.sunrise) + 60, day], [t2m(s.sunset) - 60, day],
                     [dusk, glow], [dusk + 60, night], [1440, night]];
        var out = [], last = 0, i, p;
        for (i = 0; i < stops.length; i++) {
            p = Math.max(last, Math.min(1440, stops[i][0]));
            last = p;
            out.push(stops[i][1] + " " + pct(p).toFixed(1) + "%");
        }
        return "linear-gradient(90deg," + out.join(",") + ")";
    }

    // --- small builders ---------------------------------------------------

    function fld(text, input) {
        return el("label", { class: "field" }, el("span", null, text), input);
    }

    function numIn(value, min, max, on) {
        return el("input", { type: "number", value: value, min: min, max: max, oninput: on });
    }

    // A clock field commits on change, never on every keystroke: typing 355
    // through an input event would send day 3, then 35, then 355, and with
    // Keep on each of those writes the scene to flash.
    function numCommit(value, min, max, on) {
        var n = numIn(value, min, max, null);
        n.addEventListener("change", on);
        return n;
    }

    function swtch(input) {
        return el("span", { class: "switch" }, input,
            el("span", { class: "track" }), el("span", { class: "thumb" }));
    }

    // --- clock ------------------------------------------------------------

    function sendClock(body) {
        if (u.keep && u.keep.checked) body.persist = true;
        return App.api("PUT", "/api/scene/clock", body).then(applyStatus, function (e) {
            App.toast(e.message, "error");
        });
    }

    function queueTime(minutes) {
        pendingTime = minutes;
        var wait = 150 - (Date.now() - lastSend);
        if (wait <= 0) flushTime();
        else if (!timeTimer) timeTimer = setTimeout(flushTime, wait);
    }

    function flushTime() {
        timeTimer = null;
        if (pendingTime === null) return;
        var m = pendingTime;
        pendingTime = null;
        lastSend = Date.now();
        sendClock({ mode: "manual", time: App.fmtTime(m) });
    }

    function setThumb(minutes) {
        u.sun.style.left = pct(minutes).toFixed(2) + "%";
        u.time.textContent = App.fmtTime(minutes);
    }

    function onScrub() {
        var v = parseInt(u.range.value, 10) || 0;
        holdUntil = Date.now() + 1500;
        setThumb(v);
        queueTime(v);
    }

    function clockCard() {
        u.time = el("div", { class: "town-time tnum" }, "--:--");
        u.lit = el("div", { class: "lit tnum" }, "");
        u.sky = el("div", { class: "sky" });
        u.sun = el("div", { class: "sun" });
        u.ticks = el("div", { class: "ticks" });
        // Disabled until the first status says the clock is in manual mode,
        // so a grab during the first round trip cannot move the town.
        u.range = el("input", {
            type: "range", min: 0, max: 1439, step: 1, value: 0, disabled: true,
            class: "scrub", "aria-label": "Time of day", oninput: onScrub
        });
        u.wrap = el("div", { class: "sky-wrap" }, u.sky, u.sun, u.range);

        u.seg = el("div", { class: "seg", role: "group", "aria-label": "Clock mode" });
        ["real", "accelerated", "manual"].forEach(function (m) {
            u.seg.appendChild(el("button", {
                type: "button", class: "seg-btn", "data-mode": m, "aria-pressed": "false",
                onclick: function () { sendClock({ mode: m }); }
            }, cap(m)));
        });

        u.doyOut = el("div", { class: "readout tnum" }, "");
        u.doy = numCommit(1, 1, 366, function (ev) {
            var v = parseInt(ev.target.value, 10);
            if (isNaN(v) || v < 1 || v > 366) return;
            u.doyOut.textContent = "day " + v + " = " + dateLabel(v);
            if (cfg && cfg.clock) {
                cfg.clock.dateFromSystem = false;
                touch();
            }
            sendClock({ dayOfYear: v });
        });
        u.speedIn = numCommit(20, 1, 1440, function (ev) {
            var v = parseInt(ev.target.value, 10);
            if (isNaN(v) || v < 1 || v > 1440) return;
            sendClock({ dayMinutes: v });
        });
        u.speed = fld("Real minutes per day", u.speedIn);
        u.speed.hidden = true;
        u.keep = el("input", { type: "checkbox" });

        return el("section", { class: "card" },
            el("div", { class: "clock-head" }, u.time, u.lit),
            u.seg, u.wrap, u.ticks,
            el("div", { class: "gg narrow" },
                fld("Day of year", u.doy), u.speed, fld("Keep", swtch(u.keep))),
            u.doyOut);
    }

    function renderTicks(s) {
        var key = s.dawn + s.dusk;
        if (u.ticks.tickKey === key) return;
        u.ticks.tickKey = key;
        u.ticks.innerHTML = "";
        [["Dawn", s.dawn], ["Dusk", s.dusk]].forEach(function (t) {
            // Near either end the label is aligned inside the strip instead
            // of centred on its tick, so it cannot hang off the edge.
            var p = pct(t2m(t[1]));
            var side = p < 14 ? " start" : (p > 86 ? " end" : "");
            u.ticks.appendChild(el("div", {
                class: "tick", style: "left:" + p.toFixed(1) + "%"
            }, el("span", { class: "tlabel tnum" + side }, t[0] + " " + t[1])));
        });
    }

    function applyStatus(s) {
        if (!s || !u.sky) return;
        u.sky.style.background = skyGradient(s);
        renderTicks(s);

        if (Date.now() >= holdUntil) {
            var m = typeof s.minutes === "number" ? s.minutes : t2m(s.time);
            setThumb(m);
            u.range.value = m;
        }

        var manual = s.mode === "manual", b = u.seg.children, i;
        u.range.disabled = !manual;
        u.wrap.classList.toggle("locked", !manual);
        for (i = 0; i < b.length; i++) {
            b[i].setAttribute("aria-pressed",
                b[i].getAttribute("data-mode") === s.mode ? "true" : "false");
        }

        u.speed.hidden = s.mode !== "accelerated";
        if (document.activeElement !== u.speedIn && typeof s.dayMinutes === "number") {
            u.speedIn.value = s.dayMinutes;
        }
        if (document.activeElement !== u.doy) u.doy.value = s.dayOfYear;
        u.doyOut.textContent = "day " + s.dayOfYear + " = " + dateLabel(s.dayOfYear);
        u.lit.textContent = s.lit + " of " + s.lamps + " lamps lit";

        if (cfg && cfg.clock) {
            cfg.clock.mode = s.mode;
            cfg.clock.dayOfYear = s.dayOfYear;
            if (typeof s.dayMinutes === "number") cfg.clock.dayMinutes = s.dayMinutes;
            if (manual) cfg.clock.manualTime = s.time;
        }
    }

    // --- location ---------------------------------------------------------

    function touch() {
        changed = true;
        updateSave();
    }

    function updateSave() {
        if (!u.save) return;
        u.save.disabled = !cfg;
        u.mark.textContent = changed ? "Unsaved changes" : "";
    }

    // The scene goes back as it arrived, with the location, the clock and
    // the seed as this page has them: the models and the units belong to
    // the other tabs and travel untouched.
    function saveScene() {
        if (!cfg) return;
        u.save.disabled = true;
        App.api("PUT", "/api/scene/config", cfg).then(function (s) {
            changed = false;
            updateSave();
            applyStatus(s);
            App.toast("Scene saved", "ok");
        }, function (e) {
            updateSave();
            App.toast(e.message, "error");
        });
    }

    function locationCard() {
        function setLoc(key) {
            return function (ev) {
                var v = parseFloat(ev.target.value);
                if (isNaN(v) || !cfg) return;
                cfg.location[key] = v;
                touch();
            };
        }
        u.lat = numIn("", -90, 90, setLoc("lat"));
        u.lat.setAttribute("step", "0.01");
        u.lon = numIn("", -180, 180, setLoc("lon"));
        u.lon.setAttribute("step", "0.01");
        u.seed = numIn("", 0, 4294967295, function (ev) {
            var v = parseInt(ev.target.value, 10);
            if (isNaN(v) || !cfg) return;
            cfg.seed = v;
            touch();
        });
        u.save = el("button", {
            type: "button", class: "btn primary", disabled: true, onclick: saveScene
        }, "Save scene");
        u.mark = el("span", { class: "mark" }, "");
        return el("section", { class: "card" },
            el("h2", null, "Location"),
            el("div", { class: "gg" },
                fld("Latitude", u.lat), fld("Longitude", u.lon), fld("Seed", u.seed)),
            el("p", { class: "hint" },
                "Latitude and longitude place dusk and dawn. The seed changes "
                + "everyone's habits, so a new one reshuffles who in town is an "
                + "early riser."),
            el("div", { class: "actions" }, u.save, u.mark));
    }

    // --- view -------------------------------------------------------------

    function mount(root) {
        cfg = null;
        changed = false;
        holdUntil = 0;
        u = {};

        root.appendChild(el("div", { class: "view-scene" },
            clockCard(), locationCard()));

        // The handler checks its token, so a response that lands after the
        // view is gone (or after a remount) is dropped instead of writing
        // into the wrong DOM.
        var token = ++mounted;
        App.api("GET", "/api/scene/config").then(function (c) {
            if (token !== mounted || !c) return;
            cfg = c;
            if (!cfg.location) cfg.location = {};
            if (!cfg.clock) cfg.clock = {};
            u.lat.value = cfg.location.lat;
            u.lon.value = cfg.location.lon;
            u.seed.value = cfg.seed;
            if (typeof cfg.clock.dayMinutes === "number"
                    && document.activeElement !== u.speedIn) {
                u.speedIn.value = cfg.clock.dayMinutes;
            }
            updateSave();
        }, function (e) {
            if (token !== mounted) return;
            App.toast(e.message, "error");
        });
    }

    function unmount() {
        mounted++;
        if (timeTimer) clearTimeout(timeTimer);
        timeTimer = null;
        pendingTime = null;
        cfg = null;
        u = {};
    }

    function poll() {
        App.api("GET", "/api/scene/status").then(applyStatus, function () {});
    }

    App.register("scene", {
        title: "Scene",
        mount: mount,
        unmount: unmount,
        poll: poll
    });
})();
