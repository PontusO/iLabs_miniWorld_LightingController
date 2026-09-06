// view-wifi.js - WiFi view, which is also the captive portal landing page.
//                Top to bottom: where the controller stands, the network
//                picker and connect form, then settings and forget below.
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var el = App.el, api = App.api;

    // Timezone presets, Sweden first. "custom" reveals a POSIX TZ field.
    var TZS = [
        ["CET-1CEST,M3.5.0,M10.5.0/3", "Sweden and central Europe"],
        ["UTC0", "UTC, no summer time"],
        ["WET0WEST,M3.5.0/1,M10.5.0", "Portugal and western Europe"],
        ["EET-2EEST,M3.5.0/3,M10.5.0/4", "Finland and eastern Europe"],
        ["GMT0BST,M3.5.0/1,M10.5.0", "United Kingdom and Ireland"]
    ];
    var SVG = '<svg viewBox="0 0 24 24" width="W" height="W" fill="none" stroke="currentColor" stroke-width="S" stroke-linecap="round" stroke-linejoin="round">';
    var LOCK = SVG.replace(/W/g, 13).replace("S", 2.2) +
        '<rect x="4" y="10" width="16" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg>';
    var TICK = SVG.replace(/W/g, 16).replace("S", 3) + '<path d="M4 13l5 5L20 6"/></svg>';
    var CROSS = SVG.replace(/W/g, 16).replace("S", 3) + '<path d="M6 6l12 12M18 6L6 18"/></svg>';

    var s = {}; // view state
    var r = {}; // dom references, so poll() updates in place

    function fail(e) {
        App.toast((e && e.message) || "request failed", "error");
    }

    function inp(id, attrs) {
        attrs = attrs || {};
        attrs.id = "wifi-" + id;
        attrs.autocapitalize = "none";
        attrs.spellcheck = "false";
        return el("input", attrs);
    }

    function field(label, input, hint) {
        return el("div", { class: "field" }, el("label", { for: input.id }, label),
            input, hint ? el("span", { class: "hint" }, hint) : null);
    }

    // State card ------------------------------------------------------

    // The network the result line talks about: what we last tried, else
    // what the device reports, else what it has stored.
    function attempted() {
        return s.tried || (s.status && s.status.ssid) ||
            (s.cfg && s.cfg.sta && s.cfg.sta.ssid) || "that network";
    }

    function renderState() {
        var st = s.status, mark = "off", lines = [];
        if (!st) return;
        var host = st.hostname || "miniworld";
        if (st.mode === "online") {
            r.title.textContent = "Connected to " + (st.ssid || "a network");
            if (st.ip) lines.push(el("span", { class: "tnum" }, st.ip));
            lines.push("http://" + host + ".local/");
            mark = "on";
        } else if (st.mode === "connecting") {
            r.title.textContent = "Joining " + attempted();
            lines.push("Keep this page open.");
            mark = "wait";
        } else if (st.mode === "nomodule") {
            r.title.textContent = "No WiFi module fitted";
            lines.push("The town runs on its own clock.");
        } else {
            r.title.textContent = "Not on a WiFi network";
            if (st.apActive) lines.push("Access point " + (st.apSsid || "miniWorld"));
            lines.push("Choose your network below");
        }
        r.mark.className = "state-mark " + mark;
        r.sub.innerHTML = "";
        for (var i = 0; i < lines.length; i++) {
            r.sub.appendChild(el("span", { class: "sub-line" }, lines[i]));
        }
        renderResult(st, host);
        updateGo();
    }

    function renderResult(st, host) {
        var res = st.connectResult, glyph, body;
        r.result.hidden = res !== "connecting" && res !== "ok" && res !== "failed";
        if (r.result.hidden) return;
        if (res === "connecting") {
            glyph = el("span", { class: "spin" });
            body = el("span", {}, "Connecting to " + attempted(),
                el("span", { class: "dots" }, el("i"), el("i"), el("i")));
        } else if (res === "ok") {
            glyph = el("span", { class: "glyph", html: TICK });
            body = el("span", {}, "Connected. Address ",
                el("span", { class: "tnum" }, st.ip || "unknown"),
                ". Open http://" + host + ".local/ from your home network. " +
                "This access point switches off shortly.");
        } else {
            glyph = el("span", { class: "glyph", html: CROSS });
            body = el("span", {}, "Could not join " + attempted() +
                ". Check the password and try again.");
        }
        glyph.setAttribute("aria-hidden", "true");
        r.result.className = "state-result " + res;
        r.result.innerHTML = "";
        r.result.appendChild(glyph);
        r.result.appendChild(body);
    }

    // Network list ----------------------------------------------------

    function netRow(n) {
        var level = n.rssi > -55 ? 4 : n.rssi > -65 ? 3 : n.rssi > -75 ? 2 : 1;
        var bars = el("span", { class: "bars", "aria-hidden": "true" });
        for (var i = 1; i <= 4; i++) bars.appendChild(el("i", { class: i <= level ? "on" : "" }));
        var picked = s.sel === n.ssid;
        return el("li", {}, el("button", {
            class: "net-row" + (picked ? " picked" : ""), type: "button",
            "aria-pressed": picked ? "true" : "false",
            "aria-label": n.ssid + ", signal " + level + " of 4, " +
                (n.secure ? "password needed" : "open"),
            onclick: function () { pick(n.ssid); }
        }, el("span", { class: "net-lamp", "aria-hidden": "true" }),
            el("span", { class: "net-name" }, n.ssid),
            n.secure ? el("span", { class: "lock", "aria-hidden": "true", html: LOCK }) : null,
            bars));
    }

    function renderList() {
        r.list.innerHTML = "";
        var nets = s.nets;
        r.empty.hidden = !!(nets && nets.length);
        if (r.empty.hidden) {
            for (var i = 0; i < nets.length; i++) r.list.appendChild(netRow(nets[i]));
        } else {
            r.empty.textContent = nets ?
                "No networks found. Move the controller closer and scan again." :
                "Tap Scan to look for networks nearby.";
        }
    }

    function pick(ssid) {
        s.sel = ssid;
        r.pick.value = ssid;
        r.other.value = "";
        renderList();
        updateGo();
    }

    function target() {
        return r.other.value.trim() || s.sel;
    }

    function updateGo() {
        var busy = !!(s.status && s.status.connectResult === "connecting");
        r.go.disabled = !target() || busy;
        r.go.textContent = busy ? "Connecting" : "Connect";
    }

    function scan() {
        if (s.scanning) return;
        s.scanning = true;
        r.scan.disabled = true;
        r.scan.textContent = "Scanning";
        api("GET", "/api/net/scan").then(function (d) {
            s.nets = (d && d.networks) || [];
            renderList();
        }).catch(fail).then(function () {
            s.scanning = false;
            r.scan.disabled = false;
            r.scan.textContent = "Scan";
        });
    }

    function connect() {
        var ssid = target();
        if (!ssid) return;
        s.tried = ssid;
        r.go.disabled = true;
        api("POST", "/api/net/connect", { ssid: ssid, pass: r.pass.value })
            .then(function (st) {
                if (st) s.status = st;
                renderState();
            }).catch(function (e) { fail(e); updateGo(); });
    }

    function toggleShow() {
        var hidden = r.pass.type === "password";
        r.pass.type = hidden ? "text" : "password";
        r.show.textContent = hidden ? "hide" : "show";
        r.show.setAttribute("aria-pressed", hidden ? "true" : "false");
        r.show.setAttribute("aria-label", (hidden ? "hide" : "show") + " the password");
    }

    // Settings --------------------------------------------------------

    function renderSettings() {
        var c = s.cfg || {}, i, known = false;
        s.base = { hostname: c.hostname || "", ntp: c.ntp || "", tz: c.tz || "" };
        for (i = 0; i < TZS.length; i++) if (TZS[i][0] === s.base.tz) known = true;

        // The hostname is not a name the browser should remember or fill.
        r.host = inp("host", { type: "text", value: s.base.hostname,
            autocomplete: "off" });
        r.ntp = inp("ntp", { type: "text", value: s.base.ntp });
        // new-password: this field sets the device password, it never asks
        // for one the browser has stored.
        r.dpass = inp("dpass", { type: "password", autocomplete: "new-password",
            placeholder: c.hasPassword ?
            "Set a new password" : "None, anyone on the network can edit" });
        r.tzc = inp("tzc", { type: "text", value: known ? "" : s.base.tz,
            placeholder: "POSIX TZ string" });
        r.tz = el("select", { id: "wifi-tz", onchange: onTz });
        for (i = 0; i < TZS.length; i++) {
            r.tz.appendChild(el("option", { value: TZS[i][0] }, TZS[i][1]));
        }
        r.tz.appendChild(el("option", { value: "custom" }, "Custom"));
        r.tz.value = known ? s.base.tz : "custom";
        r.tzField = field("Custom timezone", r.tzc, null);
        r.tzField.hidden = known;
        r.clear = c.hasPassword ? el("input", { id: "wifi-clear", type: "checkbox",
            onchange: function () { r.dpass.disabled = r.clear.checked; } }) : null;

        r.setBody.innerHTML = "";
        r.setBody.appendChild(field("Device name", r.host,
            "Reached at http://" + (s.base.hostname || "miniworld") + ".local/"));
        r.setBody.appendChild(field("Device password", r.dpass,
            "Asked for when this page opens. Empty keeps the current one."));
        if (r.clear) {
            r.setBody.appendChild(el("label", { class: "check", for: "wifi-clear" },
                r.clear, el("span", {}, "Clear the password on save")));
        }
        r.setBody.appendChild(field("Time server", r.ntp, null));
        r.setBody.appendChild(field("Timezone", r.tz, null));
        r.setBody.appendChild(r.tzField);
        r.setBody.appendChild(el("button",
            { class: "btn primary wide", type: "button", onclick: save }, "Save settings"));
    }

    function onTz() {
        r.tzField.hidden = r.tz.value !== "custom";
        if (!r.tzField.hidden) r.tzc.focus();
    }

    // Sends only what the owner changed, so a save never rewrites a
    // field it did not touch.
    function save() {
        var body = {}, n = 0, k;
        var host = r.host.value.trim().toLowerCase();
        var ntp = r.ntp.value.trim();
        var custom = r.tz.value === "custom";
        var tz = custom ? r.tzc.value.trim() : r.tz.value;
        if (host !== s.base.hostname) body.hostname = host;
        if (ntp !== s.base.ntp) body.ntp = ntp;
        // An empty custom timezone is "I have not typed one yet", not
        // "clear it": leave tz out so the stored value survives.
        if (tz !== s.base.tz && !(custom && tz === "")) body.tz = tz;
        if (r.clear && r.clear.checked) body.password = "";
        else if (r.dpass.value !== "") body.password = r.dpass.value;
        for (k in body) if (Object.prototype.hasOwnProperty.call(body, k)) n++;
        if (!n) {
            App.toast("Nothing changed", "info");
            return;
        }
        api("PUT", "/api/net/config", body).then(function (cfg) {
            if (cfg) s.cfg = cfg;
            renderSettings();
            App.toast("Settings saved", "ok");
        }).catch(fail);
    }

    // Forget ----------------------------------------------------------

    function renderForget() {
        r.fbody.innerHTML = "";
        if (!s.confirm) {
            r.fbody.appendChild(el("button", { class: "btn secondary wide", type: "button",
                onclick: function () { s.confirm = true; renderForget(); } }, "Forget WiFi"));
            return;
        }
        r.fbody.appendChild(el("p", { class: "ask" },
            "Forget the stored network and start the access point?"));
        r.fbody.appendChild(el("div", { class: "btn-row" },
            el("button", { class: "btn danger", type: "button", onclick: doForget }, "Yes, forget"),
            el("button", { class: "btn secondary", type: "button",
                onclick: function () { s.confirm = false; renderForget(); } }, "Keep it")));
    }

    function doForget() {
        api("POST", "/api/net/forget").then(function (st) {
            if (st) s.status = st;
            s.confirm = false;
            s.tried = "";
            renderForget();
            renderState();
            App.toast("Network forgotten", "ok");
            return api("GET", "/api/net/config").then(function (cfg) {
                if (cfg) { s.cfg = cfg; renderSettings(); }
            });
        }).catch(fail);
    }

    // Mount -----------------------------------------------------------

    function build() {
        r.mark = el("span", { class: "state-mark off", "aria-hidden": "true" });
        r.title = el("h2", { class: "state-title" }, "Reading the radio");
        r.sub = el("p", { class: "state-sub" });
        r.result = el("p", { class: "state-result", role: "status",
            "aria-live": "polite", hidden: true });
        r.scan = el("button", { class: "btn secondary", type: "button", onclick: scan }, "Scan");
        r.list = el("ul", { class: "net-list" });
        r.empty = el("p", { class: "net-empty" });
        r.pick = inp("pick", { type: "text", readonly: true, placeholder: "Tap a network above" });
        r.other = inp("other", { type: "text", placeholder: "Network name", oninput: function () {
            if (r.other.value.trim() && s.sel) { s.sel = ""; r.pick.value = ""; renderList(); }
            updateGo();
        } });
        // The passphrase of someone else's network: nothing for the browser
        // to save against this device's origin.
        r.pass = inp("pass", { type: "password", autocomplete: "off",
            placeholder: "Network password" });
        r.show = el("button", { class: "btn ghost", type: "button", "aria-pressed": "false",
            "aria-label": "show the password", onclick: toggleShow }, "show");
        r.go = el("button", { class: "btn primary wide", type: "button",
            onclick: connect, disabled: true }, "Connect");
        r.setBody = el("div", {});
        r.fbody = el("div", {});

        return el("section", { class: "view-wifi" },
            el("div", { class: "card state" },
                el("div", { class: "state-head" }, r.mark, r.title), r.sub, r.result),
            el("div", { class: "card" },
                el("div", { class: "head" }, el("h2", {}, "Choose a network"), r.scan),
                r.list, r.empty,
                field("Network", r.pick, null),
                el("details", { class: "other" }, el("summary", {}, "Other network"),
                    field("Name of a hidden network", r.other, null)),
                el("div", { class: "field" }, el("label", { for: "wifi-pass" }, "Password"),
                    el("div", { class: "pass-row" }, r.pass, r.show)),
                r.go),
            el("div", { class: "card" },
                el("div", { class: "head" }, el("h2", {}, "Device settings")), r.setBody),
            el("div", { class: "card" },
                el("div", { class: "head" }, el("h2", {}, "Stored network")), r.fbody));
    }

    function mount(root) {
        s = { status: null, cfg: null, nets: null, scanning: false, sel: "",
            tried: "", confirm: false, base: {} };
        r = {};
        root.appendChild(build());
        renderList();
        renderForget();
        api("GET", "/api/net/status").then(function (st) {
            s.status = st;
            renderState();
            if (st && st.mode === "portal") scan();
        }).catch(function () { /* the header badge already says it */ });
        api("GET", "/api/net/config").then(function (cfg) {
            s.cfg = cfg;
            renderSettings();
        }).catch(fail);
    }

    function poll() {
        api("GET", "/api/net/status").then(function (st) {
            s.status = st;
            renderState();
        }).catch(function () { /* keep the last known state on screen */ });
    }

    function unmount() {
        s = {};
        r = {};
    }

    App.register("wifi", { title: "WiFi", mount: mount, unmount: unmount, poll: poll });
})();
