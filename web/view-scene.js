// view-scene.js - Scene view: the town clock over a horizon scrubber, the
//                 group editor that gives lamps a behaviour, and the
//                 location dusk and dawn are computed from. Lamps a
//                 household on the Houses view has taken are drawn apart,
//                 since a group no longer drives them.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el;
    // Households first, then the fixtures, then Off: the order a town is
    // built in, not the order the firmware numbers them in.
    var BEHAVIOURS = ["home", "family", "elderly", "nightowl", "away",
                      "shop", "late", "street", "allnight", "off"];
    var BEH_LABELS = {
        home: "Home", family: "Family", elderly: "Elderly couple",
        nightowl: "Night owl", away: "Away", shop: "Shop", late: "Pub, late",
        street: "Street", allnight: "All night", off: "Off"
    };
    var ANCHORS = ["dusk", "dawn", "clock"];
    var MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split(" ");
    var CUM = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334];
    var PRESET_KEYS = ["onAnchor", "on", "offAnchor", "off", "litPercent",
                       "flickerPercent", "morning", "level", "fadeMs",
                       "dayActivity", "nightActivity"];
    var LEVELS = [0, 1, 2, 3];
    var DAY_WORDS = ["None", "Low", "Normal", "High"];
    var NIGHT_WORDS = ["None", "Rare", "Normal", "Often"];
    // Short lights a day per lamp: the engine's day rate at w = 1
    // (0, 0.15, 0.35, 0.70 per hour) times eight, rounded.
    var DAY_COUNT = [0, 1, 3, 6];
    // Wake-ups a night: 0, 0.4, 0.8 and 1.6 expected, said in words.
    var NIGHT_TEXT = ["no wake-ups", "a wake-up every 2 nights",
                      "a wake-up every night", "1 or 2 a night"];
    // Override fields: key, label, kind (a anchor, w window end, n number).
    var OVR = [["onAnchor", "On anchor", "a"], ["on0", "On from", "w"],
               ["on1", "On to", "w"], ["offAnchor", "Off anchor", "a"],
               ["off0", "Off from", "w"], ["off1", "Off to", "w"],
               ["litPercent", "Lit %", "n", 100], ["flickerPercent", "Flicker %", "n", 100],
               ["level", "Level", "n", 255], ["fadeMs", "Fade ms", "n", 60000]];
    var MAX_DISCS = 48;
    var MAX_GROUPS = 16;    // SCENE_MAX_GROUPS in the firmware

    var cfg = null;     // the scene as loaded, edited in place
    var presets = null; // per-behaviour defaults
    var lists = [];     // parsed lamp numbers, one array per group
    var errs = [];      // lamp text error per group, "" when good
    var texts = [];     // the lamp text as it arrived, kept for a bad group
    var lampMax = 0;    // lamps the controller reports, 0 until the first status
    var taken = {};     // lamp -> the flat that owns it, first flat wins
    var expanded = -1, changed = false;
    var holdUntil = 0;  // a drag owns the thumb until this moment passes
    var lastSend = 0, timeTimer = null, pendingTime = null;
    var mounted = 0;    // bumped on every mount and unmount, see mount()
    var u = {};         // the nodes poll() updates in place

    function cap(s) { return s.charAt(0).toUpperCase() + s.slice(1); }
    // A behaviour the device knows and this page does not still reads as
    // something, so an older GUI against a newer firmware is not blank.
    function behLabel(b) { return BEH_LABELS[b] || cap(b); }
    function pct(minutes) { return (minutes / 1440) * 100; }

    function lvl(v) { return Math.max(0, Math.min(3, v | 0)); }

    // The two levels in plain words, so nobody has to think in rates.
    function activityText(day, night) {
        var n = DAY_COUNT[day];
        return (day ? "about " + n + " short light" + (n === 1 ? "" : "s")
                        + " a day per lamp"
                    : "No daytime activity")
            + ", " + NIGHT_TEXT[night];
    }

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

    function sel(options, value, label, on) {
        var s = el("select", { onchange: on });
        options.forEach(function (o) {
            s.appendChild(el("option", { value: o, selected: o === value }, label(o)));
        });
        return s;
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
        if (typeof s.lamps === "number") lampMax = s.lamps;

        if (cfg && cfg.clock) {
            cfg.clock.mode = s.mode;
            cfg.clock.dayOfYear = s.dayOfYear;
            if (typeof s.dayMinutes === "number") cfg.clock.dayMinutes = s.dayMinutes;
            if (manual) cfg.clock.manualTime = s.time;
        }
    }

    // --- groups -----------------------------------------------------------

    function touch() {
        changed = true;
        updateSave();
    }

    function valid() {
        for (var i = 0; i < errs.length; i++) if (errs[i]) return false;
        return true;
    }

    function updateSave() {
        if (!u.save) return;
        u.save.disabled = !cfg || !valid();
        u.mark.textContent = changed ? "Unsaved changes" : "";
    }

    function applyPreset(g, p) {
        PRESET_KEYS.forEach(function (k) {
            g[k] = Array.isArray(p[k]) ? p[k].slice() : p[k];
        });
    }

    // "on0" reads g.on[0], every other key reads g[key].
    function getf(g, key) {
        var m = /^(on|off)([01])$/.exec(key);
        return m ? g[m[1]][+m[2]] : g[key];
    }

    function setf(g, key, v) {
        var m = /^(on|off)([01])$/.exec(key);
        if (m) g[m[1]][+m[2]] = v;
        else g[key] = v;
    }

    // A lamp inside a flat is driven by the household, not by this group,
    // so its disc is a window rather than a street lamp: square corners and
    // a ring, with the flat's name on it.
    function disc(lamp, member) {
        var d = App.lampDisc(member);
        var name = taken[lamp];
        if (name !== undefined) {
            d.className += " taken";
            d.setAttribute("title", "lamp " + lamp + ", in " + name);
        }
        return d;
    }

    // Discs cover the span the group reaches over, so membership reads as a
    // shape: lit for a lamp in the group, dark for a gap. A long span stops
    // at 48 discs and counts the rest.
    function fillDiscs(box, list) {
        box.innerHTML = "";
        if (!list.length) {
            box.appendChild(el("span", { class: "more" }, "no lamps"));
            return;
        }
        var lo = list[0], span = Math.min(list[list.length - 1] - lo + 1, MAX_DISCS);
        var member = {}, i, shown = 0;
        for (i = 0; i < list.length; i++) member[list[i]] = true;
        for (i = 0; i < span; i++) {
            box.appendChild(disc(lo + i, !!member[lo + i]));
            if (member[lo + i]) shown++;
        }
        if (list.length > shown) {
            box.appendChild(el("span", { class: "more tnum" },
                "+" + (list.length - shown) + " more"));
        }
    }

    // "16 lamps, 3 in flats": the second half only when a household has
    // taken some of them, so a town without flats reads as it always did.
    function countText(list) {
        var n = list.length, t = 0;
        for (var i = 0; i < n; i++) if (taken[list[i]] !== undefined) t++;
        return n + (n === 1 ? " lamp" : " lamps") + (t ? ", " + t + " in flats" : "");
    }

    // The red line under the Lamps field. One place, so a group that arrived
    // with lamps this page cannot read looks exactly like one typed wrong.
    function showErr(err, i) {
        err.textContent = errs[i]
            ? errs[i] + ". Write lamp numbers and ranges, like 0-15, 20."
            : "";
        err.hidden = !errs[i];
    }

    // Rebuilding the list drops focus on the document body, so every caller
    // that rebuilds it says where focus should land afterwards.
    function focusRow(i) {
        var rows = u.list.querySelectorAll(".grow");
        if (rows[i]) rows[i].focus();
        else if (u.add) u.add.focus();
    }

    function groupRow(g, i) {
        var open = expanded === i;
        var name = el("span", { class: "gname" }, g.name || "Unnamed group");
        var beh = el("span", { class: "gbeh" }, behLabel(g.behaviour));
        var count = el("span", { class: "gcount tnum" + (errs[i] ? " bad" : "") },
            errs[i] ? "unreadable" : countText(lists[i]));
        var discs = el("div", { class: "discs", "aria-hidden": "true" });
        fillDiscs(discs, lists[i]);

        var node = el("div", { class: "grp" },
            el("button", {
                type: "button", class: "grow", "aria-expanded": open ? "true" : "false",
                onclick: function () {
                    expanded = open ? -1 : i;
                    renderGroups();
                    focusRow(i);
                }
            }, name, beh, count),
            discs);
        if (open) node.appendChild(groupForm(g, i, name, beh, count, discs));
        return node;
    }

    function groupForm(g, i, name, beh, count, discs) {
        if (!g.on) g.on = [0, 0];
        if (!g.off) g.off = [0, 0];
        var f = {}, grid = el("div", { class: "gg" });
        var err = el("div", { class: "err", role: "alert" });
        showErr(err, i);

        OVR.forEach(function (o) {
            var lo = o[2] === "w" ? -1440 : 0, hi = o[2] === "w" ? 2880 : o[3], input;
            if (o[2] === "a") {
                input = sel(ANCHORS, g[o[0]], cap, function (ev) {
                    setf(g, o[0], ev.target.value);
                    touch();
                });
            } else {
                input = numIn(getf(g, o[0]), lo, hi, function (ev) {
                    var v = parseInt(ev.target.value, 10);
                    if (isNaN(v)) return;
                    setf(g, o[0], Math.max(lo, Math.min(hi, v)));
                    touch();
                });
            }
            f[o[0]] = input;
            grid.appendChild(fld(o[1], input));
        });
        f.morning = el("input", {
            type: "checkbox", checked: !!g.morning,
            onchange: function (ev) { g.morning = ev.target.checked; touch(); }
        });
        grid.appendChild(fld("Morning", swtch(f.morning)));

        // The life on top of the base state: two levels and one line that
        // says what they mean, so the rates stay out of the GUI.
        g.dayActivity = lvl(g.dayActivity);
        g.nightActivity = lvl(g.nightActivity);
        var line = el("p", { class: "aline" }, "");
        function syncAct() {
            f.dayActivity.value = g.dayActivity;
            f.nightActivity.value = g.nightActivity;
            line.textContent = activityText(g.dayActivity, g.nightActivity);
        }
        function actSel(key, words) {
            return sel(LEVELS, g[key],
                function (v) { return words[v]; },
                function (ev) {
                    g[key] = lvl(parseInt(ev.target.value, 10));
                    syncAct();
                    touch();
                });
        }
        f.dayActivity = actSel("dayActivity", DAY_WORDS);
        f.nightActivity = actSel("nightActivity", NIGHT_WORDS);
        var act = el("div", { class: "act" },
            el("h3", null, "Activity"),
            el("div", { class: "gg pair" },
                fld("Daytime", f.dayActivity),
                fld("Night wake-ups", f.nightActivity)),
            line);
        syncAct();

        var del = el("button", { type: "button", class: "btn danger" }, "Delete group");
        del.addEventListener("click", function () {
            if (!del.armed) {
                del.armed = true;
                del.textContent = "Tap again to delete";
                return;
            }
            cfg.groups.splice(i, 1);
            lists.splice(i, 1);
            errs.splice(i, 1);
            texts.splice(i, 1);
            expanded = -1;
            touch();
            renderGroups();
            focusRow(i - 1);
        });

        return el("div", { class: "gform" },
            el("div", { class: "gg pair" },
                fld("Name", el("input", {
                    type: "text", maxlength: 23, value: g.name || "",
                    oninput: function (ev) {
                        g.name = ev.target.value;
                        name.textContent = g.name || "Unnamed group";
                        touch();
                    }
                })),
                fld("Behaviour", sel(BEHAVIOURS, g.behaviour, behLabel, function (ev) {
                    g.behaviour = ev.target.value;
                    beh.textContent = behLabel(g.behaviour);
                    var p = presets && presets[g.behaviour];
                    if (p) {
                        applyPreset(g, p);
                        OVR.forEach(function (o) { f[o[0]].value = getf(g, o[0]); });
                        f.morning.checked = g.morning;
                        g.dayActivity = lvl(g.dayActivity);
                        g.nightActivity = lvl(g.nightActivity);
                        syncAct();
                        App.toast("Preset applied", "info");
                    }
                    touch();
                }))),
            fld("Lamps", el("input", {
                // A group whose lamps did not parse keeps the text it came
                // with, so Save writes it back rather than emptying it.
                type: "text", placeholder: "0-15, 20",
                value: errs[i] ? texts[i] : App.rangesText(lists[i]),
                "aria-invalid": errs[i] ? "true" : "false",
                oninput: function (ev) {
                    var text = ev.target.value;
                    texts[i] = text;
                    try {
                        // The lamp count from the last status is the ceiling,
                        // so a typed 5000 is caught here and not in flash.
                        lists[i] = text.trim()
                            ? App.parseRanges(text, lampMax || null) : [];
                        errs[i] = "";
                    } catch (e) {
                        errs[i] = e.message;
                    }
                    showErr(err, i);
                    ev.target.setAttribute("aria-invalid", errs[i] ? "true" : "false");
                    count.classList[errs[i] ? "add" : "remove"]("bad");
                    if (errs[i]) {
                        count.textContent = "unreadable";
                    } else {
                        fillDiscs(discs, lists[i]);
                        count.textContent = countText(lists[i]);
                    }
                    touch();
                }
            })),
            err, grid, act, del);
    }

    function renderGroups() {
        u.list.innerHTML = "";
        if (!cfg) {
            u.list.appendChild(el("p", { class: "hint" }, "Reading the scene."));
            return;
        }
        if (!cfg.groups.length) {
            u.list.appendChild(el("p", { class: "hint" },
                "No groups yet. Add one to give the town something to do."));
        }
        cfg.groups.forEach(function (g, i) { u.list.appendChild(groupRow(g, i)); });
        updateSave();
    }

    function addGroup() {
        if (!cfg) return;
        if (cfg.groups.length >= MAX_GROUPS) {
            App.toast("The controller holds at most " + MAX_GROUPS + " groups", "error");
            return;
        }
        var g = { name: "New group", behaviour: "home", lamps: [] };
        if (presets && presets.home) applyPreset(g, presets.home);
        cfg.groups.push(g);
        lists.push([]);
        errs.push("");
        texts.push("");
        expanded = cfg.groups.length - 1;
        touch();
        renderGroups();
        var nameIn = u.list.querySelector(".gform input[type=text]");
        if (nameIn) {
            nameIn.focus();
            nameIn.select();
        } else {
            focusRow(expanded);
        }
    }

    // The lamp list goes back as the device writes it: numbers and "a-b".
    function lampsJson(list) {
        var text = App.rangesText(list);
        if (!text) return [];
        return text.split(", ").map(function (p) {
            return /^\d+$/.test(p) ? parseInt(p, 10) : p;
        });
    }

    function saveScene() {
        if (!cfg || !valid()) return;
        cfg.groups.forEach(function (g, i) { g.lamps = lampsJson(lists[i]); });
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

    function groupsCard() {
        u.list = el("div", { class: "glist" });
        u.save = el("button", {
            type: "button", class: "btn primary", disabled: true, onclick: saveScene
        }, "Save scene");
        u.mark = el("span", { class: "mark" }, "");
        u.add = el("button", {
            type: "button", class: "btn secondary", onclick: addGroup
        }, "Add group");
        return el("section", { class: "card" },
            el("div", { class: "chead" }, el("h2", null, "Groups"), u.add),
            u.list,
            el("div", { class: "actions" }, u.save, u.mark));
    }

    // --- location ---------------------------------------------------------

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
        return el("section", { class: "card" },
            el("h2", null, "Location"),
            el("div", { class: "gg" },
                fld("Latitude", u.lat), fld("Longitude", u.lon), fld("Seed", u.seed)),
            el("p", { class: "hint" },
                "Latitude and longitude place dusk and dawn. The seed decides who in "
                + "town is an early riser, so a new one reshuffles every habit."));
    }

    // --- view -------------------------------------------------------------

    function mount(root) {
        cfg = null;
        presets = null;
        lists = [];
        errs = [];
        texts = [];
        taken = {};
        expanded = -1;
        changed = false;
        holdUntil = 0;
        u = {};

        root.appendChild(el("div", { class: "view-scene" },
            clockCard(),
            el("div", { class: "cols" }, groupsCard(), locationCard())));
        renderGroups();

        // The two requests are independent: without presets the editor still
        // works, it just cannot re-seed a behaviour from the defaults, so it
        // falls back to whatever the group already carries. Every handler
        // checks its token, so a response that lands after the view is gone
        // (or after a remount) is dropped instead of rendering into the
        // wrong DOM.
        var token = ++mounted;
        Promise.all([
            App.api("GET", "/api/scene/presets").then(null, function () { return null; }),
            App.api("GET", "/api/scene/config")
        ]).then(function (r) {
            if (token !== mounted || !r[1]) return;
            presets = r[0];
            cfg = r[1];
            if (!cfg.groups) cfg.groups = [];
            taken = {};
            (cfg.flats || []).forEach(function (f) {
                (f.rooms || []).forEach(function (rm) {
                    if (taken[rm.lamp] === undefined) taken[rm.lamp] = f.name || "a flat";
                });
            });
            if (!cfg.location) cfg.location = {};
            if (!cfg.clock) cfg.clock = {};
            // A lamp list this page cannot read is kept as text and the
            // group is marked invalid, which disables Save: parsing it as an
            // empty group would write that emptiness to flash on the next
            // save. One toast, for the first such group.
            lists = [];
            errs = [];
            texts = [];
            var unreadable = 0;
            cfg.groups.forEach(function (g) {
                var text = (g.lamps || []).join(", ");
                texts.push(text);
                try {
                    lists.push(text.trim() ? App.parseRanges(text) : []);
                    errs.push("");
                } catch (e) {
                    lists.push([]);
                    errs.push(e.message);
                    if (unreadable === 0) {
                        App.toast("Group \"" + (g.name || "Unnamed group")
                            + "\" has lamps this page cannot read", "error");
                    }
                    unreadable++;
                }
            });
            u.lat.value = cfg.location.lat;
            u.lon.value = cfg.location.lon;
            u.seed.value = cfg.seed;
            if (typeof cfg.clock.dayMinutes === "number"
                    && document.activeElement !== u.speedIn) {
                u.speedIn.value = cfg.clock.dayMinutes;
            }
            renderGroups();
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
