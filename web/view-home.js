// view-home.js - Home view: the town clock, the twelve I2C buses as tiles,
//                the units strip, the network line and the scene switch.
//                mount() builds the DOM once, poll() only writes text and
//                classes.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el;
    var NUM_BUSES = 12;
    var instance = null; // the current mount, so a late reply can be dropped
    var writing = false; // a clock PUT is in flight, leave the switch alone
    var r = null;        // element references, built by mount()

    // A household is awake, asleep, out or away; a unit on opening hours
    // is open or closed, or lit or dark.
    var STATES = ["awake", "asleep", "out", "away",
                  "open", "closed", "lit", "dark"];
    var ROOM_WORDS = {
        l: "living room", k: "kitchen", b: "bedroom", t: "bathroom",
        h: "hall", f: "shop front", r: "back room", s: "sign",
        o: "other room"
    };

    var NET_MODES = {
        online: "online", connecting: "connecting",
        portal: "own access point", nomodule: "no WiFi module"
    };

    // One tile: the bus number, a mark per fitted device kind, and a fault
    // line that stays hidden until a device on the bus reports one.
    function busTile(index) {
        var sx = el("span", { class: "mark" }, "SX");
        var al = el("span", { class: "mark" }, "AL×0");
        var none = el("span", { class: "mark none" }, "empty");
        var fault = el("span", { class: "bus-fault" }, el("span", { class: "glyph" }, "!"), "fault");
        var node = el("div", { class: "bus empty" },
            el("span", { class: "bus-n tnum" }, "bus " + index),
            el("span", { class: "bus-marks" }, sx, al, none),
            fault);
        return { node: node, sx: sx, al: al, none: none, fault: fault };
    }

    function netRow(label) {
        var name = el("span", { class: "net-label" }, label);
        var value = el("span", { class: "net-value" }, "");
        return { node: el("div", { class: "row" }, name, value), name: name, value: value };
    }

    function mount(root) {
        var tiles = [];
        var grid = el("div", { class: "buses" });
        for (var i = 0; i < NUM_BUSES; i++) {
            tiles.push(busTile(i));
            grid.appendChild(tiles[i].node);
        }

        var time = el("time", { class: "town-time" }, "--:--");
        var lamp = el("span", { class: "town-lamp off" });
        var caption = el("p", { class: "clock-caption" }, "waiting for the controller");
        var count = el("p", { class: "clock-count tnum" }, "0 of 0 lamps lit");
        // Only there when the status carries "loadError": the device found
        // a stored scene at boot and could not read it, so the town is
        // empty and the file has been left alone.
        var loadError = el("p", { class: "scene-error", hidden: true },
            el("span", { class: "glyph" }, "!"), el("span", null, ""));
        var devices = el("span", { class: "head-note tnum" }, "");
        var mode = netRow("mode");
        var name = netRow("network");
        var ip = netRow("address");
        ip.node.className = "row last";
        var sys = netRow("system");
        sys.node.className = "row last";
        sys.node.hidden = true;
        var warn = el("p", { class: "net-warn", hidden: true },
            el("span", { class: "glyph" }, "!"), "time not set");
        var input = el("input", {
            type: "checkbox",
            onchange: function () { sendEnabled(input.checked); }
        });

        var unitList = el("div", { class: "unit-list" });
        var unitBox = el("section", { class: "card", hidden: true },
            el("h2", null, "Units"), unitList);

        root.appendChild(el("div", { class: "view-home" },
            el("section", { class: "card clock" },
                el("div", { class: "clock-face" }, time, lamp), caption, count,
                loadError),
            el("section", { class: "card" },
                el("h2", null, "Buses", devices), grid),
            unitBox,
            el("div", { class: "cols" },
                el("section", { class: "card net" },
                    el("h2", null, "Network"), mode.node, name.node, ip.node,
                    sys.node, warn),
                el("section", { class: "card" },
                    el("label", { class: "row toggle" },
                        el("span", null, "Scene running"),
                        el("span", { class: "switch" },
                            input,
                            el("span", { class: "track" }),
                            el("span", { class: "thumb" }))),
                    el("p", { class: "hint" },
                        "Turn this off to hold the town at the time it shows now.")))));

        r = {
            time: time, lamp: lamp, caption: caption, count: count, tiles: tiles,
            devices: devices, loadError: loadError,
            loadErrorText: loadError.lastChild,
            unitBox: unitBox, unitList: unitList,
            unitKey: null, chips: [],
            mode: mode.value, name: name.value,
            nameLabel: name.name, ip: ip.value, sys: sys.value,
            sysRow: sys.node, warn: warn, input: input
        };
        instance = {};
        showSystem();
    }

    // /api/system/status is read once, on mount, and not in poll(): the
    // firmware version never changes while the page is open and the uptime
    // is a readout, not a live counter. On failure the row stays hidden and
    // there is no toast, because nothing else on the page depends on it.
    function showSystem() {
        var mine = instance;
        App.api("GET", "/api/system/status").then(function (s) {
            if (mine !== instance || !r || !s) return;
            var bits = [];
            if (s.firmware) bits.push("firmware " + s.firmware);
            if (typeof s.uptime === "number") bits.push("up " + App.fmtUptime(s.uptime));
            if (bits.length === 0) return;
            setText(r.sys, bits.join(" · "));
            r.sysRow.hidden = false;
        }, function () {
            // no system line, and no toast for it
        });
    }

    function unmount() {
        instance = null;
        writing = false;
        r = null;
    }

    // Send the new state, and put the switch back if the device refuses. A
    // reply for a switch that is gone (view left, or remounted) is dropped.
    function sendEnabled(enabled) {
        var mine = instance;
        writing = true;
        App.api("PUT", "/api/scene/clock", { enabled: !!enabled }).then(function () {
            if (mine === instance) writing = false;
        }, function (err) {
            if (mine !== instance) return;
            writing = false;
            r.input.checked = !enabled;
            App.toast(err.message || "could not change the scene", "error");
        });
    }

    function setText(node, text) {
        if (node.textContent !== text) node.textContent = text;
    }

    function setClass(node, name) {
        if (node.className !== name) node.className = name;
    }

    function showScene(s) {
        setText(r.time, s.time || "--:--");
        setClass(r.lamp, "town-lamp " + (s.lit > 0 ? "on" : "off"));
        setText(r.caption, (s.mode || "unknown") +
            " · dusk " + (s.dusk || "--:--") + " · dawn " + (s.dawn || "--:--"));
        setText(r.count, (s.lit || 0) + " of " + (s.lamps || 0) + " lamps lit");
        if (!writing && r.input.checked !== !!s.enabled) r.input.checked = !!s.enabled;
        var why = s.loadError || "";
        r.loadError.hidden = !why;
        if (why) setText(r.loadErrorText, "stored scene not loaded: " + why);
    }

    // One chip per unit: the name, a shape for the state and the rooms
    // that are lit. The state word is there for a screen reader, since the
    // shape alone would say it to nobody else.
    function buildChips(units) {
        r.unitList.innerHTML = "";
        r.chips = units.map(function (un) {
            var glyph = el("span", { class: "st", "aria-hidden": "true" });
            var word = el("span", { class: "sr" }, "");
            var lit = el("span", { class: "unit-lit", "aria-hidden": "true" });
            r.unitList.appendChild(el("div", { class: "unit-chip" },
                el("span", { class: "unit-name" }, un.name || "Unnamed unit"),
                glyph, word, lit));
            return { glyph: glyph, word: word, lit: lit, letters: null };
        });
    }

    function showUnits(units) {
        var list = units || [];
        r.unitBox.hidden = list.length === 0;
        if (!list.length) return;
        var key = list.map(function (un) { return un.name; }).join("\u0000");
        if (r.unitKey !== key) {
            r.unitKey = key;
            buildChips(list);
        }
        list.forEach(function (un, i) {
            var c = r.chips[i];
            var state = STATES.indexOf(un.state) >= 0 ? un.state : "";
            setClass(c.glyph, "st " + state);
            var letters = String(un.lit || "");
            var words = [];
            for (var w = 0; w < letters.length; w++) {
                words.push(ROOM_WORDS[letters.charAt(w)] || "room");
            }
            // The glyph and the discs are shapes, so the state and the lit
            // rooms go into one line a reader can say.
            setText(c.word, (state || "no reading")
                + (words.length ? ", lit: " + words.join(", ") : ", no room lit"));
            if (c.letters === letters) return;
            c.letters = letters;
            c.lit.innerHTML = "";
            if (!letters) {
                c.lit.appendChild(el("span", { class: "unit-dark" }, "dark"));
                return;
            }
            for (var k = 0; k < letters.length; k++) {
                c.lit.appendChild(el("span", { class: "rd on" }, letters.charAt(k)));
            }
        });
    }

    function showLamps(l) {
        var buses = l.buses || [];
        var n = l.devices || 0;
        setText(r.devices, n + (n === 1 ? " device" : " devices"));
        for (var i = 0; i < NUM_BUSES; i++) {
            var t = r.tiles[i];
            var b = buses[i] || {};
            var sx = b.sx1503 || "none";
            var al = b.al5887 || [];
            var alFault = false;
            for (var d = 0; d < al.length; d++) {
                if (al[d] === "fault") alFault = true;
            }
            var fitted = sx !== "none" || al.length > 0;
            var faulted = sx === "fault" || alFault;

            t.sx.hidden = sx === "none";
            setClass(t.sx, "mark" + (sx === "fault" ? " fault" : ""));
            t.al.hidden = al.length === 0;
            setText(t.al, "AL×" + al.length);
            setClass(t.al, "mark" + (alFault ? " fault" : ""));
            t.none.hidden = fitted;
            t.fault.hidden = !faulted;
            setClass(t.node, "bus " + (fitted ? "fitted" : "empty") + (faulted ? " has-fault" : ""));
        }
    }

    function showNet(n) {
        var portal = n.mode === "portal";
        setText(r.mode, NET_MODES[n.mode] || n.mode || "unknown");
        setText(r.nameLabel, portal ? "access point" : "network");
        setText(r.name, (portal ? n.apSsid : n.ssid) || "none");
        setText(r.ip, (portal ? n.apIp : n.ip) || "none");
        r.warn.hidden = n.timeValid !== false;
    }

    // A failed poll needs no toast: App.api raises the "no contact" badge.
    function poll() {
        var mine = instance;
        if (!mine) return;
        Promise.all([
            App.api("GET", "/api/scene/status"),
            App.api("GET", "/api/lamps/status"),
            App.api("GET", "/api/net/status")
        ]).then(function (res) {
            if (mine !== instance || !r) return;
            if (res[0]) {
                showScene(res[0]);
                showUnits(res[0].units);
            }
            if (res[1]) showLamps(res[1]);
            if (res[2]) showNet(res[2]);
        }, function () {
            // the offline badge already says it
        });
    }

    App.register("home", { title: "Home", mount: mount, unmount: unmount, poll: poll });
})();
