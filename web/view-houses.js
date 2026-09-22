// view-houses.js - Houses view: the things on the layout. Units are listed
//                  under the building they are in, each row showing which
//                  model it runs, what it is doing and which rooms are lit
//                  right now; opening a row gives the unit its model and
//                  its rooms. A room holds up to eight runs of lamps,
//                  written as 4, 6-8, and its blink button lights the room
//                  on the layout, so the right one is easy to find.
//                  Saving writes the whole scene config, models included.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el;

    var ROOMS = ["living", "kitchen", "bedroom", "bathroom", "hall",
                 "front", "back", "sign", "other"];
    var ROOM_LABELS = {
        living: "Living room", kitchen: "Kitchen", bedroom: "Bedroom",
        bathroom: "Bathroom", hall: "Hall", front: "Shop front",
        back: "Back room", sign: "Sign", other: "Other"
    };
    // Spec 3.3: one letter per lit room, in the status string.
    var ROOM_LETTER = {
        living: "l", kitchen: "k", bedroom: "b", bathroom: "t", hall: "h",
        front: "f", back: "r", sign: "s", other: "o"
    };
    // A household is awake, asleep, out or away; a unit on opening hours
    // is open or closed when both its anchors are clock, lit or dark
    // otherwise.
    var STATES = ["awake", "asleep", "out", "away",
                  "open", "closed", "lit", "dark"];
    var MAX_UNITS = 24;      // SCENE_MAX_UNITS
    var MAX_ROOMS = 12;      // UNIT_MAX_ROOMS
    var MAX_LAMP = 2047;     // LAMPS_MAX_LAMPS - 1
    var MAX_ROOM_RANGES = 8; // ROOM_MAX_RANGES
    var MAX_IDENT = 8;       // lamps one blink can ask for
    var NO_BUILDING = "No building";
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
    var errs = [];       // blocking lamp error per unit, "" when good
    var warns = [];      // lamp shared with another unit, "" when none
    var rows = [];       // per-unit nodes poll() writes into
    var lampMax = 0;     // lamps the controller reports, 0 until first status
    var expanded = -1, changed = false;
    var mounted = 0;     // bumped by mount and unmount, drops late replies
    var status = null;   // the last /api/scene/status
    var u = {};
    var identTimer = null;  // the debounce on the identify request
    var identLamps = [];    // the lamps that timer will ask for
    var identBad = {};      // lamp lists the 400 has already been said for

    // --- small helpers ----------------------------------------------------

    function has(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }

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

    function roomLabel(r) { return ROOM_LABELS[r] || r; }

    // --- models -----------------------------------------------------------

    function modelNames() {
        return (cfg.models || []).map(function (m) { return m.name; });
    }

    // The kind of the model a unit runs, for the lighter face beside the
    // selector. A name no model carries reads as nothing at all.
    function modelKind(name) {
        var list = cfg.models || [];
        for (var i = 0; i < list.length; i++) {
            if (list[i].name === name) {
                return list[i].kind === "hours" ? "hours" : "rhythm";
            }
        }
        return "";
    }

    // --- validation -------------------------------------------------------

    function unitName(i) {
        var un = cfg.units[i];
        return (un && un.name) || "another unit";
    }

    // Consecutive numbers are one range: "0, 1-3" is one run and "4, 6-8"
    // is two, which is what the device counts against ROOM_MAX_RANGES.
    function runCount(list) {
        var n = 0;
        for (var i = 0; i < list.length; i++) {
            if (i === 0 || list[i] !== list[i - 1] + 1) n++;
        }
        return n;
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
        r.ranges = runCount(r.lamps);
    }

    // A room as it arrives: a list written like a model's windows are not,
    // as numbers and "a-b" strings, or, from a controller older than this
    // GUI, a single "lamp" number.
    function fromJson(r) {
        var src = Array.isArray(r.lamps) ? r.lamps
            : (typeof r.lamp === "number" ? [r.lamp] : []);
        readRoom(r, src.join(", "));
        delete r.lamp;
    }

    // Text this room cannot be read from, more than eight runs of lamps in
    // it, and a lamp the unit lists twice all hold Save.
    function roomErr(r, k, seen) {
        var name = "Room " + (k + 1);
        if (r.err === "empty") {
            return name + " has no lamp yet. Write a number, or remove the row.";
        }
        if (r.err) {
            return name + ": " + r.err
                + ". Write lamp numbers and ranges, like 4, 6-8.";
        }
        if (r.ranges > MAX_ROOM_RANGES) {
            return name + " has " + r.ranges + " runs of lamps. A room holds at "
                + "most " + MAX_ROOM_RANGES + ".";
        }
        for (var j = 0; j < r.lamps.length; j++) {
            if (seen[r.lamps[j]]) {
                return "Lamp " + r.lamps[j] + " is in this unit twice.";
            }
        }
        return "";
    }

    // A lamp beyond what is fitted today is only a warning: a house is
    // often wired before its driver board is, and the unit must stay
    // editable and deletable meanwhile. A lamp two units both claim is a
    // warning too, since the first unit listing it gets it.
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
                    ? "Lamp " + n + " is in " + unitName(also[1])
                        + " too. This unit is listed first and keeps it."
                    : "Lamp " + n + " is in " + unitName(also[0])
                        + " too, which is listed first and keeps it.";
            }
        }
        return "";
    }

    // One pass over every unit, keeping the first error and the first
    // warning of each: one line each is what the open editor shows.
    function check() {
        errs = [];
        warns = [];
        if (!cfg) return;
        var users = {};
        cfg.units.forEach(function (un, i) {
            un.rooms.forEach(function (r) {
                r.lamps.forEach(function (n) {
                    if (!users[n]) users[n] = [];
                    if (users[n].indexOf(i) < 0) users[n].push(i);
                });
            });
        });
        cfg.units.forEach(function (un, i) {
            var seen = {}, err = "", warn = "";
            un.rooms.forEach(function (r, k) {
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
        var list = status && status.units;
        if (!list) return null;
        if (list[i] && list[i].name === name) return list[i];
        for (var j = 0; j < list.length; j++) {
            if (list[j].name === name) return list[j];
        }
        return null;
    }

    // The lit string carries one letter per lit room, so count the letters
    // and light that many rooms of each role, in the unit's own order.
    function litCounts(lit) {
        var c = {}, s = String(lit || ""), i;
        for (i = 0; i < s.length; i++) c[s[i]] = (c[s[i]] || 0) + 1;
        return c;
    }

    function paintRow(row, i) {
        var un = cfg.units[i];
        var s = statusFor(i, un.name);
        var state = s && STATES.indexOf(s.state) >= 0 ? s.state : "";
        row.glyph.className = "st " + state;
        row.word.textContent = state || "no reading";
        var c = litCounts(s && s.lit);
        var names = [];
        row.discs.forEach(function (d, k) {
            var role = un.rooms[k] ? un.rooms[k].role : "other";
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
    // the list it settles on is sent. A room may now be a whole street, so
    // only its first eight lamps are asked for: that is what the device
    // blinks. Everything but a 400 is silent: this is a convenience, and a
    // device that is not answering already says so in the header.
    function identify(lamps) {
        // A new value, valid or not, cancels whatever blink was pending,
        // so a list the field no longer shows can never fire late.
        if (identTimer) clearTimeout(identTimer);
        identTimer = null;
        if (!lamps || !lamps.length) return;
        identLamps = lamps.slice(0, MAX_IDENT);
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

    function roomDiscs(box, un, row) {
        box.innerHTML = "";
        row.discs = [];
        if (!un.rooms.length) {
            box.appendChild(el("span", { class: "rnone" }, "no rooms"));
            return;
        }
        un.rooms.forEach(function (r) {
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

    function renderRooms(box, un, i, row) {
        box.innerHTML = "";
        box.appendChild(el("div", { class: "rhead" },
            el("span", null, "Lamps"), el("span", null, "Room")));
        if (!un.rooms.length) {
            box.appendChild(el("p", { class: "hint" },
                "No rooms yet. Add one and give it a lamp."));
        }
        un.rooms.forEach(function (r, k) {
            // Every source of a new list lands here: the two step buttons
            // and typing. write is false while typing, so the field is not
            // rewritten under the caret.
            function commit(text, write) {
                readRoom(r, text);
                if (write) field.value = text;
                syncStep();
                roomDiscs(row.roomBox, un, row);
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
                blink.disabled = !r.lamps.length;
            }
            syncStep();
            var role = sel(ROOMS, r.role, roomLabel, function (ev) {
                r.role = ev.target.value;
                roomDiscs(row.roomBox, un, row);
                paintRow(row, i);
                touch();
            });
            role.setAttribute("aria-label", "Room " + (k + 1) + " kind");
            var del = el("button", {
                type: "button", class: "rdel", "aria-label": "Remove room " + (k + 1),
                onclick: function () {
                    un.rooms.splice(k, 1);
                    renderRooms(box, un, i, row);
                    roomDiscs(row.roomBox, un, row);
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
        row.add.disabled = un.rooms.length >= MAX_ROOMS;
    }

    // --- the editor -------------------------------------------------------

    function unitForm(un, i, row) {
        var name = el("input", {
            type: "text", maxlength: 23, value: un.name || "",
            oninput: function (ev) {
                un.name = ev.target.value;
                row.name.textContent = un.name || "Unnamed unit";
                recheck();
                touch();
            }
        });
        var building = el("input", {
            type: "text", maxlength: 23, value: un.building || "",
            placeholder: NO_BUILDING,
            oninput: function (ev) { un.building = ev.target.value; touch(); }
        });

        // What the unit does is the model's business, so the label is the
        // way to the tab that owns it.
        var names = modelNames();
        if (un.model && names.indexOf(un.model) < 0) {
            names = [un.model].concat(names);
        }
        var kindNote = el("small", { class: "ukind" }, modelKind(un.model));
        var model = sel(names, un.model, function (n) { return n; },
            function (ev) {
                un.model = ev.target.value;
                row.model.textContent = un.model || "No model";
                kindNote.textContent = modelKind(un.model);
                touch();
            });
        model.setAttribute("aria-label", "Model");
        var mfield = el("div", { class: "field mfield" },
            el("span", null,
                el("a", {
                    href: "#models", class: "mlink",
                    onclick: function (ev) {
                        ev.preventDefault();
                        App.go("models");
                    }
                }, "Model"),
                kindNote),
            model);

        var roomBox = el("div", { class: "rtable" });
        // One line per open unit, not per room: the stepper is the same
        // control twelve times over.
        var rhint = el("p", { class: "hint rhint" },
            "A room can hold up to eight runs of lamps, written as 4, 6-8 or "
            + "0-15, and they all switch together. The lamp button blinks the "
            + "first eight of them on the layout, and so does the stepper on a "
            + "room with one lamp.");
        row.add = el("button", {
            type: "button", class: "btn secondary rooms-add",
            onclick: function () {
                if (un.rooms.length >= MAX_ROOMS) return;
                un.rooms.push({ lamps: [0], ranges: 1, text: "0", err: "",
                    role: "living" });
                renderRooms(roomBox, un, i, row);
                roomDiscs(row.roomBox, un, row);
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
        renderRooms(roomBox, un, i, row);

        row.err = el("p", { class: "err", role: "alert", hidden: true });
        row.warn = el("p", { class: "warn", hidden: true });
        setNote(row.err, errs[i]);
        setNote(row.warn, warns[i]);

        var del = el("button", { type: "button", class: "btn danger" },
            "Delete unit");
        del.addEventListener("click", function () {
            if (!del.armed) {
                del.armed = true;
                del.textContent = "Tap again to delete";
                return;
            }
            cfg.units.splice(i, 1);
            expanded = -1;
            touch();
            render();
            focusRow(i - 1);
        });

        return el("div", { class: "uform" },
            el("div", { class: "ug" }, fld("Name", name), fld("Building", building)),
            el("div", { class: "ug" }, mfield),
            el("h3", null, "Rooms"),
            row.err, row.warn, roomBox, rhint, row.add,
            del);
    }

    // --- list -------------------------------------------------------------

    function focusRow(i) {
        var all = u.list.querySelectorAll(".urow");
        if (all[i]) all[i].focus();
        else if (u.add) u.add.focus();
    }

    function unitRow(un, i) {
        var open = expanded === i;
        var row = { discs: [] };
        row.name = el("span", { class: "uname" }, un.name || "Unnamed unit");
        row.model = el("span", { class: "umodel" }, un.model || "No model");
        row.glyph = el("span", { class: "st", "aria-hidden": "true" });
        row.word = el("span", { class: "uword" }, "no reading");
        row.bad = el("span", { class: "ubad", hidden: !errs[i] },
            el("span", { class: "ring" }, "!"), "check rooms");
        row.sr = el("span", { class: "sr" }, "");
        row.roomBox = el("span", { class: "urooms", "aria-hidden": "true" });
        roomDiscs(row.roomBox, un, row);

        var btn = el("button", {
            type: "button", class: "urow",
            "aria-expanded": open ? "true" : "false",
            onclick: function () {
                expanded = open ? -1 : i;
                render();
                focusRow(i);
            }
        },
            el("span", { class: "utop" }, row.name,
                el("span", { class: "ustate" }, row.glyph, row.word)),
            el("span", { class: "umeta" }, row.model, row.bad, row.roomBox, row.sr));

        var node = el("div", { class: "unit" }, btn);
        if (open) node.appendChild(unitForm(un, i, row));
        rows[i] = row;
        return node;
    }

    function buildingOf(un) {
        return (un.building || "").trim() || NO_BUILDING;
    }

    // Units sit under the building they are in, in the order the buildings
    // first appear, except that a unit with no building comes first: a
    // street is not in a house.
    function render() {
        rows = [];
        u.list.innerHTML = "";
        if (!cfg) {
            u.list.appendChild(el("p", { class: "hint" }, "Reading the scene."));
            return;
        }
        check();
        if (!cfg.units.length) {
            u.list.appendChild(el("p", { class: "hint" },
                "No units yet. Add one and put it on a model."));
        }
        var order = [], boxes = {};
        function box(b) {
            if (!boxes[b]) {
                boxes[b] = el("div", { class: "bunits" });
                order.push(b);
            }
            return boxes[b];
        }
        cfg.units.forEach(function (un) {
            if (buildingOf(un) === NO_BUILDING) box(NO_BUILDING);
        });
        cfg.units.forEach(function (un) { box(buildingOf(un)); });
        order.forEach(function (b) {
            u.list.appendChild(el("div", { class: "building" },
                el("h3", { class: "bname" }, b), boxes[b]));
        });
        cfg.units.forEach(function (un, i) {
            boxes[buildingOf(un)].appendChild(unitRow(un, i));
        });
        paint();
        updateSave();
    }

    function addUnit() {
        if (!cfg) return;
        if (cfg.units.length >= MAX_UNITS) {
            App.toast("The controller holds at most " + MAX_UNITS + " units",
                "error");
            return;
        }
        var names = modelNames();
        if (!names.length) {
            App.toast("Make a model first: a unit runs one", "error");
            App.go("models");
            return;
        }
        cfg.units.push({
            name: "Unit " + (cfg.units.length + 1), building: "",
            model: names[0], rooms: []
        });
        expanded = cfg.units.length - 1;
        touch();
        render();
        var nameIn = u.list.querySelector(".uform input[type=text]");
        if (nameIn) {
            nameIn.focus();
            nameIn.select();
        } else {
            focusRow(expanded);
        }
    }

    // The scene as the device wants it. The room rows carry the text the
    // field holds and what the parser made of it, which are this page's
    // business, so the rooms are written out again as lamps and a role,
    // and the model as the name it is referred to by.
    function payload() {
        var out = {}, key;
        for (key in cfg) {
            if (has(cfg, key)) out[key] = cfg[key];
        }
        out.units = cfg.units.map(function (un) {
            var g = {}, k;
            for (k in un) {
                if (has(un, k)) g[k] = un[k];
            }
            g.model = un.model || "";
            g.rooms = un.rooms.map(function (r) {
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
            App.toast("Houses saved", "ok");
        }, function (e) {
            updateSave();
            App.toast(e.message, "error");
        });
    }

    // --- view -------------------------------------------------------------

    function mount(root) {
        cfg = null;
        rows = [];
        errs = [];
        warns = [];
        expanded = -1;
        changed = false;
        status = null;
        u = {};

        u.list = el("div", { class: "ulist" });
        u.add = el("button", {
            type: "button", class: "btn secondary", onclick: addUnit
        }, "Add unit");
        u.save = el("button", {
            type: "button", class: "btn primary", disabled: true, onclick: save
        }, "Save houses");
        u.mark = el("span", { class: "mark" }, "");

        root.appendChild(el("div", { class: "view-houses" },
            el("section", { class: "card" },
                el("div", { class: "chead" },
                    el("h2", null, "Units"), u.add),
                u.list,
                el("div", { class: "actions" }, u.save, u.mark))));
        render();

        // Every handler checks its token, so a response that lands after
        // the view is gone (or after a remount) is dropped instead of
        // writing into the wrong DOM.
        var token = ++mounted;
        App.api("GET", "/api/scene/config").then(function (c) {
            if (token !== mounted || !c) return;
            cfg = c;
            if (!Array.isArray(cfg.models)) cfg.models = [];
            if (!Array.isArray(cfg.units)) cfg.units = [];
            cfg.units.forEach(function (un) {
                if (!Array.isArray(un.rooms)) un.rooms = [];
                un.rooms.forEach(fromJson);
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
