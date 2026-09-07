// view-houses.js - Houses view: the households of the town. Flats are
//                  listed under their building, each row showing who is
//                  awake and which rooms are lit right now; opening a row
//                  gives the household its rooms and its daily rhythm.
//                  A room holds up to eight lamps, written as 4, 6-8, and
//                  its blink button lights the whole room on the layout, so
//                  the right one is easy to find.
//                  Saving writes the whole scene config, groups included.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el;

    var TYPES = ["family", "elderly", "nightowl", "away", "custom"];
    var TYPE_LABELS = {
        family: "Family", elderly: "Elderly couple", nightowl: "Night owl",
        away: "Away", custom: "Custom"
    };
    var ROOMS = ["living", "kitchen", "bedroom", "bathroom", "hall", "other"];
    var ROOM_LABELS = {
        living: "Living room", kitchen: "Kitchen", bedroom: "Bedroom",
        bathroom: "Bathroom", hall: "Hall", other: "Other"
    };
    // Spec 3.5: one letter per lit room, in the status string.
    var ROOM_LETTER = {
        living: "l", kitchen: "k", bedroom: "b",
        bathroom: "t", hall: "h", other: "o"
    };
    var STATES = ["awake", "asleep", "out", "away"];
    // The four rhythm ranges, in the order a day runs.
    var RANGES = [["wake", "Wake"], ["leave", "Leave"],
                  ["home", "Home"], ["bed", "Bed"]];
    // What a type re-seeds when it changes: everything but name, building,
    // weekend and the rooms, which belong to the flat and not to the type.
    var SEED_KEYS = ["wake", "leave", "home", "bed", "outPercent", "tvPercent",
                     "level", "fadeMs", "dayActivity", "nightActivity"];
    var NUMS = [["outPercent", "Out %", 100], ["tvPercent", "TV %", 100],
                ["level", "Level", 255], ["fadeMs", "Fade ms", 60000]];
    var LEVELS = [0, 1, 2, 3];
    var DAY_WORDS = ["None", "Low", "Normal", "High"];
    var NIGHT_WORDS = ["None", "Rare", "Normal", "Often"];
    var MAX_FLATS = 32;   // SCENE_MAX_FLATS
    var MAX_ROOMS = 12;   // FLAT_MAX_ROOMS
    var MAX_LAMP = 2047;  // LAMPS_MAX_LAMPS - 1
    var MAX_ROOM_LAMPS = 8;  // ROOM_MAX_LAMPS
    var STEP_TITLE = "Step works for a single lamp";
    // A pendant lamp: cord, shade, and the light it throws. Drawn here
    // rather than fetched, like every other mark on this page.
    var LAMP_SVG = '<svg viewBox="0 0 24 24" width="21" height="21" '
        + 'fill="none" stroke="currentColor" stroke-width="1.6" '
        + 'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" '
        + 'focusable="false"><path d="M12 3.6v3.8"/>'
        + '<path d="M8.6 7.4h6.8l3.2 7H5.4z"/>'
        + '<circle cx="12" cy="17.4" r="1.7"/></svg>';

    var cfg = null;      // the scene as loaded, edited in place
    var presets = null;  // presets.households, for re-seeding a type
    var errs = [];       // blocking lamp error per flat, "" when good
    var warns = [];      // lamp shared with another flat, "" when none
    var rows = [];       // per-flat nodes poll() writes into
    var lampMax = 0;     // lamps the controller reports, 0 until first status
    var expanded = -1, changed = false;
    var mounted = 0;     // bumped by mount and unmount, drops late replies
    var status = null;   // the last /api/scene/status
    var u = {};
    var identTimer = null;  // the debounce on the identify request
    var identLamps = [];    // the lamps that timer will ask for
    var identBad = {};      // lamp lists the 400 has already been said for

    // --- small helpers ----------------------------------------------------

    function pad2(n) { return (n < 10 ? "0" : "") + n; }

    // Minutes to "HH:MM" without wrapping: a bedtime of 1530 shows as
    // 25:30, so the value the device holds survives a round trip through
    // the field. -1 (never) is an empty field.
    function fmtHM(m) {
        if (typeof m !== "number" || m < 0) return "";
        return pad2(Math.floor(m / 60)) + ":" + pad2(m % 60);
    }

    // "" -> -1 (never), "07:30" -> 450, "25:30" -> 1530. null on anything
    // else, which the caller treats as "keep what was there".
    function parseHM(text) {
        var t = String(text || "").trim();
        if (t === "") return -1;
        var m = /^(\d{1,2}):(\d{2})$/.exec(t);
        if (!m) return null;
        var h = parseInt(m[1], 10), mm = parseInt(m[2], 10);
        if (h > 47 || mm > 59) return null;
        return h * 60 + mm;
    }

    function mid(pair) {
        if (!pair || pair[0] < 0 || pair[1] < 0) return -1;
        return Math.round((pair[0] + pair[1]) / 2);
    }

    function num(v, lo, hi, dflt) {
        var n = parseInt(v, 10);
        if (isNaN(n)) return dflt;
        return Math.max(lo, Math.min(hi, n));
    }

    function lvl(v) { return Math.max(0, Math.min(3, v | 0)); }

    function fld(text, input) {
        return el("label", { class: "field" }, el("span", null, text), input);
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

    function typeLabel(t) { return TYPE_LABELS[t] || t; }
    function roomLabel(r) { return ROOM_LABELS[r] || r; }

    // The household in one line, read off the midpoints of its ranges so
    // it says what the flat will roughly do, not what was typed.
    function rhythmText(f) {
        if (f.type === "away") {
            return "No rhythm. One lamp on a timer, 19:00 to 22:30.";
        }
        var parts = [];
        var w = mid(f.wake), l = mid(f.leave), h = mid(f.home), b = mid(f.bed);
        if (w >= 0) parts.push("wakes around " + App.fmtTime(w));
        if (l >= 0) parts.push("leaves " + App.fmtTime(l));
        if (h >= 0) parts.push("home " + App.fmtTime(h));
        if (b >= 0) parts.push("bed " + App.fmtTime(b));
        var out;
        if (!f.outPercent) {
            out = "never out";
        } else {
            var n = Math.round(100 / f.outPercent);
            out = n <= 1 ? "out most evenings" : "out one evening in " + n;
        }
        if (!parts.length) return "No rhythm set.";
        return parts.join(", ") + "; " + out;
    }

    // --- validation -------------------------------------------------------

    function flatName(i) {
        var f = cfg.flats[i];
        return (f && f.name) || "another flat";
    }

    // The lamps of one room, as the field says them. r.err carries what
    // App.parseRanges said about the text, so a room that cannot be read
    // is told apart from one nobody has filled in yet.
    function readRoom(r, text) {
        r.text = text;
        try {
            r.lamps = App.parseRanges(text, MAX_LAMP + 1);
            r.err = "";
        } catch (e) {
            r.lamps = [];
            r.err = /empty range/.test(e.message) ? "empty" : e.message;
        }
    }

    // A room as it arrives: a list written like a group's lamps, or, from a
    // controller older than this GUI, a single "lamp" number.
    function fromJson(r) {
        var src = Array.isArray(r.lamps) ? r.lamps
            : (typeof r.lamp === "number" ? [r.lamp] : []);
        readRoom(r, src.join(", "));
        delete r.lamp;
    }

    // Text this room cannot be read from, more than eight lamps in it, and
    // a lamp the flat lists twice all hold Save.
    function roomErr(r, k, seen) {
        var name = "Room " + (k + 1);
        if (r.err === "empty") {
            return name + " has no lamp yet. Write a number, or remove the row.";
        }
        if (r.err) {
            return name + ": " + r.err
                + ". Write lamp numbers and ranges, like 4, 6-8.";
        }
        if (r.lamps.length > MAX_ROOM_LAMPS) {
            return name + " has " + r.lamps.length + " lamps. A room holds at most "
                + MAX_ROOM_LAMPS + ".";
        }
        for (var j = 0; j < r.lamps.length; j++) {
            if (seen[r.lamps[j]]) {
                return "Lamp " + r.lamps[j] + " is in this flat twice.";
            }
        }
        return "";
    }

    // A lamp beyond what is fitted today is only a warning: a house is
    // often wired before its driver board is, and the flat must stay
    // editable and deletable meanwhile. A lamp two flats both claim is a
    // warning too, since the first flat listing it gets it.
    function roomWarn(r, i, users) {
        for (var j = 0; j < r.lamps.length; j++) {
            var n = r.lamps[j];
            if (lampMax && n >= lampMax) {
                return "Lamp " + n + " is beyond the " + lampMax
                    + " lamps fitted; it lights once that hardware is added.";
            }
            var also = users[n] || [];
            if (also.length > 1) {
                return also[0] === i
                    ? "Lamp " + n + " is in " + flatName(also[1])
                        + " too. This flat is listed first and keeps it."
                    : "Lamp " + n + " is in " + flatName(also[0])
                        + " too, which is listed first and keeps it.";
            }
        }
        return "";
    }

    // One pass over every flat, keeping the first error and the first
    // warning of each: one line each is what the open editor shows.
    function check() {
        errs = [];
        warns = [];
        if (!cfg) return;
        var users = {};
        cfg.flats.forEach(function (f, i) {
            f.rooms.forEach(function (r) {
                r.lamps.forEach(function (n) {
                    if (!users[n]) users[n] = [];
                    if (users[n].indexOf(i) < 0) users[n].push(i);
                });
            });
        });
        cfg.flats.forEach(function (f, i) {
            var seen = {}, err = "", warn = "";
            f.rooms.forEach(function (r, k) {
                if (!err) err = roomErr(r, k, seen);
                if (!warn) warn = roomWarn(r, i, users);
                r.lamps.forEach(function (n) { seen[n] = true; });
            });
            errs.push(err);
            warns.push(warn);
        });
    }

    function ok() {
        for (var i = 0; i < errs.length; i++) if (errs[i]) return false;
        return true;
    }

    function updateSave() {
        if (!u.save) return;
        u.save.disabled = !cfg || !ok();
        u.mark.textContent = changed ? "Unsaved changes" : "";
    }

    function setNote(node, text) {
        if (!node) return;
        node.textContent = text;
        node.hidden = !text;
    }

    // Re-run validation and write the result into the nodes that show it,
    // without rebuilding the list: the open editor keeps its focus.
    function recheck() {
        check();
        rows.forEach(function (row, i) {
            row.bad.hidden = !errs[i];
            setNote(row.err, errs[i]);
            setNote(row.warn, warns[i]);
        });
        updateSave();
    }

    function touch() {
        changed = true;
        updateSave();
    }

    // --- live state -------------------------------------------------------

    function statusFor(i, name) {
        var list = status && status.flats;
        if (!list) return null;
        if (list[i] && list[i].name === name) return list[i];
        for (var j = 0; j < list.length; j++) {
            if (list[j].name === name) return list[j];
        }
        return null;
    }

    // The lit string carries one letter per lit room, so count the letters
    // and light that many rooms of each role, in the flat's own order.
    function litCounts(lit) {
        var c = {}, s = String(lit || ""), i;
        for (i = 0; i < s.length; i++) c[s[i]] = (c[s[i]] || 0) + 1;
        return c;
    }

    function paintRow(row, i) {
        var f = cfg.flats[i];
        var s = statusFor(i, f.name);
        var state = s && STATES.indexOf(s.state) >= 0 ? s.state : "";
        row.glyph.className = "st " + state;
        row.word.textContent = state || "no reading";
        var c = litCounts(s && s.lit);
        var names = [];
        row.discs.forEach(function (d, k) {
            var role = f.rooms[k] ? f.rooms[k].role : "other";
            var on = c[ROOM_LETTER[role]] > 0;
            if (on) {
                c[ROOM_LETTER[role]]--;
                names.push(roomLabel(role).toLowerCase());
            }
            d.className = "rd " + (on ? "on" : "off");
        });
        // The discs are a shape; this line is the same thing in words.
        row.sr.textContent = names.length
            ? "lit: " + names.join(", ") : "no room lit";
    }

    function paint() {
        if (!cfg) return;
        rows.forEach(paintRow);
    }

    // --- identify ---------------------------------------------------------

    // The one answer worth a word: the number is not a lamp this controller
    // has. App.api rejects with the server's own text and not with the
    // status code, so a 400 from this route is recognised by what the route
    // says, plus the line App.api writes when the body does not parse.
    var BAD_LAMP = /^(lamp must be|no lamps are fitted|request failed: 400)/;

    // Blink the room's lamps on the layout, all of them together, so the
    // room being given its numbers can be found among the houses. A held
    // button, a typed list and the blink button all arrive here on every
    // change, so the request waits 200 ms for the list to settle and only
    // the list it settles on is sent. A list this page already knows the
    // device will refuse is not sent at all. Everything but a 400 is
    // silent: this is a convenience, and a device that is not answering
    // already says so in the header.
    function identify(lamps) {
        // A new value, valid or not, cancels whatever blink was pending,
        // so a list the field no longer shows can never fire late.
        if (identTimer) clearTimeout(identTimer);
        identTimer = null;
        if (!lamps || !lamps.length || lamps.length > MAX_ROOM_LAMPS) return;
        identLamps = lamps.slice();
        identTimer = setTimeout(function () {
            identTimer = null;
            var list = identLamps, key = list.join(",");
            App.api("POST", "/api/scene/identify", { lamps: list }).then(null,
                function (e) {
                    var msg = (e && e.message) || "";
                    if (!BAD_LAMP.test(msg) || identBad[key]) return;
                    identBad[key] = true;      // once per list, not per try
                    App.toast(msg, "error");
                });
        }, 200);
    }

    // Leaving the view drops a blink that has not been asked for yet.
    function stopIdentify() {
        if (identTimer) clearTimeout(identTimer);
        identTimer = null;
    }

    // --- rooms ------------------------------------------------------------

    function roomDiscs(box, f, row) {
        box.innerHTML = "";
        row.discs = [];
        if (!f.rooms.length) {
            box.appendChild(el("span", { class: "rnone" }, "no rooms"));
            return;
        }
        f.rooms.forEach(function (r) {
            var d = el("span", { class: "rd off" },
                ROOM_LETTER[r.role] || "o");
            d.setAttribute("title", roomLabel(r.role) + ", "
                + (!r.lamps.length ? "no lamp yet"
                    : (r.lamps.length === 1 ? "lamp " : "lamps ")
                        + App.rangesText(r.lamps)));
            box.appendChild(d);
            row.discs.push(d);
        });
    }

    function renderRooms(box, f, i, row) {
        box.innerHTML = "";
        box.appendChild(el("div", { class: "rhead" },
            el("span", null, "Lamps"), el("span", null, "Room")));
        if (!f.rooms.length) {
            box.appendChild(el("p", { class: "hint" },
                "No rooms yet. Add one and give it a lamp."));
        }
        f.rooms.forEach(function (r, k) {
            // Every source of a new list lands here: the two step buttons
            // and typing. write is false while typing, so the field is not
            // rewritten under the caret.
            function commit(text, write) {
                readRoom(r, text);
                if (write) field.value = text;
                syncStep();
                roomDiscs(row.roomBox, f, row);
                paintRow(row, i);
                recheck();
                touch();
                identify(r.lamps);
            }
            var field = el("input", {
                type: "text", class: "rlamps", value: r.text,
                inputmode: "numeric", autocomplete: "off", spellcheck: "false",
                placeholder: "4, 6-8",
                "aria-label": "Room " + (k + 1) + " lamps",
                oninput: function (ev) { commit(ev.target.value, false); }
            });
            // Stepping means one lamp up or down, which a list has no
            // answer to, so with anything but a single number the two
            // buttons are off and say why.
            function stepper(by, glyph, what) {
                return el("button", {
                    type: "button", class: "stepb",
                    "aria-label": "Room " + (k + 1) + " lamp " + what,
                    onclick: function () {
                        if (r.lamps.length !== 1) return;
                        commit(String(Math.max(0, Math.min(MAX_LAMP,
                            r.lamps[0] + by))), true);
                    }
                }, glyph);
            }
            var down = stepper(-1, "−", "down"), up = stepper(1, "+", "up");
            var step = el("div", { class: "step" }, down, up);
            var blink = el("button", {
                type: "button", class: "blinkb", html: LAMP_SVG,
                title: "Blink this room", "aria-label": "Blink this room",
                onclick: function () { identify(r.lamps); }
            });
            function syncStep() {
                var one = r.lamps.length === 1;
                [down, up, step].forEach(function (node) {
                    if (node !== step) node.disabled = !one;
                    if (one) node.removeAttribute("title");
                    else node.setAttribute("title", STEP_TITLE);
                });
                blink.disabled = !r.lamps.length
                    || r.lamps.length > MAX_ROOM_LAMPS;
            }
            syncStep();
            var role = sel(ROOMS, r.role, roomLabel, function (ev) {
                r.role = ev.target.value;
                roomDiscs(row.roomBox, f, row);
                paintRow(row, i);
                touch();
            });
            role.setAttribute("aria-label", "Room " + (k + 1) + " kind");
            var del = el("button", {
                type: "button", class: "rdel", "aria-label": "Remove room " + (k + 1),
                onclick: function () {
                    f.rooms.splice(k, 1);
                    renderRooms(box, f, i, row);
                    roomDiscs(row.roomBox, f, row);
                    paintRow(row, i);
                    recheck();
                    touch();
                    var next = box.querySelectorAll(".rdel");
                    if (next[k]) next[k].focus();
                    else if (next[k - 1]) next[k - 1].focus();
                    else row.add.focus();
                }
            }, "×");
            box.appendChild(el("div", { class: "rrow" },
                field, role, blink, del, step));
        });
        row.add.disabled = f.rooms.length >= MAX_ROOMS;
    }

    // --- the editor -------------------------------------------------------

    function flatForm(f, i, row) {
        var tin = {};   // the eight time inputs, by range key
        var fin = {};   // the number inputs and selects, by field key
        var line = el("p", { class: "rline" }, rhythmText(f));

        function syncLine() { line.textContent = rhythmText(f); }

        function syncFields() {
            RANGES.forEach(function (rg) {
                tin[rg[0]][0].value = fmtHM(f[rg[0]][0]);
                tin[rg[0]][1].value = fmtHM(f[rg[0]][1]);
            });
            NUMS.forEach(function (n) { fin[n[0]].value = f[n[0]]; });
            fin.dayActivity.value = f.dayActivity;
            fin.nightActivity.value = f.nightActivity;
            syncLine();
        }

        // A range is a pair: emptying either end means "never", and filling
        // one end of an empty pair fills both, so a half-set range cannot
        // reach the device. A "to" earlier than its "from" is read as the
        // next day, which is how a bedtime of 00:45 is stored as 1485.
        function commitTime(key, idx, input) {
            var v = parseHM(input.value);
            if (v === null) {
                input.value = fmtHM(f[key][idx]);
                App.toast("Write a time as HH:MM, or leave it empty for never",
                    "error");
                return;
            }
            if (v < 0) {
                f[key][0] = -1;
                f[key][1] = -1;
            } else {
                f[key][idx] = v;
                if (f[key][1 - idx] < 0) f[key][1 - idx] = v;
                if (f[key][1] < f[key][0]) {
                    if (idx === 1) f[key][1] = v + 1440;
                    else f[key][1] = f[key][0];
                }
            }
            tin[key][0].value = fmtHM(f[key][0]);
            tin[key][1].value = fmtHM(f[key][1]);
            syncLine();
            touch();
        }

        function timeIn(key, idx, label) {
            var input = el("input", {
                type: "text", inputmode: "numeric", placeholder: "never",
                maxlength: 5, class: "tin", value: fmtHM(f[key][idx]),
                "aria-label": label + " " + (idx ? "to" : "from")
            });
            input.addEventListener("change", function () {
                commitTime(key, idx, input);
            });
            return input;
        }

        var name = el("input", {
            type: "text", maxlength: 23, value: f.name || "",
            oninput: function (ev) {
                f.name = ev.target.value;
                row.name.textContent = f.name || "Unnamed flat";
                recheck();
                touch();
            }
        });
        var building = el("input", {
            type: "text", maxlength: 23, value: f.building || "",
            placeholder: "Other",
            oninput: function (ev) { f.building = ev.target.value; touch(); }
        });
        var type = sel(TYPES, f.type, typeLabel, function (ev) {
            f.type = ev.target.value;
            row.type.textContent = typeLabel(f.type);
            var p = presets && presets[f.type];
            if (p) {
                SEED_KEYS.forEach(function (k) {
                    f[k] = Array.isArray(p[k]) ? p[k].slice() : p[k];
                });
                syncFields();
                App.toast("Rhythm re-seeded from " + typeLabel(f.type), "info");
            } else {
                syncLine();
            }
            touch();
        });
        var weekend = el("input", {
            type: "checkbox", checked: !!f.weekend,
            onchange: function (ev) { f.weekend = ev.target.checked; touch(); }
        });

        var roomBox = el("div", { class: "rtable" });
        // One line per open flat, not per room: the stepper is the same
        // control twelve times over.
        var rhint = el("p", { class: "hint rhint" },
            "A room can hold up to eight lamps, written as 4, 6-8. The lamp "
            + "button blinks the whole room on the layout, and so does the "
            + "stepper on a room with one lamp.");
        row.add = el("button", {
            type: "button", class: "btn secondary rooms-add",
            onclick: function () {
                if (f.rooms.length >= MAX_ROOMS) return;
                f.rooms.push({ lamps: [0], text: "0", err: "", role: "living" });
                renderRooms(roomBox, f, i, row);
                roomDiscs(row.roomBox, f, row);
                paintRow(row, i);
                recheck();
                touch();
                var ins = roomBox.querySelectorAll(".rrow input");
                var last = ins[ins.length - 1];
                if (last) {
                    last.focus();
                    last.select();
                }
            }
        }, "Add room");
        renderRooms(roomBox, f, i, row);

        var rng = el("div", { class: "rhythm" });
        RANGES.forEach(function (rg) {
            tin[rg[0]] = [timeIn(rg[0], 0, rg[1]), timeIn(rg[0], 1, rg[1])];
            rng.appendChild(el("div", { class: "rng" },
                el("span", { class: "rlab" }, rg[1]),
                fld("from", tin[rg[0]][0]), fld("to", tin[rg[0]][1])));
        });

        var grid = el("div", { class: "fg" });
        NUMS.forEach(function (n) {
            fin[n[0]] = el("input", {
                type: "number", min: 0, max: n[2], value: f[n[0]],
                oninput: function (ev) {
                    f[n[0]] = num(ev.target.value, 0, n[2], f[n[0]]);
                    if (n[0] === "outPercent") syncLine();
                    touch();
                }
            });
            grid.appendChild(fld(n[1], fin[n[0]]));
        });
        fin.dayActivity = sel(LEVELS, lvl(f.dayActivity),
            function (v) { return DAY_WORDS[v]; },
            function (ev) { f.dayActivity = lvl(ev.target.value); touch(); });
        fin.nightActivity = sel(LEVELS, lvl(f.nightActivity),
            function (v) { return NIGHT_WORDS[v]; },
            function (ev) { f.nightActivity = lvl(ev.target.value); touch(); });
        grid.appendChild(fld("Daytime", fin.dayActivity));
        grid.appendChild(fld("Night wake-ups", fin.nightActivity));

        row.err = el("p", { class: "err", role: "alert", hidden: true });
        row.warn = el("p", { class: "warn", hidden: true });
        setNote(row.err, errs[i]);
        setNote(row.warn, warns[i]);

        var del = el("button", { type: "button", class: "btn danger" },
            "Delete flat");
        del.addEventListener("click", function () {
            if (!del.armed) {
                del.armed = true;
                del.textContent = "Tap again to delete";
                return;
            }
            cfg.flats.splice(i, 1);
            expanded = -1;
            touch();
            render();
            focusRow(i - 1);
        });

        return el("div", { class: "fform" },
            el("div", { class: "fg" }, fld("Name", name), fld("Building", building)),
            el("div", { class: "fg" }, fld("Household", type),
                el("label", { class: "field wk" },
                    el("span", null, "Weekend rhythm"), swtch(weekend))),
            line,
            el("h3", null, "Rooms"),
            row.err, row.warn, roomBox, rhint, row.add,
            el("h3", null, "Rhythm"), rng, grid,
            del);
    }

    // --- list -------------------------------------------------------------

    function focusRow(i) {
        var all = u.list.querySelectorAll(".frow");
        if (all[i]) all[i].focus();
        else if (u.add) u.add.focus();
    }

    function flatRow(f, i) {
        var open = expanded === i;
        var row = { discs: [] };
        row.name = el("span", { class: "fname" }, f.name || "Unnamed flat");
        row.type = el("span", { class: "ftype" }, typeLabel(f.type));
        row.glyph = el("span", { class: "st", "aria-hidden": "true" });
        row.word = el("span", { class: "fword" }, "no reading");
        row.bad = el("span", { class: "fbad", hidden: !errs[i] },
            el("span", { class: "ring" }, "!"), "check rooms");
        row.sr = el("span", { class: "sr" }, "");
        row.roomBox = el("span", { class: "frooms", "aria-hidden": "true" });
        roomDiscs(row.roomBox, f, row);

        var btn = el("button", {
            type: "button", class: "frow",
            "aria-expanded": open ? "true" : "false",
            onclick: function () {
                expanded = open ? -1 : i;
                render();
                focusRow(i);
            }
        },
            el("span", { class: "ftop" }, row.name,
                el("span", { class: "fstate" }, row.glyph, row.word)),
            el("span", { class: "fmeta" }, row.type, row.bad, row.roomBox, row.sr));

        var node = el("div", { class: "flat" }, btn);
        if (open) node.appendChild(flatForm(f, i, row));
        rows[i] = row;
        return node;
    }

    // Flats sit under the building they are in, in the order the buildings
    // first appear; a flat with no building lands under Other.
    function render() {
        rows = [];
        u.list.innerHTML = "";
        if (!cfg) {
            u.list.appendChild(el("p", { class: "hint" }, "Reading the scene."));
            return;
        }
        check();
        if (!cfg.flats.length) {
            u.list.appendChild(el("p", { class: "hint" },
                "No households yet. Add a flat to give a building its own rhythm."));
        }
        var order = [], boxes = {};
        cfg.flats.forEach(function (f) {
            var b = (f.building || "").trim() || "Other";
            if (!boxes[b]) {
                boxes[b] = el("div", { class: "bflats" });
                order.push(b);
                u.list.appendChild(el("div", { class: "building" },
                    el("h3", { class: "bname" }, b), boxes[b]));
            }
        });
        cfg.flats.forEach(function (f, i) {
            var b = (f.building || "").trim() || "Other";
            boxes[b].appendChild(flatRow(f, i));
        });
        paint();
        updateSave();
    }

    function addFlat() {
        if (!cfg) return;
        if (cfg.flats.length >= MAX_FLATS) {
            App.toast("The controller holds at most " + MAX_FLATS + " households",
                "error");
            return;
        }
        var f = {
            name: "Flat " + (cfg.flats.length + 1), building: "", type: "family",
            weekend: true, rooms: [], wake: [390, 435], leave: [450, 495],
            home: [960, 1050], bed: [1350, 1410], outPercent: 14, tvPercent: 70,
            level: 210, fadeMs: 300, dayActivity: 3, nightActivity: 1
        };
        var p = presets && presets.family;
        if (p) {
            SEED_KEYS.forEach(function (k) {
                f[k] = Array.isArray(p[k]) ? p[k].slice() : p[k];
            });
        }
        cfg.flats.push(f);
        expanded = cfg.flats.length - 1;
        touch();
        render();
        var nameIn = u.list.querySelector(".fform input[type=text]");
        if (nameIn) {
            nameIn.focus();
            nameIn.select();
        } else {
            focusRow(expanded);
        }
    }

    // The scene as the device wants it. The room rows carry the text the
    // field holds and what the parser made of it, which are this page's
    // business, so the rooms are written out again as lamps and a role.
    function payload() {
        var out = {}, key;
        for (key in cfg) {
            if (Object.prototype.hasOwnProperty.call(cfg, key)) out[key] = cfg[key];
        }
        out.flats = cfg.flats.map(function (f) {
            var g = {}, k;
            for (k in f) {
                if (Object.prototype.hasOwnProperty.call(f, k)) g[k] = f[k];
            }
            g.rooms = f.rooms.map(function (r) {
                return { lamps: r.lamps.slice(), role: r.role };
            });
            return g;
        });
        return out;
    }

    function save() {
        if (!cfg || !ok()) return;
        u.save.disabled = true;
        App.api("PUT", "/api/scene/config", payload()).then(function () {
            changed = false;
            updateSave();
            App.toast("Households saved", "ok");
        }, function (e) {
            updateSave();
            App.toast(e.message, "error");
        });
    }

    // --- view -------------------------------------------------------------

    function mount(root) {
        cfg = null;
        presets = null;
        rows = [];
        errs = [];
        warns = [];
        expanded = -1;
        changed = false;
        status = null;
        u = {};

        u.list = el("div", { class: "flist" });
        u.add = el("button", {
            type: "button", class: "btn secondary", onclick: addFlat
        }, "Add flat");
        u.save = el("button", {
            type: "button", class: "btn primary", disabled: true, onclick: save
        }, "Save households");
        u.mark = el("span", { class: "mark" }, "");

        root.appendChild(el("div", { class: "view-houses" },
            el("section", { class: "card" },
                el("div", { class: "chead" },
                    el("h2", null, "Households"), u.add),
                u.list,
                el("div", { class: "actions" }, u.save, u.mark))));
        render();

        // Presets only re-seed a type, so the editor still works without
        // them; the config is the one request that has to land.
        var token = ++mounted;
        Promise.all([
            App.api("GET", "/api/scene/presets").then(null, function () { return null; }),
            App.api("GET", "/api/scene/config")
        ]).then(function (r) {
            if (token !== mounted || !r[1]) return;
            presets = r[0] && r[0].households;
            cfg = r[1];
            if (!cfg.flats) cfg.flats = [];
            cfg.flats.forEach(function (f) {
                if (!Array.isArray(f.rooms)) f.rooms = [];
                f.rooms.forEach(fromJson);
                RANGES.forEach(function (rg) {
                    if (!Array.isArray(f[rg[0]])) f[rg[0]] = [-1, -1];
                });
            });
            render();
            poll();
        }, function (e) {
            if (token !== mounted) return;
            App.toast(e.message, "error");
        });
    }

    function unmount() {
        mounted++;
        stopIdentify();
        identBad = {};
        cfg = null;
        rows = [];
        u = {};
    }

    // The state words and the lit rooms are the only things poll() writes:
    // no node is rebuilt, so an open editor keeps its focus and its text.
    function poll() {
        var token = mounted;
        App.api("GET", "/api/scene/status").then(function (s) {
            if (token !== mounted || !s || !cfg) return;
            status = s;
            if (typeof s.lamps === "number" && s.lamps !== lampMax) {
                lampMax = s.lamps;
                recheck();
            }
            paint();
        }, function () {
            // the offline badge already says it
        });
    }

    App.register("houses", {
        title: "Houses",
        mount: mount,
        unmount: unmount,
        poll: poll
    });
})();
