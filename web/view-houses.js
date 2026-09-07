// view-houses.js - Houses view: the households of the town. Flats are
//                  listed under their building, each row showing who is
//                  awake and which rooms are lit right now; opening a row
//                  gives the household its rooms and its daily rhythm.
//                  Stepping a room's lamp number blinks that lamp on the
//                  layout, so the right one is easy to find.
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
    var identLamp = -1;     // the value that timer will ask for
    var identBad = {};      // lamp numbers the 400 has already been said for

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

    // One pass over every flat. A lamp number the controller can never
    // have (below 0 or above 2047) and a repeat inside one flat are errors
    // and hold Save. A lamp beyond what is fitted today is only a warning:
    // a house is often wired before its driver board is, and the flat
    // must stay editable and deletable meanwhile. A lamp two flats both
    // claim is a warning too, since the first flat listing it gets it.
    function check() {
        errs = [];
        warns = [];
        if (!cfg) return;
        var users = {};
        cfg.flats.forEach(function (f, i) {
            f.rooms.forEach(function (r) {
                if (!users[r.lamp]) users[r.lamp] = [];
                if (users[r.lamp].indexOf(i) < 0) users[r.lamp].push(i);
            });
        });
        cfg.flats.forEach(function (f, i) {
            var seen = {}, err = "", warn = "";
            f.rooms.forEach(function (r) {
                var n = r.lamp;
                var also = users[n] || [];
                if (!err && (typeof n !== "number" || n < 0 || n > 2047)) {
                    err = "Lamp " + n + " is outside 0 to 2047.";
                } else if (!err && seen[n]) {
                    err = "Lamp " + n + " is in this flat twice.";
                } else if (!warn && lampMax && n >= lampMax) {
                    warn = "Lamp " + n + " is beyond the " + lampMax
                        + " lamps fitted; it lights once that hardware is added.";
                } else if (!warn && also.length > 1) {
                    warn = also[0] === i
                        ? "Lamp " + n + " is in " + flatName(also[1])
                            + " too. This flat is listed first and keeps it."
                        : "Lamp " + n + " is in " + flatName(also[0])
                            + " too, which is listed first and keeps it.";
                }
                seen[n] = true;
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

    // Blink the lamp on the layout, so the room being given a number can be
    // found among the houses. A held button and a typed number both arrive
    // here on every change, so the request waits 200 ms for the value to
    // settle and only the value it settles on is sent. Everything but a 400
    // is silent: this is a convenience, and a device that is not answering
    // already says so in the header.
    function identify(n) {
        if (typeof n !== "number" || n < 0) return;
        identLamp = n;
        if (identTimer) clearTimeout(identTimer);
        identTimer = setTimeout(function () {
            identTimer = null;
            var lamp = identLamp;
            App.api("POST", "/api/scene/identify", { lamp: lamp }).then(null,
                function (e) {
                    var msg = (e && e.message) || "";
                    if (!BAD_LAMP.test(msg) || identBad[lamp]) return;
                    identBad[lamp] = true;      // once per value, not per try
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
            d.setAttribute("title", roomLabel(r.role) + ", lamp " + r.lamp);
            box.appendChild(d);
            row.discs.push(d);
        });
    }

    function renderRooms(box, f, i, row) {
        box.innerHTML = "";
        box.appendChild(el("div", { class: "rhead" },
            el("span", null, "Lamp"), el("span", null, "Room")));
        if (!f.rooms.length) {
            box.appendChild(el("p", { class: "hint" },
                "No rooms yet. Add one and give it a lamp."));
        }
        f.rooms.forEach(function (r, k) {
            // Every source of a new number lands here: the two buttons, the
            // arrow keys and typing. write is false while typing, so the
            // field is not rewritten under the caret.
            function commit(n, write) {
                r.lamp = n;
                if (write) lamp.value = n;
                roomDiscs(row.roomBox, f, row);
                paintRow(row, i);
                recheck();
                touch();
                identify(n);
            }
            var lamp = el("input", {
                type: "number", min: 0, max: MAX_LAMP, value: r.lamp,
                inputmode: "numeric",
                "aria-label": "Room " + (k + 1) + " lamp",
                oninput: function (ev) {
                    commit(num(ev.target.value, 0, MAX_LAMP, 0), false);
                }
            });
            // The buttons read the field rather than r.lamp, so stepping
            // after a half-typed number carries on from what is shown.
            function stepper(by, glyph, what) {
                return el("button", {
                    type: "button", class: "stepb",
                    "aria-label": "Room " + (k + 1) + " lamp " + what,
                    onclick: function () {
                        var n = num(lamp.value, 0, MAX_LAMP, r.lamp) + by;
                        commit(Math.max(0, Math.min(MAX_LAMP, n)), true);
                    }
                }, glyph);
            }
            var step = el("div", { class: "step" },
                stepper(-1, "−", "down"), lamp, stepper(1, "+", "up"));
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
            box.appendChild(el("div", { class: "rrow" }, step, role, del));
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
            "Stepping a lamp number blinks that lamp on the layout.");
        row.add = el("button", {
            type: "button", class: "btn secondary rooms-add",
            onclick: function () {
                if (f.rooms.length >= MAX_ROOMS) return;
                f.rooms.push({ lamp: 0, role: "living" });
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

    function save() {
        if (!cfg || !ok()) return;
        u.save.disabled = true;
        App.api("PUT", "/api/scene/config", cfg).then(function () {
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
