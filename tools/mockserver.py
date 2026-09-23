#!/usr/bin/env python3
"""
mockserver.py - a standard-library-only stand-in for the RP2040 firmware's
                 HTTP server, so the web GUI in web/ can be built and tested
                 on a laptop before there is any hardware to talk to.

Serves web/index.html at "/" with the same CSS/JS inlining buildweb.py
does (imported from it), and implements every route in spec section 3.6
plus /api/lamps/* and /api/scene/* against in-memory state: a per-bus
lamp configuration, a seeded scene, a simulated clock, and a network
state machine with a fake scan list and a fake connect attempt.

usage: mockserver.py [--portal] [--password PASSWORD] [--port PORT]

Invector Embedded Systems AB
"""

import argparse
import base64
import hashlib
import json
import math
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from buildweb import build_page  # noqa: E402

WEBDIR = Path(__file__).resolve().parent.parent / "web"

LAMPS_MAX_LAMPS = 2048
LAMPS_NUM_BUSES = 12
# SceneEngine::identify() blinks up to this many lamps together.
IDENTIFY_MAX_LAMPS = 8

# The paths phones and desktops fetch to find out whether they are behind a
# captive portal, the same list HttpServer.cpp answers with a redirect.
CAPTIVE_PROBES = (
    "/generate_204",
    "/gen_204",
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/connecttest.txt",
    "/ncsi.txt",
    "/canonical.html",
    "/success.txt",
    "/redirect",
)

# Firmware upload, spec 2026-09-23-firmware-update-design.md sections 3
# and 5. The numbers mirror the device: a 1 MB LittleFS with the three
# JSON documents in it, a 64 kB margin for the filesystem's own needs.
FIRMWARE_MIN_SIZE = 100000
FIRMWARE_MAX_SIZE = 1048576
FIRMWARE_FS_TOTAL = 1048576
FIRMWARE_FS_USED = 36864
FIRMWARE_FS_MARGIN = 65536
FIRMWARE_BANNER = b"miniWorld lighting controller %s (%s)"
FIRMWARE_SCAN_CHUNK = 4096
FIRMWARE_SCAN_OVERLAP = 63

# Models and units: spec section 2. SCENE_MAX_MODELS / SCENE_MAX_UNITS /
# UNIT_MAX_ROOMS / ROOM_MAX_RANGES mirror the firmware #defines; twenty-four
# and not thirty-two, per Scene.h: a unit is about 460 bytes and thirty-two
# put the board at 41 % of its RAM against the 39 % it had before models and
# units. ROOM_NAMES is the Room enum order, used both for
# /api/scene/presets "rooms" and for the order status letters are checked in.
SCENE_NAME_LEN = 24
SCENE_MAX_MODELS = 16
SCENE_MAX_UNITS = 24
UNIT_MAX_ROOMS = 12
ROOM_MAX_RANGES = 8

TEMPLATE_KEYS = ("family", "elderly", "nightowl", "away",
                 "home", "shop", "pub", "street", "allnight")
RHYTHM_TEMPLATE_KEYS = ("family", "elderly", "nightowl", "away")
TEMPLATE_TITLES = {
    "family": "Family", "elderly": "Elderly couple", "nightowl": "Night owl",
    "away": "Away", "home": "Home", "shop": "Shop", "pub": "Pub",
    "street": "Street light", "allnight": "All night",
}

ROOM_NAMES = ("living", "kitchen", "bedroom", "bathroom", "hall",
              "front", "back", "sign", "other")
ROOM_LETTERS = {
    "living": "l", "kitchen": "k", "bedroom": "b", "bathroom": "t", "hall": "h",
    "front": "f", "back": "r", "sign": "s", "other": "o",
}

# The retired group behaviours (RETIRED_NAMES) and the behaviours a group
# could carry (BEHAVIOUR_NAMES), spec section 5 point 4 and Scene.cpp's
# migrateGroup(). "off" is not a template: it is the Home hours values with
# litPercent forced to 0, never lit.
BEHAVIOUR_NAMES = ("off", "street", "home", "shop", "late", "allnight")
BEHAVIOUR_TEMPLATE = {
    "off": "home", "street": "street", "home": "home",
    "shop": "shop", "late": "pub", "allnight": "allnight",
}
RETIRED_NAMES = ("family", "elderly", "nightowl", "away")
HOUSEHOLD_NAMES = ("family", "elderly", "nightowl", "away", "custom")


def _fill_rhythm(key):
    """Scene.cpp's fillRhythm(): the rhythm half plus the brightness and the
    life that go with it. Every template that is not one of the four
    households (every hours template, via Template::Family) takes these
    entry defaults untouched but for the four moments."""
    d = {
        "weekend": True, "outPercent": 14, "tvPercent": 70,
        "level": 210, "fadeMs": 300, "dayActivity": 3, "nightActivity": 1,
        "wake": (390, 435), "leave": (450, 495), "home": (960, 1050), "bed": (1350, 1410),
    }
    if key == "elderly":
        d.update(wake=(345, 390), leave=(570, 630), home=(720, 810), bed=(1275, 1335),
                  outPercent=7, tvPercent=40, level=180, fadeMs=400,
                  dayActivity=2, nightActivity=3)
    elif key == "nightowl":
        d.update(wake=(570, 660), leave=(690, 750), home=(1140, 1260), bed=(1485, 1575),
                  outPercent=28, tvPercent=80, level=200, fadeMs=300,
                  dayActivity=1, nightActivity=1)
    elif key == "away":
        # Nobody lives here this week. No rhythm at all.
        d.update(wake=(-1, -1), leave=(-1, -1), home=(-1, -1), bed=(-1, -1),
                  outPercent=0, tvPercent=0, level=200, fadeMs=0,
                  dayActivity=0, nightActivity=0)
    # else: family, or any hours key, keeps the entry defaults above.
    return d


def _fill_hours(key):
    """Scene.cpp's fillHours(): the hours half plus the brightness and the
    life that go with it. Every template that is not one of the five hours
    templates (every rhythm template, via Template::Home) takes the Home
    case below."""
    d = {
        "litPercent": 100, "flickerPercent": 0, "morning": False, "individual": False,
        "level": 255, "fadeMs": 800, "dayActivity": 0, "nightActivity": 0,
    }
    if key == "shop":
        d.update(onAnchor="clock", on=(510, 540), offAnchor="clock", off=(1080, 1110),
                  level=230, fadeMs=200, dayActivity=1)
    elif key == "pub":
        d.update(onAnchor="dusk", on=(-30, 30), offAnchor="clock", off=(1380, 1560),
                  level=220, dayActivity=1)
    elif key == "street":
        d.update(onAnchor="dusk", on=(-10, 5), offAnchor="dawn", off=(-5, 15), fadeMs=4000)
    elif key == "allnight":
        d.update(onAnchor="dusk", on=(-15, 0), offAnchor="dawn", off=(0, 15))
    else:   # home, or any rhythm key, which carries the Home hours behind it
        d.update(onAnchor="dusk", on=(0, 240), offAnchor="clock", off=(1320, 1470),
                  litPercent=85, flickerPercent=12, morning=True, individual=True,
                  level=200, fadeMs=300, dayActivity=2, nightActivity=1)
    return d


def _build_template(key):
    """ModelConfig::setTemplate(): both halves, the model's own kind last so
    its brightness, fade and life are its own. name is the template's title;
    the caller renames it for anything but a fresh preset."""
    rhythm = key in RHYTHM_TEMPLATE_KEYS
    m = {}
    if rhythm:
        m.update(_fill_hours("home"))
        m.update(_fill_rhythm(key))
    else:
        m.update(_fill_rhythm("family"))
        m.update(_fill_hours(key))
    m["kind"] = "rhythm" if rhythm else "hours"
    m["name"] = TEMPLATE_TITLES[key]
    return m


# The nine templates, spec 2.3: a fresh device's models, and what
# /api/scene/presets and "New model, start from" offer.
MODEL_TEMPLATES = {key: _build_template(key) for key in TEMPLATE_KEYS}


def make_model(key, **overrides):
    """A fresh model from a template, for the mock's own fixtures: the
    template's fields plus whatever the caller overrides directly."""
    m = dict(MODEL_TEMPLATES[key])
    m.update(overrides)
    return m


