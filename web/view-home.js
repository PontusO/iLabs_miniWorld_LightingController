// view-home.js - Home view: the town clock, the twelve I2C buses as tiles,
//                the network line and the scene switch. mount() builds the
//                DOM once, poll() only writes text and classes.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el;
    var NUM_BUSES = 12;
    var instance = null; // the current mount, so a late reply can be dropped
    var writing = false; // a clock PUT is in flight, leave the switch alone
    var r = null;        // element references, built by mount()

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

        root.appendChild(el("div", { class: "view-home" },
            el("section", { class: "card clock" },
                el("div", { class: "clock-face" }, time, lamp), caption, count),
            el("section", { class: "card" },
                el("h2", null, "Buses", devices), grid),
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
            devices: devices, mode: mode.value, name: name.value,
            nameLabel: name.name, ip: ip.value, sys: sys.value,
            sysRow: sys.node, warn: warn, input: input
        };
        instance = {};
        showSystem();
    }

    // 4512 -> "1h 15m". Days and hours, hours and minutes, minutes, or
    // seconds for the first minute after a reboot.
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
            if (typeof s.uptime === "number") bits.push("up " + fmtUptime(s.uptime));
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
            if (res[0]) showScene(res[0]);
            if (res[1]) showLamps(res[1]);
            if (res[2]) showNet(res[2]);
        }, function () {
            // the offline badge already says it
        });
    }

    App.register("home", { title: "Home", mount: mount, unmount: unmount, poll: poll });
})();
