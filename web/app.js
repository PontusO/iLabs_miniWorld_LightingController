// app.js - the shell: hash router, API helper, toasts and small DOM/format
//          helpers shared by every view. View files (view-home.js and so
//          on) are written against the App object below and register
//          themselves with App.register().
//
// Invector Embedded Systems AB

(function () {
    "use strict";

    var views = {};
    var current = null; // { name, view }
    var pollTimer = null;

    // Builds a DOM element. attrs is a plain object of attributes, or null;
    // "class" sets className, "html" sets innerHTML, an "on<Event>"
    // function attaches an event listener, true/false/null attributes are
    // set/omitted. Children are strings, numbers, nodes or arrays of those.
    function el(tag, attrs) {
        var node = document.createElement(tag);
        if (attrs) {
            for (var key in attrs) {
                if (!Object.prototype.hasOwnProperty.call(attrs, key)) continue;
                var value = attrs[key];
                if (key === "class") {
                    node.className = value;
                } else if (key === "html") {
                    node.innerHTML = value;
                } else if (key.indexOf("on") === 0 && typeof value === "function") {
                    node.addEventListener(key.slice(2).toLowerCase(), value);
                } else if (value === true) {
                    node.setAttribute(key, "");
                } else if (value === false || value === null || value === undefined) {
                    // omitted
                } else {
                    node.setAttribute(key, value);
                }
            }
        }
        for (var i = 2; i < arguments.length; i++) {
            appendChild(node, arguments[i]);
        }
        return node;
    }

    function appendChild(node, child) {
        if (child === null || child === undefined) return;
        if (Array.isArray(child)) {
            for (var i = 0; i < child.length; i++) appendChild(node, child[i]);
            return;
        }
        if (typeof child === "string" || typeof child === "number") {
            node.appendChild(document.createTextNode(String(child)));
        } else {
            node.appendChild(child);
        }
    }

    // 1182 -> "19:42". Wraps to 0..1439 first so a window past midnight
    // (minutes >= 1440, or negative) still shows a plain clock face.
    function fmtTime(minutes) {
        var m = Math.round(minutes) % 1440;
        if (m < 0) m += 1440;
        var h = Math.floor(m / 60);
        var mm = m % 60;
        return pad2(h) + ":" + pad2(mm);
    }

    function pad2(n) {
        return (n < 10 ? "0" : "") + n;
    }

    // "0-15, 20" -> [0,1,...,15,20]. Accepts a comma-separated list of
    // single numbers or "a-b" ranges (a <= b). Throws on anything else,
    // and when max is given, on any number >= max.
    function parseRanges(text, max) {
        var result = [];
        var seen = {};
        var parts = String(text || "").split(",");
        var any = false;
        for (var i = 0; i < parts.length; i++) {
            var part = parts[i].trim();
            if (part === "") continue;
            any = true;
            var m = /^(\d+)(?:-(\d+))?$/.exec(part);
            if (!m) {
                throw new Error("bad range: " + part);
            }
            var a = parseInt(m[1], 10);
            var b = m[2] !== undefined ? parseInt(m[2], 10) : a;
            if (b < a) {
                throw new Error("bad range: " + part);
            }
            for (var n = a; n <= b; n++) {
                if (max !== undefined && max !== null && n >= max) {
                    throw new Error("lamp " + n + " is out of range");
                }
                if (!seen[n]) {
                    seen[n] = true;
                    result.push(n);
                }
            }
        }
        if (!any) {
            throw new Error("empty range list");
        }
        result.sort(function (x, y) { return x - y; });
        return result;
    }

    // [0,1,2,20] -> "0-2, 20". The inverse of parseRanges, for round-
    // tripping a lamp list into the editable text field.
    function rangesText(list) {
        if (!list || list.length === 0) return "";
        var seen = {};
        var unique = [];
        for (var u = 0; u < list.length; u++) {
            if (!seen[list[u]]) {
                seen[list[u]] = true;
                unique.push(list[u]);
            }
        }
        if (unique.length === 0) return "";
        var sorted = unique.sort(function (a, b) { return a - b; });
        var out = [];
        var start = sorted[0];
        var prev = sorted[0];
        for (var i = 1; i <= sorted.length; i++) {
            var cur = sorted[i];
            if (cur === prev + 1) {
                prev = cur;
                continue;
            }
            out.push(start === prev ? String(start) : start + "-" + prev);
            if (i < sorted.length) {
                start = cur;
                prev = cur;
            }
        }
        return out.join(", ");
    }

    // A lamp status disc: <span class="lamp on|off">.
    function lampDisc(lit) {
        return el("span", { class: "lamp " + (lit ? "on" : "off") });
    }

    function setOffline(offline) {
        var badge = document.getElementById("offline");
        if (badge) badge.hidden = !offline;
    }

    // Shows a message in the toast stack for 3 s. kind is "info", "ok" or
    // "error" and picks the toast's colour.
    function toast(message, kind) {
        var container = document.getElementById("toasts");
        if (!container) return;
        var node = el("div", { class: "toast " + (kind || "info") }, message);
        container.appendChild(node);
        setTimeout(function () {
            if (node.parentNode) node.parentNode.removeChild(node);
        }, 3000);
    }

    // Calls the JSON API. Resolves with the parsed body on any 2xx
    // response. On 401 it reloads the page so the browser's own basic-
    // auth prompt appears, and rejects. On another non-2xx it parses
    // {"error": "..."} from the body and rejects with that message. On a
    // network failure (device unreachable) it shows the "no contact"
    // badge and rejects. Any response at all, success or error status,
    // clears the badge, because it proves the device is there.
    function api(method, path, body) {
        return fetch(path, {
            method: method,
            headers: { "Content-Type": "application/json" },
            body: body ? JSON.stringify(body) : undefined
        }).then(function (res) {
            setOffline(false);
            if (res.status === 401) {
                location.reload();
                throw new Error("unauthorized");
            }
            if (!res.ok) {
                return res.json().catch(function () {
                    return {};
                }).then(function (data) {
                    throw new Error((data && data.error) || ("request failed: " + res.status));
                });
            }
            if (res.status === 204) return null;
            return res.json().catch(function () {
                return null;
            });
        }, function (err) {
            setOffline(true);
            throw err;
        });
    }

    // Registers a view under a hash name. view is { title, mount(root),
    // unmount(), poll() }; mount/unmount/poll are all optional except
    // mount.
    function register(name, view) {
        views[name] = view;
    }

    // Navigates to a view, updating location.hash.
    function go(name) {
        if (location.hash === "#" + name) {
            route();
        } else {
            location.hash = "#" + name;
        }
    }

    function markActiveNav(name) {
        var links = document.querySelectorAll("#nav a");
        for (var i = 0; i < links.length; i++) {
            var a = links[i];
            if (a.getAttribute("data-view") === name) {
                a.classList.add("active");
            } else {
                a.classList.remove("active");
            }
        }
    }

    function route() {
        var name = (location.hash || "#home").replace(/^#/, "") || "home";

        if (pollTimer) {
            clearInterval(pollTimer);
            pollTimer = null;
        }
        if (current && current.view && typeof current.view.unmount === "function") {
            current.view.unmount();
        }
        current = null;

        var root = document.getElementById("view");
        if (root) root.innerHTML = "";

        markActiveNav(name);

        var view = views[name];
        if (!view) {
            if (root) root.textContent = "no view registered for " + name;
            return;
        }

        current = { name: name, view: view };
        view.mount(root);

        if (typeof view.poll === "function") {
            view.poll();
            pollTimer = setInterval(function () {
                view.poll();
            }, 2000);
        }
    }

    // The skip control moves focus into the view without going through the
    // hash, which the router owns: a link to #view would look like a fifth
    // view name to route().
    function wireSkip() {
        var skip = document.getElementById("skip");
        var main = document.getElementById("view");
        if (!skip || !main) return;
        skip.addEventListener("click", function () {
            main.focus();
            main.scrollIntoView();
        });
    }

    wireSkip();
    window.addEventListener("hashchange", route);
    window.addEventListener("load", route);

    window.App = {
        register: register,
        go: go,
        toast: toast,
        api: api,
        el: el,
        fmtTime: fmtTime,
        parseRanges: parseRanges,
        rangesText: rangesText,
        lampDisc: lampDisc
    };
})();