def clamp_model(m):
    """SceneConfig::clamp()'s per-model loop, the numeric ranges: every field
    keeps its meaning, its range and its clamp regardless of where it came
    from (JSON, a migrated flat, a migrated group)."""
    m["level"] = max(0, min(255, m["level"]))
    m["fadeMs"] = max(0, min(60000, m["fadeMs"]))
    m["dayActivity"] = max(0, min(3, m["dayActivity"]))
    m["nightActivity"] = max(0, min(3, m["nightActivity"]))
    m["outPercent"] = max(0, min(100, m["outPercent"]))
    m["tvPercent"] = max(0, min(100, m["tvPercent"]))
    m["litPercent"] = max(0, min(100, m["litPercent"]))
    m["flickerPercent"] = max(0, min(100, m["flickerPercent"]))
    for key in ("wake", "leave", "home", "bed"):
        a, b = m[key]
        a = max(-1, min(2879, a))
        b = max(-1, min(2879, b))
        if b < a:
            b = a
        m[key] = (a, b)
    for key in ("on", "off"):
        a, b = m[key]
        a = max(-720, min(2879, a))
        b = max(-720, min(2879, b))
        if b < a:
            b = a
        m[key] = (a, b)
    if m["onAnchor"] not in ("dusk", "dawn", "clock"):
        m["onAnchor"] = "clock"
    if m["offAnchor"] not in ("dusk", "dawn", "clock"):
        m["offAnchor"] = "clock"
    return m


class ApiError(Exception):
    """Raised by a handler to send a 4xx/5xx {"error": msg} response."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


# ---------------------------------------------------------------------------
# Lamp hardware state, spec 3.1
# ---------------------------------------------------------------------------

class LampState:
    def __init__(self):
        self.lock = threading.Lock()
        self.busSpeed = 400000
        self.activeLow = False
        self.rgb = False
        self.buses = [{"sx1503": False, "al5887": 0} for _ in range(LAMPS_NUM_BUSES)]
        # Bus 0 sx1503 true, bus 1 al5887 2, per the mock's default fixture.
        self.buses[0]["sx1503"] = True
        self.buses[1]["al5887"] = 2

    def lamp_count(self):
        return sum(16 * (1 if b["sx1503"] else 0) + 36 * b["al5887"] for b in self.buses)

    def device_count(self):
        return sum((1 if b["sx1503"] else 0) + b["al5887"] for b in self.buses)

    def config_json(self):
        with self.lock:
            return {
                "busSpeed": self.busSpeed,
                "activeLow": self.activeLow,
                "rgb": self.rgb,
                "buses": [dict(b) for b in self.buses],
            }

    def status_json(self):
        with self.lock:
            buses_status = []
            faults = []
            for b in self.buses:
                buses_status.append({
                    "sx1503": "ok" if b["sx1503"] else "none",
                    "al5887": ["ok"] * b["al5887"],
                })
                if b["sx1503"]:
                    faults.append(False)
                faults.extend([False] * b["al5887"])
            lamps = self.lamp_count()
            return {
                "lamps": lamps,
                "devices": self.device_count(),
                "intensity": False,
                "resolutionBits": 1 if lamps else 0,
                "buses": buses_status,
                "faults": faults,
            }

    def apply_json(self, data):
        if not isinstance(data, dict):
            raise ApiError(400, "invalid JSON")

        bus_speed = data.get("busSpeed", self.busSpeed)
        if bus_speed not in (100000, 400000, 1000000):
            bus_speed = 400000
        active_low = bool(data.get("activeLow", self.activeLow))
        rgb = bool(data.get("rgb", self.rgb))

        # LampConfig::fromJson merge rule: buses absent (or null) keeps the
        # twelve buses as they are, present as an array replaces all twelve,
        # present as anything else is an error.
        buses_in = data.get("buses")
        new_buses = None
        if buses_in is not None:
            if not isinstance(buses_in, list):
                raise ApiError(400, "buses must be an array")
            if len(buses_in) > LAMPS_NUM_BUSES:
                raise ApiError(400, "buses: at most 12 entries")

            new_buses = [{"sx1503": False, "al5887": 0} for _ in range(LAMPS_NUM_BUSES)]
            for i, entry in enumerate(buses_in):
                if not isinstance(entry, dict):
                    raise ApiError(400, "buses[%d] must be an object" % i)
                sx1503 = bool(entry.get("sx1503", False))
                al5887 = entry.get("al5887", 0)
                if not isinstance(al5887, int) or al5887 < 0 or al5887 > 4:
                    raise ApiError(400, "buses[%d].al5887 must be 0..4" % i)
                new_buses[i] = {"sx1503": sx1503, "al5887": al5887}

        with self.lock:
            self.busSpeed = bus_speed
            self.activeLow = active_low
            self.rgb = rgb
            if new_buses is not None:
                self.buses = new_buses

    def probe(self):
        # The mock has no real hardware to interrogate: probing "finds"
        # exactly what is configured, which is plausible enough for GUI
        # development against the diff/"use what was found" flow.
        return self.config_json()

    def test(self, data):
        if not isinstance(data, dict):
            raise ApiError(400, "invalid JSON")
        if "level" not in data or not isinstance(data["level"], int):
            raise ApiError(400, "level required")
        level = data["level"]
        if level < 0 or level > 255:
            raise ApiError(400, "level must be 0..255")
        if "lamp" in data:
            lamp = data["lamp"]
            if not isinstance(lamp, int) or lamp < 0 or lamp >= self.lamp_count():
                raise ApiError(400, "lamp out of range")
        return {}


# ---------------------------------------------------------------------------
# Scene state, spec Scene.h / SceneWebApi.h
# ---------------------------------------------------------------------------

def _ranges(lamps):
    """Sorted, deduplicated lamp numbers into a list of (from, to) runs."""
    s = sorted(set(lamps))
    out = []
    i = 0
    n = len(s)
    while i < n:
        start = s[i]
        j = i
        while j + 1 < n and s[j + 1] == s[j] + 1:
            j += 1
        out.append((start, s[j]))
        i = j + 1
    return out


def _tokenize_ranges(ranges):
    """A list of (from, to) runs written the way a person would: a single
    number, a pair, or an "a-b" string. Mirrors Scene.cpp's runToJson()."""
    out = []
    for a, b in ranges:
        if b == a:
            out.append(a)
        elif b == a + 1:
            out.append(a)
            out.append(b)
        else:
            out.append("%d-%d" % (a, b))
    return out


def compress_lamps(lamps):
    """Mirrors Scene.cpp's lampsToJson: ints for single lamps, a pair of
    ints for two adjacent, an "a-b" string for a longer run."""
    return _tokenize_ranges(_ranges(lamps))


def expand_lamps(arr):
    """Mirrors Scene.cpp's lampsFromJson."""
    if not isinstance(arr, list):
        raise ApiError(400, "lamps must be an array")
    lamps = set()
    for v in arr:
        if isinstance(v, bool):
            raise ApiError(400, "lamp index out of range")
        if isinstance(v, int):
            if v < 0 or v >= LAMPS_MAX_LAMPS:
                raise ApiError(400, "lamp index out of range")
            lamps.add(v)
        elif isinstance(v, str):
            parts = v.split("-", 1)
            try:
                a = int(parts[0])
                b = int(parts[1]) if len(parts) > 1 else a
            except ValueError:
                raise ApiError(400, "lamp range out of range")
            if a < 0 or b < a or b >= LAMPS_MAX_LAMPS:
                raise ApiError(400, "lamp range out of range")
            lamps.update(range(a, b + 1))
    return lamps


