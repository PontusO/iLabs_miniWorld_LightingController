// view-models.js - Models view: the ways a building in the town can
//                  behave. A model owns no lamps: it is a household's day,
//                  or a shop's opening hours, and the Houses view hangs
//                  units on it. Each row says what the model does in one
//                  line and how many units use it; opening a row gives
//                  the editor for its kind.
//                  Saving writes the whole scene config, units included,
//                  since a renamed model has to be renamed in them too.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el;

    var MAX_MODELS = 16;   // SCENE_MAX_MODELS
    var KINDS = ["rhythm", "hours"];
    var KIND_LABELS = { rhythm: "rhythm", hours: "hours" };
    // The nine built-in models: key in /api/scene/presets, the title a new
    // model gets, and the kind to fall back on without the presets.
    var TEMPLATES = [
        ["family", "Family", "rhythm"],
        ["elderly", "Elderly couple", "rhythm"],
        ["nightowl", "Night owl", "rhythm"],
        ["away", "Away", "rhythm"],
        ["home", "Home", "hours"],
        ["shop", "Shop", "hours"],
        ["pub", "Pub", "hours"],
        ["street", "Street light", "hours"],
        ["allnight", "All night", "hours"]
    ];
    var ANCHORS = ["dusk", "dawn", "clock"];
    // The four rhythm ranges, in the order a day runs.
    var RANGES = [["wake", "Wake"], ["leave", "Leave"],
                  ["home", "Home"], ["bed", "Bed"]];
    // The two halves of a model, so one can be written out and the other
    // filled from a template when the kind changes.
    var RHYTHM_KEYS = ["weekend", "wake", "leave", "home", "bed",
                       "outPercent", "tvPercent"];
    var HOURS_KEYS = ["onAnchor", "on", "offAnchor", "off", "litPercent",
                      "flickerPercent", "morning", "individual"];
    // A half with nothing to copy from, for a device that did not answer
    // with its templates: the Family day and the Home hours.
    var RHYTHM_BASE = {
        weekend: true, wake: [390, 435], leave: [450, 495], home: [960, 1050],
        bed: [1350, 1410], outPercent: 14, tvPercent: 70
    };
    var HOURS_BASE = {
        onAnchor: "dusk", on: [0, 240], offAnchor: "clock", off: [1320, 1470],
        litPercent: 85, flickerPercent: 12, morning: true, individual: true
    };
    var RNUMS = [["outPercent", "Out %", 100], ["tvPercent", "TV %", 100]];
    var HNUMS = [["litPercent", "Lit %", 100], ["flickerPercent", "Flicker %", 100]];
    var CNUMS = [["level", "Level", 255], ["fadeMs", "Fade ms", 60000]];
    // The four window ends: key, label. A window end may sit before
    // midnight or past it, so the pair is wider than a day.
    var WINS = [["on0", "On from"], ["on1", "On to"],
                ["off0", "Off from"], ["off1", "Off to"]];
    var LEVELS = [0, 1, 2, 3];
    var DAY_WORDS = ["None", "Low", "Normal", "High"];
    var NIGHT_WORDS = ["None", "Rare", "Normal", "Often"];
    // Short lights a day per lamp: the engine's day rate at w = 1
    // (0, 0.15, 0.35, 0.70 per hour) times eight, rounded.
    var DAY_COUNT = [0, 1, 3, 6];
    // Wake-ups a night: 0, 0.4, 0.8 and 1.6 expected, said in words.
    var NIGHT_TEXT = ["no wake-ups", "a wake-up every 2 nights",
                      "a wake-up every night", "1 or 2 a night"];

    var cfg = null;      // the scene as loaded, edited in place
    var presets = null;  // presets.models, the nine templates
    var refs = [];       // the model name the units carry, one per model
    var errs = [];       // name error per model, "" when good
    var open = -1;       // the expanded row, -1 for none
    var changed = false;
    var mounted = 0;     // bumped by mount and unmount, drops late replies
    var u = {};

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
    function cap(s) { return s.charAt(0).toUpperCase() + s.slice(1); }
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

    function swtch(input) {
        return el("span", { class: "switch" }, input,
            el("span", { class: "track" }), el("span", { class: "thumb" }));
    }

    function kind(m) { return m.kind === "hours" ? "hours" : "rhythm"; }
    function kindLabel(k) { return KIND_LABELS[k] || k; }

    // "on0" reads m.on[0], every other key reads m[key].
    function getf(m, key) {
        var p = /^(on|off)([01])$/.exec(key);
        return p ? m[p[1]][+p[2]] : m[key];
    }

    function setf(m, key, v) {
        var p = /^(on|off)([01])$/.exec(key);
        if (p) m[p[1]][+p[2]] = v;
        else m[key] = v;
    }

    // --- what a model does, in one line -----------------------------------

    // The household read off the midpoints of its ranges, so it says what
    // the model will roughly do and not what was typed. A rhythm with no
    // wake and no bed is the away household.
    function rhythmText(m) {
        var w = mid(m.wake), l = mid(m.leave), h = mid(m.home), b = mid(m.bed);
        if (w < 0 && b < 0) {
            return "No rhythm. One lamp on a timer, 19:00 to 22:30.";
        }
        var parts = [];
        if (w >= 0) parts.push("wakes around " + App.fmtTime(w));
        if (l >= 0) parts.push("leaves " + App.fmtTime(l));
        if (h >= 0) parts.push("home " + App.fmtTime(h));
        if (b >= 0) parts.push("bed " + App.fmtTime(b));
        var out;
        if (!m.outPercent) {
            out = "never out";
        } else {
            var n = Math.round(100 / m.outPercent);
            out = n <= 1 ? "out most evenings" : "out one evening in " + n;
        }
        if (!parts.length) return "No rhythm set.";
        return parts.join(", ") + "; " + out;
    }

    function signed(n) { return (n < 0 ? "-" : "+") + Math.abs(n | 0); }

    // A clock window is two clock faces, a dusk or dawn window two signed
    // offsets from that event: "22:00..00:30", "dusk -10..+5".
    function windowText(anchor, win) {
        var a = win && win.length ? win[0] : 0, b = win && win.length > 1 ? win[1] : 0;
        if (anchor === "clock") return App.fmtTime(a) + ".." + App.fmtTime(b);
        return (anchor || "dusk") + " " + signed(a) + ".." + signed(b);
    }

    function hoursText(m) {
        return windowText(m.onAnchor, m.on) + " to " + windowText(m.offAnchor, m.off);
    }

    function summary(m) {
        return kind(m) === "hours" ? hoursText(m) : rhythmText(m);
    }

    // The life on top of the base state, in plain words, so the rates stay
    // out of the GUI.
    function activityText(day, night) {
        var n = DAY_COUNT[day];
        return (day ? "about " + n + " short light" + (n === 1 ? "" : "s")
                        + " a day per lamp"
                    : "No daytime activity")
            + ", " + NIGHT_TEXT[night];
    }

    // --- models and the units on them -------------------------------------

    // Counted on the name the units carry, not on the name in the field: a
    // rename only reaches the units when Save builds the payload.
    function usedBy(i) {
        var n = 0;
        (cfg.units || []).forEach(function (un) {
            if (un.model === refs[i]) n++;
        });
        return n;
    }

    function unitsText(n) { return n + (n === 1 ? " unit" : " units"); }

    // A half a model has never carried is filled from its template, so
    // switching kind in the editor never opens an empty form.
    function fillHalf(m, keys, key, base) {
        var src = (presets && presets[key]) || base;
        keys.forEach(function (k) {
            if (m[k] !== undefined && m[k] !== null) return;
            var v = src[k];
            m[k] = Array.isArray(v) ? v.slice() : v;
        });
    }

    function complete(m) {
        if (kind(m) === "hours") fillHalf(m, HOURS_KEYS, "home", HOURS_BASE);
        else fillHalf(m, RHYTHM_KEYS, "family", RHYTHM_BASE);
        if (typeof m.level !== "number") m.level = 200;
        if (typeof m.fadeMs !== "number") m.fadeMs = 300;
        m.dayActivity = lvl(m.dayActivity);
        m.nightActivity = lvl(m.nightActivity);
    }

    // "Shop", then "Shop 2", "Shop 3": names are unique, compared without
    // case, so a second convenience store is one tap away.
    function freeName(title) {
        var taken = {};
        cfg.models.forEach(function (m) {
            taken[String(m.name || "").toLowerCase()] = true;
        });
        if (!taken[title.toLowerCase()]) return title;
        for (var n = 2; n < 100; n++) {
            if (!taken[(title + " " + n).toLowerCase()]) return title + " " + n;
        }
        return title;
    }

    // --- validation -------------------------------------------------------

    function check() {
        errs = [];
        if (!cfg) return;
        var seen = {};
        cfg.models.forEach(function (m) {
            var key = String(m.name || "").trim().toLowerCase();
            seen[key] = (seen[key] || 0) + 1;
        });
        cfg.models.forEach(function (m) {
            var name = String(m.name || "").trim();
            if (!name) errs.push("A model needs a name.");
            else if (seen[name.toLowerCase()] > 1) {
                errs.push("Another model is called \"" + name
                    + "\" too. Two models cannot share a name.");
            } else errs.push("");
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

    function touch() {
        changed = true;
        updateSave();
    }

    // --- the editor -------------------------------------------------------

    function rhythmFields(m, f, sync) {
        // A range is a pair: emptying either end means "never", and filling
        // one end of an empty pair fills both, so a half-set range cannot
        // reach the device. A "to" earlier than its "from" is read as the
        // next day, which is how a bedtime of 00:45 is stored as 1485.
        function commitTime(key, idx, input) {
            var v = parseHM(input.value);
            if (v === null) {
                input.value = fmtHM(m[key][idx]);
                App.toast("Write a time as HH:MM, or leave it empty for never",
                    "error");
                return;
            }
            if (v < 0) {
                m[key][0] = -1;
                m[key][1] = -1;
            } else {
                m[key][idx] = v;
                if (m[key][1 - idx] < 0) m[key][1 - idx] = v;
                if (m[key][1] < m[key][0]) {
                    if (idx === 1) m[key][1] = v + 1440;
                    else m[key][1] = m[key][0];
                }
            }
            f[key][0].value = fmtHM(m[key][0]);
            f[key][1].value = fmtHM(m[key][1]);
            sync();
            touch();
        }

        function timeIn(key, idx, label) {
            var input = el("input", {
                type: "text", inputmode: "numeric", placeholder: "never",
                maxlength: 5, class: "tin", value: fmtHM(m[key][idx]),
                "aria-label": label + " " + (idx ? "to" : "from")
            });
            input.addEventListener("change", function () {
                commitTime(key, idx, input);
            });
            return input;
        }

        var rng = el("div", { class: "rhythm" });
        RANGES.forEach(function (rg) {
            f[rg[0]] = [timeIn(rg[0], 0, rg[1]), timeIn(rg[0], 1, rg[1])];
            rng.appendChild(el("div", { class: "rng" },
                el("span", { class: "rlab" }, rg[1]),
                fld("from", f[rg[0]][0]), fld("to", f[rg[0]][1])));
        });

        var weekend = el("input", {
            type: "checkbox", checked: !!m.weekend,
            onchange: function (ev) { m.weekend = ev.target.checked; touch(); }
        });
        var grid = el("div", { class: "mg" });
        RNUMS.forEach(function (n) {
            grid.appendChild(fld(n[1], el("input", {
                type: "number", min: 0, max: n[2], value: m[n[0]],
                oninput: function (ev) {
                    m[n[0]] = num(ev.target.value, 0, n[2], m[n[0]]);
                    sync();
                    touch();
                }
            })));
        });
        grid.appendChild(el("label", { class: "field wk" },
            el("span", null, "Weekend rhythm"), swtch(weekend)));
        return el("div", null, rng, grid);
    }

    function hoursFields(m, sync) {
        var grid = el("div", { class: "mg" });

        function anchor(key, label) {
            grid.appendChild(fld(label, sel(ANCHORS, m[key], cap, function (ev) {
                m[key] = ev.target.value;
                sync();
                touch();
            })));
        }

        // A window end may be an offset from dusk or a clock minute past
        // midnight, so the field is as wide as both: the bounds the
        // firmware's clamp() uses for the hours windows, -720 to 2879.
        function winField(w) {
            grid.appendChild(fld(w[1], el("input", {
                type: "number", min: -720, max: 2879, value: getf(m, w[0]),
                oninput: function (ev) {
                    var v = parseInt(ev.target.value, 10);
                    if (isNaN(v)) return;
                    setf(m, w[0], Math.max(-720, Math.min(2879, v)));
                    sync();
                    touch();
                }
            })));
        }

        anchor("onAnchor", "On anchor");
        WINS.slice(0, 2).forEach(winField);
        anchor("offAnchor", "Off anchor");
        WINS.slice(2).forEach(winField);

        HNUMS.forEach(function (n) {
            grid.appendChild(fld(n[1], el("input", {
                type: "number", min: 0, max: n[2], value: m[n[0]],
                oninput: function (ev) {
                    m[n[0]] = num(ev.target.value, 0, n[2], m[n[0]]);
                    touch();
                }
            })));
        });

        var morning = el("input", {
            type: "checkbox", checked: !!m.morning,
            onchange: function (ev) { m.morning = ev.target.checked; touch(); }
        });
        grid.appendChild(el("label", { class: "field wk" },
            el("span", null, "Morning"), swtch(morning)));

        var individual = el("input", {
            type: "checkbox", checked: !!m.individual,
            onchange: function (ev) { m.individual = ev.target.checked; touch(); }
        });
        return el("div", null, grid,
            el("label", { class: "field wk own" },
                el("span", null, "Each lamp keeps its own moment"),
                swtch(individual)),
            el("p", { class: "hint" },
                "On, every lamp picks its own moment inside the window, as a "
                + "block of windows does. Off, a room switches as one, as a "
                + "street does."));
    }

    function form(m, i, row) {
        var f = {};
        var line = el("p", { class: "aline" }, "");

        function sync() { row.sum.textContent = summary(m); }

        var err = el("p", { class: "err", role: "alert", hidden: !errs[i] }, errs[i]);
        var name = el("input", {
            type: "text", maxlength: 23, value: m.name || "",
            oninput: function (ev) {
                m.name = ev.target.value;
                row.name.textContent = m.name || "Unnamed model";
                check();
                err.textContent = errs[i];
                err.hidden = !errs[i];
                updateSave();
                touch();
            }
        });
        var kindSel = sel(KINDS, kind(m), kindLabel, function (ev) {
            m.kind = ev.target.value;
            complete(m);
            touch();
            render();
            focusRow(i);
        });

        function actSel(key, words) {
            return sel(LEVELS, lvl(m[key]),
                function (v) { return words[v]; },
                function (ev) {
                    m[key] = lvl(parseInt(ev.target.value, 10));
                    line.textContent = activityText(m.dayActivity, m.nightActivity);
                    touch();
                });
        }
        f.dayActivity = actSel("dayActivity", DAY_WORDS);
        f.nightActivity = actSel("nightActivity", NIGHT_WORDS);
        line.textContent = activityText(lvl(m.dayActivity), lvl(m.nightActivity));

        var common = el("div", { class: "mg" });
        CNUMS.forEach(function (n) {
            common.appendChild(fld(n[1], el("input", {
                type: "number", min: 0, max: n[2], value: m[n[0]],
                oninput: function (ev) {
                    m[n[0]] = num(ev.target.value, 0, n[2], m[n[0]]);
                    touch();
                }
            })));
        });

        var del = el("button", { type: "button", class: "btn danger" },
            "Delete model");
        del.addEventListener("click", function () {
            var n = usedBy(i);
            if (n) {
                App.toast("Model in use by " + unitsText(n), "error");
                return;
            }
            if (!del.armed) {
                del.armed = true;
                del.textContent = "Tap again to delete";
                return;
            }
            cfg.models.splice(i, 1);
            refs.splice(i, 1);
            open = -1;
            check();
            touch();
            render();
            focusRow(i - 1);
        });

        return el("div", { class: "mform" },
            el("div", { class: "mg" }, fld("Name", name), fld("Kind", kindSel)),
            err,
            kind(m) === "hours" ? hoursFields(m, sync) : rhythmFields(m, f, sync),
            el("h3", null, "Every lamp"), common,
            el("h3", null, "Activity"),
            el("div", { class: "mg" },
                fld("Daytime", f.dayActivity),
                fld("Night wake-ups", f.nightActivity)),
            line,
            del);
    }

    // --- the list ---------------------------------------------------------

    function focusRow(i) {
        var all = u.list.querySelectorAll(".mrow");
        if (all[i]) all[i].focus();
        else if (u.add) u.add.focus();
    }

    function modelRow(m, i) {
        var isOpen = open === i;
        var row = {};
        row.name = el("span", { class: "mname" }, m.name || "Unnamed model");
        row.kind = el("span", { class: "mkind" }, kindLabel(kind(m)));
        row.sum = el("span", { class: "msum" }, summary(m));
        row.count = el("span", { class: "mcount tnum" }, unitsText(usedBy(i)));

        var node = el("div", { class: "model" },
            el("button", {
                type: "button", class: "mrow",
                "aria-expanded": isOpen ? "true" : "false",
                onclick: function () {
                    open = isOpen ? -1 : i;
                    render();
                    focusRow(i);
                }
            }, row.name, row.kind, row.sum, row.count));
        if (isOpen) node.appendChild(form(m, i, row));
        return node;
    }

    function render() {
        u.list.innerHTML = "";
        if (!cfg) {
            u.list.appendChild(el("p", { class: "hint" }, "Reading the scene."));
            return;
        }
        check();
        if (!cfg.models.length) {
            u.list.appendChild(el("p", { class: "hint" },
                "No models yet. Add one and the Houses view can hang units on it."));
        }
        cfg.models.forEach(function (m, i) {
            u.list.appendChild(modelRow(m, i));
        });
        updateSave();
    }

    // --- new model --------------------------------------------------------

    function addModel(t) {
        if (!cfg) return;
        u.pick.hidden = true;
        if (cfg.models.length >= MAX_MODELS) {
            App.toast("The controller holds at most " + MAX_MODELS + " models",
                "error");
            return;
        }
        var src = presets && presets[t[0]];
        var m = { kind: t[2] }, k;
        if (src) {
            for (k in src) {
                if (has(src, k)) {
                    m[k] = Array.isArray(src[k]) ? src[k].slice() : src[k];
                }
            }
        }
        m.name = freeName(t[1]);
        complete(m);
        cfg.models.push(m);
        refs.push(m.name);      // a new model has no units yet
        open = cfg.models.length - 1;
        check();
        touch();
        render();
        var nameIn = u.list.querySelector(".mform input[type=text]");
        if (nameIn) {
            nameIn.focus();
            nameIn.select();
        } else {
            focusRow(open);
        }
    }

    function chooser() {
        var list = el("div", { class: "picks" });
        TEMPLATES.forEach(function (t) {
            list.appendChild(el("button", {
                type: "button", class: "pickb",
                onclick: function () { addModel(t); }
            }, t[1]));
        });
        return el("div", { class: "pick", hidden: true },
            el("p", { class: "hint" }, "Start from:"), list);
    }

    // --- saving -----------------------------------------------------------

    // A rename reaches the units here and nowhere else: every unit on the
    // name a model used to have gets the name it has now, in the payload
    // that carries the rename itself.
    function renameUnits() {
        var map = {}, any = false;
        cfg.models.forEach(function (m, i) {
            if (refs[i] !== m.name) {
                map[refs[i]] = m.name;
                any = true;
            }
        });
        if (!any) return;
        (cfg.units || []).forEach(function (un) {
            if (has(map, un.model)) un.model = map[un.model];
        });
    }

    // A model is written with the common fields and only the half its kind
    // uses, which is the shape the device reads.
    function payload() {
        var out = {}, k;
        for (k in cfg) {
            if (has(cfg, k)) out[k] = cfg[k];
        }
        out.models = cfg.models.map(function (m) {
            var g = {
                name: m.name, kind: kind(m), level: m.level, fadeMs: m.fadeMs,
                dayActivity: lvl(m.dayActivity), nightActivity: lvl(m.nightActivity)
            };
            (kind(m) === "hours" ? HOURS_KEYS : RHYTHM_KEYS).forEach(function (key) {
                g[key] = Array.isArray(m[key]) ? m[key].slice() : m[key];
            });
            return g;
        });
        return out;
    }

    function save() {
        if (!cfg || !ok()) return;
        renameUnits();
        refs = cfg.models.map(function (m) { return m.name; });
        u.save.disabled = true;
        App.api("PUT", "/api/scene/config", payload()).then(function () {
            changed = false;
            render();
            App.toast("Models saved", "ok");
        }, function (e) {
            updateSave();
            App.toast(e.message, "error");
        });
    }

    // --- view -------------------------------------------------------------

    // The Houses view sets App.pendingModel when its Model link is tapped,
    // so the tab opens on the model that unit runs. It is read once and
    // cleared, and a name no model carries just leaves every row closed.
    function openHandover() {
        var want = App.pendingModel;
        App.pendingModel = null;
        if (!want) return;
        cfg.models.forEach(function (m, i) {
            if (m.name === want) open = i;
        });
    }

    function scrollToOpen() {
        if (open < 0) return;
        var node = u.list.querySelectorAll(".mrow")[open];
        if (node) node.scrollIntoView();
    }

    function mount(root) {
        cfg = null;
        presets = null;
        refs = [];
        errs = [];
        open = -1;
        changed = false;
        u = {};

        u.list = el("div", { class: "mlist" });
        u.pick = chooser();
        u.add = el("button", {
            type: "button", class: "btn secondary",
            onclick: function () { u.pick.hidden = !u.pick.hidden; }
        }, "New model");
        u.save = el("button", {
            type: "button", class: "btn primary", disabled: true, onclick: save
        }, "Save models");
        u.mark = el("span", { class: "mark" }, "");

        root.appendChild(el("div", { class: "view-models" },
            el("section", { class: "card" },
                el("div", { class: "chead" }, el("h2", null, "Models"), u.add),
                u.pick, u.list,
                el("div", { class: "actions" }, u.save, u.mark))));
        render();

        // The templates only seed a new model, so the editor still works
        // without them; the config is the one request that has to land.
        var token = ++mounted;
        Promise.all([
            App.api("GET", "/api/scene/presets").then(null, function () { return null; }),
            App.api("GET", "/api/scene/config")
        ]).then(function (r) {
            if (token !== mounted || !r[1]) return;
            presets = r[0] && r[0].models;
            cfg = r[1];
            if (!Array.isArray(cfg.models)) cfg.models = [];
            if (!Array.isArray(cfg.units)) cfg.units = [];
            cfg.models.forEach(complete);
            refs = cfg.models.map(function (m) { return m.name; });
            openHandover();
            render();
            scrollToOpen();
        }, function (e) {
            if (token !== mounted) return;
            App.toast(e.message, "error");
        });
    }

    function unmount() {
        mounted++;
        cfg = null;
        presets = null;
        refs = [];
        errs = [];
        open = -1;
        changed = false;
        u = {};
    }

    App.register("models", {
        title: "Models",
        mount: mount,
        unmount: unmount
    });
})();