def fmt_time(minutes):
    m = int(round(minutes)) % 1440
    if m < 0:
        m += 1440
    return "%02d:%02d" % (m // 60, m % 60)


# ---------------------------------------------------------------------------
# Models. Scene.h / Scene.cpp: a named way of behaving, no lamps.
# ---------------------------------------------------------------------------

def model_to_json(m):
    """Mirrors SceneConfig::toJson()'s per-model loop: the common fields,
    then only the half that matches the model's kind."""
    o = {
        "name": m["name"], "kind": m["kind"], "level": m["level"], "fadeMs": m["fadeMs"],
        "dayActivity": m["dayActivity"], "nightActivity": m["nightActivity"],
    }
    if m["kind"] == "rhythm":
        o["weekend"] = m["weekend"]
        o["wake"] = list(m["wake"])
        o["leave"] = list(m["leave"])
        o["home"] = list(m["home"])
        o["bed"] = list(m["bed"])
        o["outPercent"] = m["outPercent"]
        o["tvPercent"] = m["tvPercent"]
    else:
        o["onAnchor"] = m["onAnchor"]
        o["on"] = list(m["on"])
        o["offAnchor"] = m["offAnchor"]
        o["off"] = list(m["off"])
        o["litPercent"] = m["litPercent"]
        o["flickerPercent"] = m["flickerPercent"]
        o["morning"] = m["morning"]
        o["individual"] = m["individual"]
    return o


def model_from_json(o, models_so_far):
    """Mirrors SceneConfig::fromJson()'s per-model loop: setTemplate() first
    (absent means the template's value), then the name (required, unique
    case-insensitively against the models already read this save), then
    whatever the JSON overrides. models_so_far is the list being built, not
    yet holding this model, the same self-exclusion uniqueName() gets from
    being called with self == the not-yet-reached index."""
    if not isinstance(o, dict):
        raise ApiError(400, "model must be an object")

    kind_raw = o.get("kind", "")
    kind = kind_raw.lower() if isinstance(kind_raw, str) else ""
    if kind not in ("rhythm", "hours"):
        raise ApiError(400, "unknown model kind")

    m = make_model("family" if kind == "rhythm" else "home")
    m["kind"] = kind

    name = o.get("name", "")
    if not isinstance(name, str) or not name:
        raise ApiError(400, "model name required")
    name = name[:SCENE_NAME_LEN - 1]
    for existing in models_so_far:
        if existing["name"].lower() == name.lower():
            raise ApiError(400, "duplicate model name")
    m["name"] = name

    for key in ("level", "fadeMs", "dayActivity", "nightActivity"):
        v = o.get(key)
        if isinstance(v, int) and not isinstance(v, bool):
            m[key] = v

    if kind == "rhythm":
        if isinstance(o.get("weekend"), bool):
            m["weekend"] = o["weekend"]
        for key in ("wake", "leave", "home", "bed"):
            v = o.get(key)
            if (isinstance(v, list) and len(v) == 2
                    and all(isinstance(x, int) and not isinstance(x, bool) for x in v)):
                m[key] = (v[0], v[1])
        for key in ("outPercent", "tvPercent"):
            v = o.get(key)
            if isinstance(v, int) and not isinstance(v, bool):
                m[key] = v
    else:
        for key in ("onAnchor", "offAnchor"):
            v = o.get(key)
            if isinstance(v, str) and v.lower() in ("dusk", "dawn", "clock"):
                m[key] = v.lower()
        for key in ("on", "off"):
            v = o.get(key)
            if (isinstance(v, list) and len(v) == 2
                    and all(isinstance(x, int) and not isinstance(x, bool) for x in v)):
                m[key] = (v[0], v[1])
        for key in ("litPercent", "flickerPercent"):
            v = o.get(key)
            if isinstance(v, int) and not isinstance(v, bool):
                m[key] = v
        if isinstance(o.get("morning"), bool):
            m["morning"] = o["morning"]
        if isinstance(o.get("individual"), bool):
            m["individual"] = o["individual"]

    return clamp_model(m)


# ---------------------------------------------------------------------------
# Units and their rooms. Scene.h / Scene.cpp: a thing on the layout, a name,
# a building label, a model by name, and rooms of lamp ranges.
# ---------------------------------------------------------------------------

def find_model(models, name):
    """SceneConfig::findModel(): case-insensitive, -1 for an absent, empty
    or wrong-type name (mirrors `o["model"] | ""`, which falls back to ""
    for anything that is not a JSON string), which is how an unresolved
    "model" key ends up at "unknown model" rather than a distinct
    empty-name error."""
    if not isinstance(name, str) or not name:
        return -1
    for i, m in enumerate(models):
        if m["name"].lower() == name.lower():
            return i
    return -1


def ranges_from_json(arr):
    """A room's lamps, spec 2.1: up to eight ranges. expand_lamps() gives
    the set of lamp numbers named by the array (single numbers and "a-b"
    strings); the runs in that set, sorted, are the ranges. This is
    simpler than RoomConfig::add()'s order-preserving merge and gives the
    same result for lamps listed in ascending order, which is how every
    fixture and the GUI write them. Mirrors the "too many ranges" limit of
    roomRangesFromJson()/RoomConfig::add()."""
    lamps = expand_lamps(arr)
    ranges = _ranges(lamps)
    if len(ranges) > ROOM_MAX_RANGES:
        raise ApiError(400, "too many ranges")
    return ranges


def ranges_to_json(ranges):
    """A room's stored ranges back to the JSON list form. Mirrors
    roomLampsToJson(): the ranges are written in the order they are stored,
    so [0, "4-15"] round-trips as [0, "4-15"] and merged runs stay merged."""
    return _tokenize_ranges(ranges)


def room_from_json(o):
    """Mirrors Scene.cpp's roomFromJson(): "lamps" is the list form, "lamp"
    is the legacy single-lamp form, and an absent or wrong-type role
    defaults to "other"; a role that is a string but not one of the nine is
    an error."""
    if not isinstance(o, dict):
        raise ApiError(400, "room must be an object")

    ranges = []
    ls = o.get("lamps")
    if isinstance(ls, list):
        ranges = ranges_from_json(ls)
    else:
        lamp = o.get("lamp")
        if isinstance(lamp, int) and not isinstance(lamp, bool):
            if lamp < 0 or lamp >= LAMPS_MAX_LAMPS:
                raise ApiError(400, "room lamp index out of range")
            ranges = [(lamp, lamp)]

    role_val = o.get("role")
    if isinstance(role_val, str):
        role = role_val.lower()
        if role not in ROOM_NAMES:
            raise ApiError(400, "unknown room role")
    else:
        role = "other"

    return {"ranges": ranges, "role": role}


def make_unit(name, building, model, rooms):
    """Seed helper, the unit equivalent of the old make_flat: rooms is a
    list of (lamps, role) pairs, lamps being one lamp number, a list of
    them (compressed into ranges), or a list of (from, to) tuples given
    directly."""
    room_list = []
    for lamps, role in rooms:
        if isinstance(lamps, int):
            ranges = [(lamps, lamps)]
        elif lamps and isinstance(lamps[0], (list, tuple)):
            ranges = [tuple(r) for r in lamps]
        else:
            ranges = _ranges(lamps)
        room_list.append({"ranges": ranges, "role": role})
    return {"name": name, "building": building, "model": model, "rooms": room_list}


def unit_to_json(u):
    return {
        "name": u["name"],
        "building": u["building"],
        "model": u["model"],
        "rooms": [{"lamps": ranges_to_json(r["ranges"]), "role": r["role"]}
                  for r in u["rooms"]],
    }


def unit_from_json(o, models):
    """Mirrors SceneConfig::fromJson()'s per-unit loop: the model is
    resolved by name against the models the scene has now (whichever the
    caller passes: the ones just read, or the ones already stored), which
    is also what refuses a save naming a model that is not there."""
    if not isinstance(o, dict):
        raise ApiError(400, "unit must be an object")

    mi = find_model(models, o.get("model", ""))
    if mi < 0:
        raise ApiError(400, "unknown model")

    u = {
        "name": str(o.get("name", ""))[:SCENE_NAME_LEN - 1],
        "building": str(o.get("building", ""))[:SCENE_NAME_LEN - 1],
        "model": models[mi]["name"],
        "rooms": [],
    }
    rooms_in = o.get("rooms")
    if not isinstance(rooms_in, list):
        rooms_in = []
    for r in rooms_in:
        if len(u["rooms"]) >= UNIT_MAX_ROOMS:
            raise ApiError(400, "too many rooms in a unit")
        u["rooms"].append(room_from_json(r))
    return u


def clamp_units(units):
    """SceneConfig::clamp()'s dedupUnitRooms(): within one unit a lamp
    belongs to one room, the first room listing it wins, and a room left
    with nothing is not a room. The claim set is per unit, as the firmware's
    is: two units that list the same lamp both keep it in what they store,
    and rebuild() settles who drives it, the first unit in the list.
    Simplified from the firmware's order-preserving range split: a claimed
    lamp is removed by expanding to individual lamps and re-detecting runs,
    and anything past the eight-range cap is dropped rather than refused, as
    clamp() does."""
    for u in units:
        claimed = set()
        kept = []
        for room in u["rooms"]:
            lamps = []
            for a, b in room["ranges"]:
                lamps.extend(range(a, b + 1))
            free = [l for l in lamps if l not in claimed]
            if not free:
                continue
            claimed.update(free)
            kept.append({"ranges": _ranges(free)[:ROOM_MAX_RANGES], "role": room["role"]})
        u["rooms"] = kept
    return units


# ---------------------------------------------------------------------------
# Migration. Spec section 5: a document with "groups" or "flats" and no
# "models" was written before models and units existed. Flats first, so
# their unit indices, and with them their room keys and their household
# draws, stay what they are today; then groups.
# ---------------------------------------------------------------------------

def _same_rhythm(a, b):
    """Did this flat keep the rhythm its household type gives? Everything
    the rhythm half carries, and the brightness and the life that go with
    it; the name is not part of it. Mirrors Scene.cpp's sameRhythm()."""
    keys = ("weekend", "wake", "leave", "home", "bed", "outPercent", "tvPercent",
            "level", "fadeMs", "dayActivity", "nightActivity")
    return all(a[k] == b[k] for k in keys)


def _unique_name(models, name, self_index=None):
    """Mirrors Scene.cpp's uniqueName(): an empty name starts from the Home
    title, and a name already taken (case-insensitively, excluding
    self_index) gets " 2", " 3" and so on, with the base trimmed so the
    suffix fits in SCENE_NAME_LEN."""
    if not name:
        name = TEMPLATE_TITLES["home"]
    base = name
    for n in range(2, 100):
        taken = False
        for i, m in enumerate(models):
            if i == self_index:
                continue
            if m["name"].lower() == name.lower():
                taken = True
                break
        if not taken:
            return name
        suffix = " %d" % n
        keep = SCENE_NAME_LEN - 1 - len(suffix)
        if keep > len(base):
            keep = len(base)
        name = base[:keep] + suffix
    return name


def migrate_flat(o):
    """One old flat: a rhythm model with the household's day, and a unit
    with the flat's name, building and rooms. Returns
    (model, unit, template_key, custom). Mirrors Scene.cpp's migrateFlat()."""
    if not isinstance(o, dict):
        raise ApiError(400, "flat must be an object")

    ftype_raw = o.get("type", "family")
    ftype = ftype_raw.lower() if isinstance(ftype_raw, str) else ""
    if ftype not in HOUSEHOLD_NAMES:
        raise ApiError(400, "unknown household type")
    custom = (ftype == "custom")
    tmpl_key = "family" if custom else ftype
    m = make_model(tmpl_key)

    u = {
        "name": str(o.get("name", ""))[:SCENE_NAME_LEN - 1],
        "building": str(o.get("building", ""))[:SCENE_NAME_LEN - 1],
        "model": None,
        "rooms": [],
    }
    if isinstance(o.get("weekend"), bool):
        m["weekend"] = o["weekend"]

    rooms_in = o.get("rooms")
    if not isinstance(rooms_in, list):
        rooms_in = []
    for r in rooms_in:
        if len(u["rooms"]) >= UNIT_MAX_ROOMS:
            raise ApiError(400, "too many rooms in a unit")
        u["rooms"].append(room_from_json(r))

    for key in ("wake", "leave", "home", "bed"):
        v = o.get(key)
        if (isinstance(v, list) and len(v) == 2
                and all(isinstance(x, int) and not isinstance(x, bool) for x in v)):
            m[key] = (v[0], v[1])
    if isinstance(o.get("outPercent"), int) and not isinstance(o.get("outPercent"), bool):
        m["outPercent"] = o["outPercent"]
    if isinstance(o.get("tvPercent"), int) and not isinstance(o.get("tvPercent"), bool):
        m["tvPercent"] = o["tvPercent"]
    if isinstance(o.get("level"), int) and not isinstance(o.get("level"), bool):
        m["level"] = o["level"]
    if isinstance(o.get("fadeMs"), int) and not isinstance(o.get("fadeMs"), bool):
        m["fadeMs"] = o["fadeMs"]
    for key in ("dayActivity", "nightActivity"):
        v = o.get(key)
        if isinstance(v, int) and not isinstance(v, bool):
            m[key] = v

    return clamp_model(m), u, tmpl_key, custom


def migrate_group(o):
    """One old group: an hours model with the behaviour's habits, and, when
    it had lamps, the rooms (role "other", eight ranges each, more rooms as
    needed) a unit of the same name would need. Returns
    (model, rooms, raw_has_lamps); the caller decides whether there is room
    for another unit. Mirrors Scene.cpp's migrateGroup()."""
    if not isinstance(o, dict):
        raise ApiError(400, "group must be an object")

    b = o.get("behaviour", "off")
    bl = b.lower() if isinstance(b, str) else "off"
    if bl in BEHAVIOUR_NAMES:
        pass
    elif bl in RETIRED_NAMES:
        # The four household behaviours retired when flats arrived. They
        # loaded as "home" then and they migrate as Home now.
        bl = "home"
    else:
        raise ApiError(400, "unknown behaviour")

    tmpl_key = BEHAVIOUR_TEMPLATE[bl]
    m = make_model(tmpl_key)
    if bl == "off":
        # Off: lit by nothing, at no time, with no life on top.
        # The level stays what the template set. litPercent 0 already keeps
        # every lamp dark, and a level of 0 would leave the model unusable
        # the moment somebody raised litPercent to look at it.
        m.update(onAnchor="clock", on=(0, 0), offAnchor="clock", off=(0, 0),
                 litPercent=0, flickerPercent=0, morning=False,
                 fadeMs=800, dayActivity=0, nightActivity=0)
    # Every lamp of a group kept its own moment inside the windows, its own
    # chance of taking part and its own television.
    m["individual"] = True
    m["name"] = str(o.get("name", ""))[:SCENE_NAME_LEN - 1]

    for key in ("onAnchor", "offAnchor"):
        v = o.get(key)
        if isinstance(v, str) and v.lower() in ("dusk", "dawn", "clock"):
            m[key] = v.lower()
    for key in ("on", "off"):
        v = o.get(key)
        if (isinstance(v, list) and len(v) == 2
                and all(isinstance(x, int) and not isinstance(x, bool) for x in v)):
            m[key] = (v[0], v[1])
    for key in ("litPercent", "flickerPercent"):
        v = o.get(key)
        if isinstance(v, int) and not isinstance(v, bool):
            m[key] = v
    if isinstance(o.get("morning"), bool):
        m["morning"] = o["morning"]
    if isinstance(o.get("level"), int) and not isinstance(o.get("level"), bool):
        m["level"] = o["level"]
    if isinstance(o.get("fadeMs"), int) and not isinstance(o.get("fadeMs"), bool):
        m["fadeMs"] = o["fadeMs"]
    for key in ("dayActivity", "nightActivity"):
        v = o.get(key)
        if isinstance(v, int) and not isinstance(v, bool):
            m[key] = v
    clamp_model(m)

    lamps_in = o.get("lamps", [])
    raw_has_lamps = isinstance(lamps_in, list) and len(lamps_in) > 0
    lampset = expand_lamps(lamps_in) if isinstance(lamps_in, list) else set()
    all_ranges = _ranges(lampset)
    rooms = []
    for i in range(0, len(all_ranges), ROOM_MAX_RANGES):
        if len(rooms) >= UNIT_MAX_ROOMS:
            raise ApiError(400, "too many rooms in a unit")
        rooms.append({"ranges": all_ranges[i:i + ROOM_MAX_RANGES], "role": "other"})

    return m, rooms, raw_has_lamps


def migrate_old(data):
    """Flats first, then groups, spec section 5. Returns (models, units)."""
    models = []
    units = []
    from_template = {}   # template key -> model index, for flats on a template

    flats_in = data.get("flats")
    if not isinstance(flats_in, list):
        flats_in = []
    for o in flats_in:
        if len(units) >= SCENE_MAX_UNITS:
            raise ApiError(400, "too many units")
        flat_model, unit, tmpl_key, custom = migrate_flat(o)
        tmpl_model = make_model(tmpl_key)

        on_template = (not custom) and _same_rhythm(flat_model, tmpl_model)
        mi = from_template.get(tmpl_key, -1) if on_template else -1
        if mi < 0:
            if len(models) >= SCENE_MAX_MODELS:
                raise ApiError(400, "too many models")
            m = dict(tmpl_model) if on_template else dict(flat_model)
            m["name"] = TEMPLATE_TITLES[tmpl_key] if on_template else unit["name"]
            m["name"] = _unique_name(models, m["name"])
            models.append(m)
            mi = len(models) - 1
            if on_template:
                from_template[tmpl_key] = mi
        unit["model"] = models[mi]["name"]
        units.append(unit)

    groups_in = data.get("groups")
    if not isinstance(groups_in, list):
        groups_in = []
    for o in groups_in:
        if len(models) >= SCENE_MAX_MODELS:
            raise ApiError(400, "too many models")
        m, rooms, raw_has_lamps = migrate_group(o)
        can_place = len(units) < SCENE_MAX_UNITS
        if not can_place and raw_has_lamps:
            # Lamps with nowhere to go: the model alone would be a scene
            # missing a street, so the whole document is refused.
            raise ApiError(400, "too many units")
        m["name"] = _unique_name(models, m["name"])
        models.append(m)
        if can_place and rooms:
            units.append({"name": m["name"], "building": "", "model": m["name"], "rooms": rooms})

    # An old scene that had neither a group nor a flat, which is what a
    # board that was never set up stored. Nothing crosses over and the
    # sketch does not seed, because the file exists, so the templates are
    # put in here instead of coming up with an empty Models tab.
    if not models and not units:
        models = [make_model(k) for k in TEMPLATE_KEYS]

    return models, units


# ---------------------------------------------------------------------------
# Status. SceneWebApi::statusJson() / SceneEngine::unitState() /
# SceneEngine::unitLitRooms(), simplified to range midpoints with no
# per-day draw and no per-lamp jitter, which is close enough to give the
# eight state words and the lkbthfrso letters spec 3.3 and 9.3/9.4 describe.
# ---------------------------------------------------------------------------

def _in_range(x, lo, hi):
    """True when x falls in [lo, hi] mod 1440; the interval may cross
    midnight (lo > hi), same semantics as the firmware's inWindow()."""
    lo %= 1440
    hi %= 1440
    x %= 1440
    if lo <= hi:
        return lo <= x <= hi
    return x >= lo or x <= hi


def _in_window(t, on, off):
    """The absolute minute-line version, for an hours window: on/off may run
    past 1440 to mean "tomorrow". Mirrors SceneEngine::inWindow()."""
    return (t >= on and t < off) or (t + 1440 >= on and t + 1440 < off)


def _range_mid(pair):
    a, b = pair
    if a < 0 or b < 0:
        return None
    return (a + b) / 2.0


def _is_away(model):
    """Mirrors ModelConfig::isAway(): a rhythm model with no wake and no
    bed is the away household."""
    return model["kind"] == "rhythm" and model["wake"][0] < 0 and model["bed"][0] < 0


def _hours_window(model, dusk, dawn):
    """The middle of each window, spec 9.3: anchorBase(on) + midpoint of on,
    to anchorBase(off) + midpoint of off."""
    def anchor_base(anchor, for_off):
        if anchor == "dusk":
            return dusk
        if anchor == "dawn":
            return dawn + 1440 if for_off else dawn
        return 0   # clock

    on_mid = (model["on"][0] + model["on"][1]) / 2.0
    off_mid = (model["off"][0] + model["off"][1]) / 2.0
    on = anchor_base(model["onAnchor"], False) + on_mid
    off = anchor_base(model["offAnchor"], True) + off_mid
    if off <= on:
        off += 1440
    return on, off


def unit_status(u, model, minutes, is_weekday, dusk, dawn):
    """Mirrors SceneEngine::unitState() and unitLitRooms(), spec 3.3 and
    9.3/9.4, but from the model's windows directly rather than a per-day
    draw: state, lit-letters."""
    m = minutes % 1440
    roles_present = {r["role"] for r in u["rooms"]}

    if model["kind"] == "hours":
        on, off = _hours_window(model, dusk, dawn)
        lit = _in_window(m, on, off)
        by_clock = model["onAnchor"] == "clock" and model["offAnchor"] == "clock"
        # A model no lamp takes part in is never lit, whatever its windows
        # say. An old Off group migrates to clock windows of [0, 0] and
        # [0, 0], which would otherwise read as open all day.
        if model["litPercent"] == 0:
            return ("closed" if by_clock else "dark"), ""
        state = ("open" if lit else "closed") if by_clock else ("lit" if lit else "dark")
        letters = [ROOM_LETTERS[r] for r in ROOM_NAMES if r in roles_present] if lit else []
        return state, "".join(letters)

    if _is_away(model):
        # A timer lamp in the evening, in the living room, or the other
        # room when there is no living room, and nothing else.
        letters = []
        if _in_range(m, 19 * 60, 22 * 60 + 30):
            if "living" in roles_present:
                letters.append(ROOM_LETTERS["living"])
            elif "other" in roles_present:
                letters.append(ROOM_LETTERS["other"])
        return "away", "".join(letters)

    wake_mid = _range_mid(model["wake"])
    leave_mid = _range_mid(model["leave"])
    home_mid = _range_mid(model["home"])
    bed_mid = _range_mid(model["bed"])

    asleep = (wake_mid is not None and bed_mid is not None
              and _in_range(m, bed_mid, wake_mid))
    leave_valid = model["leave"][0] >= 0 and model["leave"][1] >= 0
    is_out = (not asleep and is_weekday and leave_valid
              and leave_mid is not None and home_mid is not None
              and _in_range(m, leave_mid, home_mid))

    state = "asleep" if asleep else ("out" if is_out else "awake")
    if is_out:
        return state, ""

    # dinner = home + lerp(45, 90, u4); the mock has no daily draw, so it
    # uses the midpoint of that lerp, 45 + (90-45)/2 = 67.5.
    dinner_mid = (home_mid + 67.5) if home_mid is not None else None
    is_weekend_day = not is_weekday

    letters = []
    for role in ROOM_NAMES:
        if role not in roles_present:
            continue
        on = False
        if role == "kitchen":
            if wake_mid is not None and _in_range(m, wake_mid, wake_mid + 30):
                on = True
            if (home_mid is not None and dinner_mid is not None
                    and _in_range(m, home_mid + 10, dinner_mid + 60)):
                on = True
            if model["weekend"] and is_weekend_day and _in_range(m, 12 * 60, 13 * 60):
                on = True
        elif role == "hall":
            if leave_mid is not None and _in_range(m, leave_mid - 5, leave_mid + 2):
                on = True
            if home_mid is not None and _in_range(m, home_mid - 1, home_mid + 6):
                on = True
        elif role in ("living", "front", "back", "sign", "other"):
            # The three new roles behave as "other" under a rhythm model,
            # spec 2.4: an outside light that follows the evening.
            if (dinner_mid is not None and bed_mid is not None
                    and _in_range(m, dinner_mid, bed_mid - 10)):
                on = True
        elif role == "bedroom":
            if bed_mid is not None and _in_range(m, bed_mid - 15, bed_mid + 8):
                on = True
            if wake_mid is not None and _in_range(m, wake_mid - 2, wake_mid + 8):
                on = True
        elif role == "bathroom":
            if wake_mid is not None and _in_range(m, wake_mid + 5, wake_mid + 15):
                on = True
            if model["nightActivity"] > 0 and asleep and int(round(m)) % 10 == 0:
                on = True
        if on:
            letters.append(ROOM_LETTERS[role])

    return state, "".join(letters)


class SceneState:
    def __init__(self):
        self.lock = threading.Lock()

        self.latitude = 55.7
        self.longitude = 13.2
        self.autoTimezone = True
        self.utcOffsetMinutes = 60

        self.mode = "real"
        self.dayMinutes = 20
        self.dateFromSystem = True
        self.dayOfYearOverride = 172
        self.seed = 1
        self.enabled = True

        # SceneEngine::loadError(): why a stored scene would not load at
        # boot. None on a device whose scene loaded or that had nothing
        # stored, and then the status carries no "loadError" key at all.
        # The mock never fails a load by itself; a test sets this.
        self.load_error = None

        # The nine templates and no units is what a fresh device runs,
        # spec 2.3; the mock seeds the example town on top, spec 4.5.
        self.models = [make_model(k) for k in TEMPLATE_KEYS]

        # The seeded example town: the four households from today's fixture,
        # a shop with front/back/sign rooms, and a street with no building.
        self.units = [
            # One room with two lamps, so the GUI has a room to show the
            # list form on: the Anderssons light their living room from
            # two fittings.
            make_unit("Andersson", "Storgatan 3", "Family", [
                (16, "kitchen"), ([17, 41], "living"), (18, "bedroom"),
                (19, "bathroom"), (20, "hall"),
            ]),
            make_unit("Karlsson", "Storgatan 3", "Elderly couple", [
                (21, "kitchen"), (22, "living"), (23, "bedroom"),
                (24, "bathroom"), (25, "hall"),
            ]),
            make_unit("Nilsson", "Storgatan 3", "Night owl", [
                (26, "kitchen"), (27, "living"), (28, "bedroom"),
                (29, "bathroom"), (30, "hall"),
            ]),
            make_unit("Persson", "Kyrkogatan 1", "Family", [
                (31, "kitchen"), (32, "living"), (33, "bedroom"),
                (34, "bathroom"), (35, "hall"),
            ]),
            make_unit("Svensson", "Kyrkogatan 1", "Away", [
                (36, "kitchen"), (37, "living"), (38, "bedroom"),
                (39, "bathroom"), (40, "hall"),
            ]),
            make_unit("Ica Nära", "Storgatan 3", "Shop", [
                ([96, 97], "front"), (98, "back"), (99, "sign"),
            ]),
            make_unit("Main street", "", "Street light", [
                (list(range(0, 16)), "other"),
            ]),
        ]

        # Manual clock anchor, minutes since midnight (0 = start manual at
        # a plausible evening hour so the town has something lit).
        self._manual_minutes = 20 * 60
        self._anchor_wall = time.time()
        self._anchor_sim = self._wallclock_minutes()

    def _wallclock_minutes(self):
        t = time.localtime()
        return t.tm_hour * 60 + t.tm_min + t.tm_sec / 60.0

    def sim_minutes(self):
        with self.lock:
            if self.mode == "manual":
                return self._manual_minutes % 1440
            elapsed_minutes = (time.time() - self._anchor_wall) / 60.0
            if self.mode == "accelerated":
                rate = 1440.0 / max(1, self.dayMinutes)
                return (self._anchor_sim + elapsed_minutes * rate) % 1440
            # real
            return (self._anchor_sim + elapsed_minutes) % 1440

    def day_of_year(self):
        if self.dateFromSystem:
            return time.localtime().tm_yday
        return self.dayOfYearOverride

    def dusk_dawn(self):
        doy = self.day_of_year()
        # Peaks near the summer solstice (day ~172): dusk later, dawn
        # earlier, both shifting up to 2 hours either way over the year.
        shift = 120.0 * math.sin(2 * math.pi * (doy - 80) / 365.0)
        dusk = 19 * 60 + 58 + shift
        dawn = 5 * 60 + 47 - shift
        return dusk % 1440, dawn % 1440

    def total_lamps(self):
        lamps = set()
        for u in self.units:
            for r in u["rooms"]:
                for a, b in r["ranges"]:
                    lamps.update(range(a, b + 1))
        return len(lamps)

    def find_model(self, name):
        return find_model(self.models, name)

    def lit_fraction(self, minutes, dusk, dawn):
        def in_night(m):
            if dusk <= dawn:
                return dusk <= m <= dawn
            return m >= dusk or m <= dawn

        if not in_night(minutes):
            return 0.0
        since_dusk = (minutes - dusk) % 1440
        until_dawn = (dawn - minutes) % 1440
        ramp = min(since_dusk / 60.0, 1.0)
        fade = min(until_dawn / 45.0, 1.0)
        return max(0.0, min(ramp, fade))

    def lit_count(self):
        minutes = self.sim_minutes()
        dusk, dawn = self.dusk_dawn()
        return self.lit_count_at(minutes, dusk, dawn)

    def lit_count_at(self, minutes, dusk, dawn):
        # Lock-free: called both standalone (lit_count) and from inside
        # status_json's locked block, where sim_minutes() must not be
        # called again (self.lock is not reentrant).
        if not self.enabled:
            return 0
        frac = self.lit_fraction(minutes, dusk, dawn)
        lit = int(round(frac * self.total_lamps() * 0.85))
        # The seeded units cover more lamps than the seeded hardware has,
        # so clamp to the hardware count; the Home view reads this as
        # "<lit> of <lamps> lit" and must never show more than the total.
        return min(lit, LAMPS.lamp_count())

    def active_count_at(self, minutes):
        # Lamps in a short event right now. The engine counts them for real;
        # the mock only has to be plausible and steady from minute to minute:
        # nothing between 23:00 and 05:30, one to three the rest of the day.
        if not self.enabled:
            return 0
        m = int(minutes) % 1440
        if m >= 23 * 60 or m < 5 * 60 + 30:
            return 0
        h = (m * 2654435761) & 0xFFFFFFFF
        h ^= h >> 15
        return 1 + h % 3

    def config_json(self):
        with self.lock:
            return {
                "location": {
                    "lat": self.latitude,
                    "lon": self.longitude,
                    "autoTimezone": self.autoTimezone,
                    "utcOffsetMinutes": self.utcOffsetMinutes,
                },
                "clock": {
                    "mode": self.mode,
                    "dayMinutes": self.dayMinutes,
                    "manualTime": fmt_time(self._manual_minutes),
                    "dateFromSystem": self.dateFromSystem,
                    "dayOfYear": self.dayOfYearOverride,
                },
                "seed": self.seed,
                "models": [model_to_json(m) for m in self.models],
                "units": [unit_to_json(u) for u in self.units],
            }

    def status_json(self):
        minutes = self.sim_minutes()
        dusk, dawn = self.dusk_dawn()
        sunset = (dusk - 25) % 1440
        sunrise = (dawn + 25) % 1440
        with self.lock:
            # Weekday, spec 3.1: tm_wday derived from the simulated day of
            # year with 2026-01-01 (doy 1) a Thursday; wday 0/6 is the
            # weekend, used by unit_status() for the "out" state.
            wday = (self.day_of_year() + 3) % 7
            is_weekday = wday not in (0, 6)
            units_status = []
            for u in self.units:
                mi = self.find_model(u["model"])
                if mi < 0:
                    continue
                state, lit = unit_status(u, self.models[mi], minutes, is_weekday, dusk, dawn)
                units_status.append({"name": u["name"], "state": state, "lit": lit})
            out = {
                "time": fmt_time(minutes),
                "minutes": int(round(minutes)),
                "dayOfYear": self.day_of_year(),
                "dusk": fmt_time(dusk),
                "dawn": fmt_time(dawn),
                "sunset": fmt_time(sunset),
                "sunrise": fmt_time(sunrise),
                "mode": self.mode,
                "dayMinutes": self.dayMinutes,
                "clockValid": True,
                "enabled": self.enabled,
                "lit": self.lit_count_at(minutes, dusk, dawn),
                "active": self.active_count_at(minutes),
                "lamps": LAMPS.lamp_count(),
                "units": units_status,
            }
            if self.load_error:
                out["loadError"] = self.load_error
            return out

    def apply_config(self, data):
        if not isinstance(data, dict):
            raise ApiError(400, "invalid JSON")

        loc = data.get("location")
        new_lat = self.latitude
        new_lon = self.longitude
        new_auto = self.autoTimezone
        new_offset = self.utcOffsetMinutes
        if isinstance(loc, dict):
            if isinstance(loc.get("lat"), (int, float)):
                new_lat = float(loc["lat"])
            if isinstance(loc.get("lon"), (int, float)):
                new_lon = float(loc["lon"])
            if isinstance(loc.get("autoTimezone"), bool):
                new_auto = loc["autoTimezone"]
            if isinstance(loc.get("utcOffsetMinutes"), int):
                new_offset = loc["utcOffsetMinutes"]

        clk = data.get("clock")
        new_mode = self.mode
        new_day_minutes = self.dayMinutes
        new_manual = self._manual_minutes
        new_date_from_system = self.dateFromSystem
        new_day_of_year = self.dayOfYearOverride
        if isinstance(clk, dict):
            if isinstance(clk.get("mode"), str):
                if clk["mode"] not in ("real", "accelerated", "manual"):
                    raise ApiError(400, "clock.mode must be real, accelerated or manual")
                new_mode = clk["mode"]
            if isinstance(clk.get("dayMinutes"), int):
                new_day_minutes = clk["dayMinutes"]
            if isinstance(clk.get("manualTime"), str):
                new_manual = parse_time(clk["manualTime"])
            elif isinstance(clk.get("manualTime"), int):
                new_manual = clk["manualTime"]
            if isinstance(clk.get("dateFromSystem"), bool):
                new_date_from_system = clk["dateFromSystem"]
            if isinstance(clk.get("dayOfYear"), int):
                new_day_of_year = clk["dayOfYear"]

        new_seed = self.seed
        if isinstance(data.get("seed"), int):
            new_seed = data["seed"]

        # Models first, then units against whatever models the scene has
        # now: the ones just read, or the ones it already had. Mirrors
        # SceneConfig::fromJson()'s two top-level ifs (spec 5, 6).
        models_in = data.get("models")
        has_models = isinstance(models_in, list)
        units_in = data.get("units")
        has_units = isinstance(units_in, list)

        new_models = self.models
        new_units = self.units

        if has_models:
            was_following = [u["model"] for u in self.units]
            new_models = []
            for o in models_in:
                if len(new_models) >= SCENE_MAX_MODELS:
                    raise ApiError(400, "too many models")
                new_models.append(model_from_json(o, new_models))

            if not has_units:
                # The units that were not sent, against the table just
                # read: a unit whose model is still there by name follows
                # it wherever it moved to; a unit whose model went with
                # this save goes with it.
                kept = []
                for u, old_model_name in zip(self.units, was_following):
                    mi = find_model(new_models, old_model_name)
                    if mi < 0:
                        continue
                    nu = dict(u)
                    nu["model"] = new_models[mi]["name"]
                    kept.append(nu)
                new_units = kept

        if has_units:
            new_units = []
            for o in units_in:
                if len(new_units) >= SCENE_MAX_UNITS:
                    raise ApiError(400, "too many units")
                new_units.append(unit_from_json(o, new_models))
        elif not has_models and ("flats" in data or "groups" in data):
            # The old shape, spec section 5: no "models" and no "units",
            # but "flats" or "groups" present.
            new_models, new_units = migrate_old(data)

        clamp_units(new_units)

        manual_time_given = isinstance(clk, dict) and (
            isinstance(clk.get("manualTime"), str) or isinstance(clk.get("manualTime"), int)
        )

        with self.lock:
            # Settle the clock under the settings that were running before
            # this change, so switching mode or rate does not jump time.
            self._reanchor_locked()

            self.latitude, self.longitude = new_lat, new_lon
            self.autoTimezone, self.utcOffsetMinutes = new_auto, new_offset
            self.mode = new_mode
            self.dayMinutes = new_day_minutes
            self._manual_minutes = new_manual % 1440
            self.dateFromSystem = new_date_from_system
            self.dayOfYearOverride = new_day_of_year
            self.seed = new_seed
            self.models = new_models
            self.units = new_units
            if manual_time_given:
                self._anchor_sim = self._manual_minutes
                self._anchor_wall = time.time()

    def _reanchor_locked(self):
        # Must be called with self.lock held. Re-anchors the running clock
        # to whatever it reads right now, so a mode/rate change does not
        # jump the visible time.
        if self.mode == "manual":
            return
        elapsed_minutes = (time.time() - self._anchor_wall) / 60.0
        if self.mode == "accelerated":
            rate = 1440.0 / max(1, self.dayMinutes)
            self._anchor_sim = (self._anchor_sim + elapsed_minutes * rate) % 1440
        else:
            self._anchor_sim = (self._anchor_sim + elapsed_minutes) % 1440
        self._anchor_wall = time.time()

    def apply_clock(self, data):
        if not isinstance(data, dict):
            raise ApiError(400, "invalid JSON")

        with self.lock:
            self._reanchor_locked()

            if isinstance(data.get("mode"), str):
                if data["mode"] not in ("real", "accelerated", "manual"):
                    raise ApiError(400, "mode must be real, accelerated or manual")
                self.mode = data["mode"]

            if isinstance(data.get("time"), str):
                self._manual_minutes = parse_time(data["time"]) % 1440
                self._anchor_sim = self._manual_minutes
                self._anchor_wall = time.time()
            elif isinstance(data.get("time"), int):
                self._manual_minutes = data["time"] % 1440
                self._anchor_sim = self._manual_minutes
                self._anchor_wall = time.time()

            if isinstance(data.get("dayMinutes"), int):
                d = data["dayMinutes"]
                if d < 1 or d > 1440:
                    raise ApiError(400, "dayMinutes must be 1..1440")
                self.dayMinutes = d

            if isinstance(data.get("dayOfYear"), int):
                d = data["dayOfYear"]
                if d < 1 or d > 366:
                    raise ApiError(400, "dayOfYear must be 1..366")
                self.dayOfYearOverride = d
                self.dateFromSystem = False

            if isinstance(data.get("enabled"), bool):
                self.enabled = data["enabled"]

    def presets_json(self):
        # The nine templates, each a whole model object as the scene
        # document writes it, keyed by the template's key: what "New
        # model, start from" offers, spec 2.3 / 6.
        return {
            "models": {key: model_to_json(MODEL_TEMPLATES[key]) for key in TEMPLATE_KEYS},
            "rooms": list(ROOM_NAMES),
        }


def parse_time(s):
    if not s:
        raise ApiError(400, "time must be HH:MM")
    if ":" in s:
        h, _, m = s.partition(":")
        try:
            h, m = int(h), int(m)
        except ValueError:
            raise ApiError(400, "time must be HH:MM")
        if h < 0 or h > 47 or m < 0 or m > 59:
            raise ApiError(400, "time must be HH:MM")
        return h * 60 + m
    try:
        v = int(s)
    except ValueError:
        raise ApiError(400, "time must be HH:MM")
    if v < 0 or v > 2879:
        raise ApiError(400, "time must be HH:MM")
    return v


# ---------------------------------------------------------------------------
# Net state, spec 3.2 / 3.6
# ---------------------------------------------------------------------------

SCAN_NETWORKS = [
    {"ssid": "HomeWifi", "rssi": -45, "secure": True},
    {"ssid": "CafeOpen", "rssi": -60, "secure": False},
    {"ssid": "Neighbour", "rssi": -72, "secure": True},
    {"ssid": "IoT-Guest", "rssi": -80, "secure": False},
]


class NetState:
    def __init__(self, portal, password):
        self.lock = threading.Lock()
        self.hostname = "miniworld"
        self.password = password or ""
        self.ntp = "pool.ntp.org"
        self.tz = "CET-1CEST,M3.5.0,M10.5.0/3"
        self.sta_ssid = ""
        self.sta_pass = ""

        self.apSsid = "miniWorld-MOCK"
        self.apIp = "192.168.4.1"
        self.connectResult = "idle"

        if portal:
            self.mode = "portal"
            self.apActive = True
            self.ssid = ""
            self.ip = ""
            self.rssi = 0
        else:
            self.mode = "online"
            self.apActive = False
            self.sta_ssid = "MockHome"
            self.ssid = "MockHome"
            self.ip = "192.168.1.50"
            self.rssi = -55

    def status_json(self):
        with self.lock:
            return {
                "mode": self.mode,
                "ssid": self.ssid,
                "ip": self.ip,
                "rssi": self.rssi,
                "hostname": self.hostname,
                "apSsid": self.apSsid,
                "apIp": self.apIp,
                "apActive": self.apActive,
                "connectResult": self.connectResult,
                "auth": bool(self.password),
                "timeValid": True,
            }

    def config_json(self):
        # NetConfig::toJson(false) (the API form) writes sta.ssid
        # unconditionally: only sta.pass and password are secrets, and
        # those are the two fields this never returns.
        with self.lock:
            return {
                "sta": {"ssid": self.sta_ssid},
                "hostname": self.hostname,
                "ntp": self.ntp,
                "tz": self.tz,
                "hasWifi": bool(self.sta_ssid),
                "hasPassword": bool(self.password),
            }

    def apply_config(self, data):
        if not isinstance(data, dict):
            raise ApiError(400, "invalid JSON")
        with self.lock:
            if isinstance(data.get("hostname"), str):
                h = data["hostname"].lower()
                ok = 1 <= len(h) <= 24 and all(
                    c.isdigit() or c.islower() or c == "-" for c in h
                )
                self.hostname = h if ok else "miniworld"
            if isinstance(data.get("password"), str):
                self.password = data["password"]
            if isinstance(data.get("ntp"), str):
                self.ntp = data["ntp"]
            if isinstance(data.get("tz"), str):
                self.tz = data["tz"]

    def connect(self, data):
        if not isinstance(data, dict) or not isinstance(data.get("ssid"), str):
            raise ApiError(400, "ssid required")
        ssid = data["ssid"]
        password = data.get("pass", "")
        with self.lock:
            self.sta_ssid = ssid
            self.sta_pass = password
            self.connectResult = "connecting"
            self.mode = "connecting"

        def finish():
            with self.lock:
                if password == "wrong":
                    self.connectResult = "failed"
                    self.mode = "portal"
                    self.apActive = True
                else:
                    self.connectResult = "ok"
                    self.mode = "online"
                    self.ssid = ssid
                    self.ip = "192.168.1.42"
                    self.rssi = -58

        threading.Timer(4.0, finish).start()

    def forget(self):
        with self.lock:
            self.sta_ssid = ""
            self.sta_pass = ""
            self.connectResult = "idle"
            self.mode = "portal"
            self.apActive = True
            self.ssid = ""
            self.ip = ""


# ---------------------------------------------------------------------------
# Global state and startup time, for system status
# ---------------------------------------------------------------------------

LAMPS = LampState()
SCENE = SceneState()
NET = None  # set in main() once args are parsed
BOOT_TIME = time.time()


def system_status_json():
    now = time.localtime()
    return {
        "firmware": "0.1.0",
        "build": FIRMWARE.build,
        "uptime": int(time.time() - BOOT_TIME),
        "heap": 180000,
        "time": time.strftime("%Y-%m-%dT%H:%M:%S", now),
        "timeValid": True,
    }


class FirmwareState:
    """What the device does with an upload, minus the flash: the band and
    header checks, an MD5 over the body, the scan for the banner and the
    build stamp in 4 kB pieces with a 63-byte carry like the firmware's,
    and on success the reported build becomes the uploaded stamp, which is
    what tools/ota.sh polls for.

    The device decides the 413 in its header parser, before basic auth,
    and then the MD5 and build headers; check() keeps that order. One
    difference stays: the mock's auth check runs before this class sees
    the request, so an unauthorised oversize POST gets 401 here and 413
    on the device."""

    def __init__(self):
        self.build = "mock"
        self.staged = False

    def free(self):
        return FIRMWARE_FS_TOTAL - FIRMWARE_FS_USED

    def max_size(self):
        return min(FIRMWARE_MAX_SIZE, self.free() - FIRMWARE_FS_MARGIN)

    def info_json(self):
        return {
            "fsTotal": FIRMWARE_FS_TOTAL,
            "fsFree": self.free(),
            "maxSize": self.max_size(),
            "minSize": FIRMWARE_MIN_SIZE,
            "staged": self.staged,
        }

    def check(self, length, md5, build):
        """The checks the device makes from the headers alone, before it
        reads a body byte. Raises ApiError; returns None when all is well."""
        if length < FIRMWARE_MIN_SIZE or length > self.max_size():
            raise ApiError(413, "payload too large")
        if len(md5) != 32 or any(c not in "0123456789abcdefABCDEF" for c in md5):
            raise ApiError(400, "missing X-Firmware-MD5")
        if not build:
            raise ApiError(400, "missing X-Firmware-Build")
        if len(build) > 63:
            raise ApiError(400, "X-Firmware-Build longer than 63 characters")

    @staticmethod
    def _contains(data, needle):
        carry = b""
        for off in range(0, len(data), FIRMWARE_SCAN_CHUNK):
            piece = carry + data[off:off + FIRMWARE_SCAN_CHUNK]
            if needle in piece:
                return True
            carry = piece[-FIRMWARE_SCAN_OVERLAP:]
        return False

    def upload(self, body, md5, build):
        self.check(len(body), md5, build)
        got = hashlib.md5(body).hexdigest()
        if got != md5.lower():
            raise ApiError(422, "md5 mismatch")
        if not self._contains(body, FIRMWARE_BANNER):
            raise ApiError(422, "not this sketch")
        if not self._contains(body, build.encode()):
            raise ApiError(422, "build stamp not in image")
        self.build = build
        self.staged = False   # the device reboots and cleans up at boot
        sys.stderr.write("mock: firmware %d bytes staged, build %s (reboot ignored)\n"
                         % (len(body), build))
        return {"ok": True, "size": len(body), "md5": got, "build": build}


FIRMWARE = FirmwareState()


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "miniWorldMock/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length <= 0:
            return b""
        return self.rfile.read(length)

    def _check_auth(self):
        password = self.server.net.password
        if not password:
            return True
        header = self.headers.get("Authorization", "")
        if header.startswith("Basic "):
            try:
                decoded = base64.b64decode(header[6:]).decode("utf-8", "replace")
            except Exception:
                decoded = ""
            _, _, given = decoded.partition(":")
            if given == password:
                return True
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="miniWorld"')
        self._send_json({"error": "unauthorized"}, 401, skip_status_line=True)
        return False

    def _send_json(self, obj, code, skip_status_line=False):
        payload = json.dumps(obj).encode("utf-8")
        if not skip_status_line:
            self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _send_redirect(self, url):
        self.send_response(302)
        self.send_header("Location", url)
        self.send_header("Content-Length", "0")
        self.end_headers()

    # The firmware compares the Host header with the AP address; the mock
    # compares it with the address the client actually connected to, so a
    # request carrying somebody else's Host reads as foreign here too.
    def _foreign_host(self):
        host = (self.headers.get("Host") or "").strip().lower()
        if not host:
            return False
        ip, port = self.connection.getsockname()[:2]
        own = {
            "%s:%d" % (ip, port), "localhost:%d" % port,
            "127.0.0.1:%d" % port, "[::1]:%d" % port,
        }
        if port == 80:
            own |= {ip, "localhost", "127.0.0.1"}
        return host not in own

    def _send_html(self, text, code=200):
        payload = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def do_PUT(self):
        self._dispatch("PUT")

    def _dispatch(self, method):
        path = self.path.split("?", 1)[0]
        body_bytes = self._read_body() if method in ("POST", "PUT") else b""

        # Routing steps 1 and 2 of spec section 3.5, before the password
        # check: a probe has no credentials to offer, and its whole job is
        # to open the sign-in sheet.
        portal_url = "http://%s/" % self.server.net.apIp
        if path in CAPTIVE_PROBES:
            self._send_redirect(portal_url)
            return
        if self.server.net.mode == "portal" and self._foreign_host():
            self._send_redirect(portal_url)
            return

        if not self._check_auth():
            return

        if path == "/api/system/firmware" and method == "POST":
            # The device answers 400 and 413 from the headers alone, before
            # the body; the mock has already read the body, which is the one
            # difference, and it checks in the same order.
            md5 = (self.headers.get("X-Firmware-MD5") or "").strip()
            build = (self.headers.get("X-Firmware-Build") or "").strip()
            length = int(self.headers.get("Content-Length", 0) or 0)
            try:
                FIRMWARE.check(length, md5, build)
                result = FIRMWARE.upload(body_bytes, md5, build)
            except ApiError as e:
                payload = {"error": e.message}
                if e.code == 413:
                    payload["max"] = FIRMWARE.max_size()
                self._send_json(payload, e.code)
                return
            self._send_json(result, 200)
            return

        if path in ("/", "/index.html") and method == "GET":
            self._send_html(build_page(WEBDIR))
            return

        if path.startswith("/api/"):
            data = None
            if body_bytes:
                try:
                    data = json.loads(body_bytes.decode("utf-8"))
                except Exception:
                    self._send_json({"error": "invalid JSON"}, 400)
                    return
            try:
                code, result = self._api(method, path, data)
            except ApiError as e:
                self._send_json({"error": e.message}, e.code)
                return
            self._send_json(result, code)
            return

        self._send_json({"error": "not found"}, 404)

    # There is no lamp to blink here, so the mock does what the firmware
    # does apart from the light itself: it checks the numbers and says so on
    # stderr, which is enough to watch the GUI's debounce from the log.
    # Both forms are taken, {"lamp": n} and {"lamps": [n, ...]}, the second
    # up to eight lamps blinked together.
    def _identify(self, data):
        if not isinstance(data, dict):
            raise ApiError(400, "invalid JSON")
        count = LAMPS.lamp_count()

        def bad():
            if count == 0:
                return ApiError(400, "no lamps are fitted")
            return ApiError(400, "lamp must be 0..%d" % (count - 1))

        if "lamps" in data:
            given = data["lamps"]
            if not isinstance(given, list):
                raise ApiError(400, "lamps must be an array")
            if len(given) > IDENTIFY_MAX_LAMPS:
                raise ApiError(400, "at most 8 lamps")
        else:
            given = [data.get("lamp")]
        lamps = []
        for lamp in given:
            if not isinstance(lamp, int) or isinstance(lamp, bool) \
                    or lamp < 0 or lamp >= count:
                raise bad()
            lamps.append(lamp)
        if not lamps:
            raise bad()
        sys.stderr.write("mock: identify lamps %s\n"
                         % ", ".join(str(n) for n in lamps))
        return {"ok": True, "lamps": lamps}

    def _api(self, method, path, data):
        if path == "/api/lamps/config":
            if method == "GET":
                return 200, LAMPS.config_json()
            if method in ("PUT", "POST"):
                LAMPS.apply_json(data or {})
                return 200, LAMPS.status_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/lamps/status":
            if method == "GET":
                return 200, LAMPS.status_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/lamps/probe":
            if method == "POST":
                return 200, LAMPS.probe()
            raise ApiError(405, "method not allowed")

        if path == "/api/lamps/test":
            if method == "POST":
                return 200, LAMPS.test(data or {})
            raise ApiError(405, "method not allowed")

        if path == "/api/scene/config":
            if method == "GET":
                return 200, SCENE.config_json()
            if method in ("PUT", "POST"):
                SCENE.apply_config(data or {})
                return 200, SCENE.status_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/scene/status":
            if method == "GET":
                return 200, SCENE.status_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/scene/clock":
            if method in ("PUT", "POST"):
                SCENE.apply_clock(data or {})
                return 200, SCENE.status_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/scene/identify":
            if method == "POST":
                return 200, self._identify(data or {})
            raise ApiError(405, "method not allowed")

        if path == "/api/scene/presets":
            if method == "GET":
                return 200, SCENE.presets_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/net/status":
            if method == "GET":
                return 200, self.server.net.status_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/net/scan":
            if method == "GET":
                return 200, {"networks": SCAN_NETWORKS}
            raise ApiError(405, "method not allowed")

        if path == "/api/net/config":
            if method == "GET":
                return 200, self.server.net.config_json()
            if method == "PUT":
                self.server.net.apply_config(data or {})
                return 200, self.server.net.config_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/net/connect":
            if method == "POST":
                self.server.net.connect(data or {})
                return 202, self.server.net.status_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/net/forget":
            if method == "POST":
                self.server.net.forget()
                return 200, self.server.net.status_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/system/firmware":
            if method == "GET":
                return 200, FIRMWARE.info_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/system/status":
            if method == "GET":
                return 200, system_status_json()
            raise ApiError(405, "method not allowed")

        if path == "/api/system/reboot":
            if method == "POST":
                sys.stderr.write("mock: reboot requested (ignored)\n")
                return 200, {"ok": True}
            raise ApiError(405, "method not allowed")

        raise ApiError(404, "not found")


def main():
    parser = argparse.ArgumentParser(description="miniWorld web GUI mock server")
    parser.add_argument("--portal", action="store_true", help="start in Portal mode")
    parser.add_argument("--password", default="", help="device password for basic auth")
    parser.add_argument("--port", type=int, default=8080, help="TCP port, default 8080")
    args = parser.parse_args()

    global NET
    NET = NetState(args.portal, args.password)

    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    server.net = NET
    print("mock: listening on http://0.0.0.0:%d/" % args.port)
    print("mock: portal=%s auth=%s" % (args.portal, bool(args.password)))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
