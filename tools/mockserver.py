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
SCENE_MAX_GROUPS = 16

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

# dayActivity 0 none, 1 low, 2 normal, 3 high; nightActivity 0 none, 1 rare,
# 2 normal, 3 often. The four households (family, elderly, nightowl, away)
# follow Behaviour in the firmware, which numbers them after allnight.
BEHAVIOUR_PRESETS = {
    "street": {
        "onAnchor": "dusk", "on": (-10, 5),
        "offAnchor": "dawn", "off": (-5, 15),
        "litPercent": 100, "flickerPercent": 0, "morning": False,
        "level": 255, "fadeMs": 4000,
        "dayActivity": 0, "nightActivity": 0,
    },
    "home": {
        "onAnchor": "dusk", "on": (0, 240),
        "offAnchor": "clock", "off": (1320, 1470),
        "litPercent": 85, "flickerPercent": 12, "morning": True,
        "level": 200, "fadeMs": 300,
        "dayActivity": 2, "nightActivity": 1,
    },
    "shop": {
        "onAnchor": "clock", "on": (510, 540),
        "offAnchor": "clock", "off": (1080, 1110),
        "litPercent": 100, "flickerPercent": 0, "morning": False,
        "level": 230, "fadeMs": 200,
        "dayActivity": 1, "nightActivity": 0,
    },
    "late": {
        "onAnchor": "dusk", "on": (-30, 30),
        "offAnchor": "clock", "off": (1380, 1560),
        "litPercent": 100, "flickerPercent": 0, "morning": False,
        "level": 220, "fadeMs": 800,
        "dayActivity": 1, "nightActivity": 0,
    },
    "allnight": {
        "onAnchor": "dusk", "on": (-15, 0),
        "offAnchor": "dawn", "off": (0, 15),
        "litPercent": 100, "flickerPercent": 0, "morning": False,
        "level": 255, "fadeMs": 800,
        "dayActivity": 0, "nightActivity": 0,
    },
    "family": {
        "onAnchor": "dusk", "on": (0, 180),
        "offAnchor": "clock", "off": (1350, 1440),
        "litPercent": 90, "flickerPercent": 25, "morning": True,
        "level": 210, "fadeMs": 300,
        "dayActivity": 3, "nightActivity": 1,
    },
    "elderly": {
        "onAnchor": "dusk", "on": (-30, 60),
        "offAnchor": "clock", "off": (1260, 1335),
        "litPercent": 90, "flickerPercent": 8, "morning": True,
        "level": 180, "fadeMs": 400,
        "dayActivity": 2, "nightActivity": 3,
    },
    "nightowl": {
        "onAnchor": "dusk", "on": (60, 240),
        "offAnchor": "clock", "off": (1470, 1590),
        "litPercent": 80, "flickerPercent": 30, "morning": False,
        "level": 200, "fadeMs": 300,
        "dayActivity": 1, "nightActivity": 1,
    },
    # A timer lamp: one in four, the same minutes every evening, no life.
    "away": {
        "onAnchor": "clock", "on": (1140, 1150),
        "offAnchor": "clock", "off": (1350, 1360),
        "litPercent": 25, "flickerPercent": 0, "morning": False,
        "level": 200, "fadeMs": 0,
        "dayActivity": 0, "nightActivity": 0,
    },
    "off": {
        "onAnchor": "clock", "on": (0, 0),
        "offAnchor": "clock", "off": (0, 0),
        "litPercent": 0, "flickerPercent": 0, "morning": False,
        "level": 0, "fadeMs": 800,
        "dayActivity": 0, "nightActivity": 0,
    },
}

# Flats: spec section 2. SCENE_MAX_FLATS / FLAT_MAX_ROOMS mirror the firmware
# #defines; ROOM_NAMES is the Room enum order, used both for /api/scene/presets
# "rooms" and for the order status letters are checked in.
SCENE_MAX_FLATS = 32
FLAT_MAX_ROOMS = 12
FLAT_TYPES = ("family", "elderly", "nightowl", "away", "custom")
ROOM_NAMES = ("living", "kitchen", "bedroom", "bathroom", "hall", "other")
ROOM_LETTERS = {
    "living": "l", "kitchen": "k", "bedroom": "b",
    "bathroom": "t", "hall": "h", "other": "o",
}

# setPreset(type), weekday values, spec section 2 table. Ranges are minutes
# since midnight, (from, to); away sets every range to (-1, -1). "custom"
# has no entry here: setPreset(custom) reuses the family values (spec: "custom
# | family values"), so flat_from_json/make_flat fall back to "family" for it.
FLAT_HOUSEHOLD_PRESETS = {
    "family": {
        "wake": (390, 435), "leave": (450, 495), "home": (960, 1050), "bed": (1350, 1410),
        "outPercent": 14, "tvPercent": 70, "level": 210, "fadeMs": 300,
        "dayActivity": 3, "nightActivity": 1,
    },
    "elderly": {
        "wake": (345, 390), "leave": (570, 630), "home": (720, 810), "bed": (1275, 1335),
        "outPercent": 7, "tvPercent": 40, "level": 180, "fadeMs": 400,
        "dayActivity": 2, "nightActivity": 3,
    },
    "nightowl": {
        "wake": (570, 660), "leave": (690, 750), "home": (1140, 1260), "bed": (1485, 1575),
        "outPercent": 28, "tvPercent": 80, "level": 200, "fadeMs": 300,
        "dayActivity": 1, "nightActivity": 1,
    },
    "away": {
        "wake": (-1, -1), "leave": (-1, -1), "home": (-1, -1), "bed": (-1, -1),
        "outPercent": 0, "tvPercent": 0, "level": 200, "fadeMs": 0,
        "dayActivity": 0, "nightActivity": 0,
    },
}


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

def compress_lamps(lamps):
    """Mirrors Scene.cpp's lampsToJson: ints for single lamps, a pair of
    ints for two adjacent, an "a-b" string for a longer run."""
    s = sorted(set(lamps))
    out = []
    i = 0
    n = len(s)
    while i < n:
        start = s[i]
        j = i
        while j + 1 < n and s[j + 1] == s[j] + 1:
            j += 1
        end = s[j]
        if end == start:
            out.append(start)
        elif end == start + 1:
            out.append(start)
            out.append(end)
        else:
            out.append("%d-%d" % (start, end))
        i = j + 1
    return out


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


def make_group(name, behaviour, lo, hi):
    preset = BEHAVIOUR_PRESETS[behaviour]
    g = dict(preset)
    g["name"] = name
    g["behaviour"] = behaviour
    g["lamps"] = set(range(lo, hi + 1))
    return g


def group_to_json(g):
    return {
        "name": g["name"],
        "behaviour": g["behaviour"],
        "lamps": compress_lamps(g["lamps"]),
        "onAnchor": g["onAnchor"],
        "on": list(g["on"]),
        "offAnchor": g["offAnchor"],
        "off": list(g["off"]),
        "litPercent": g["litPercent"],
        "flickerPercent": g["flickerPercent"],
        "morning": g["morning"],
        "level": g["level"],
        "fadeMs": g["fadeMs"],
        "dayActivity": g["dayActivity"],
        "nightActivity": g["nightActivity"],
    }


def group_from_json(o):
    if not isinstance(o, dict):
        raise ApiError(400, "group must be an object")
    behaviour = o.get("behaviour", "off")
    if behaviour not in BEHAVIOUR_PRESETS:
        raise ApiError(400, "unknown behaviour")
    g = dict(BEHAVIOUR_PRESETS[behaviour])
    g["name"] = str(o.get("name", ""))[:23]
    g["behaviour"] = behaviour
    g["lamps"] = expand_lamps(o.get("lamps", []))
    if isinstance(o.get("onAnchor"), str):
        g["onAnchor"] = o["onAnchor"]
    if isinstance(o.get("offAnchor"), str):
        g["offAnchor"] = o["offAnchor"]
    on = o.get("on")
    if isinstance(on, list) and len(on) == 2:
        g["on"] = (on[0], on[1])
    off = o.get("off")
    if isinstance(off, list) and len(off) == 2:
        g["off"] = (off[0], off[1])
    for key in ("litPercent", "flickerPercent", "level", "fadeMs"):
        if isinstance(o.get(key), int):
            g[key] = o[key]
    if isinstance(o.get("morning"), bool):
        g["morning"] = o["morning"]
    # Absent means the preset's value, the way SceneConfig::fromJson reads a
    # scene written before the activity layer existed. clamp() caps both at 3.
    for key in ("dayActivity", "nightActivity"):
        v = o.get(key)
        if isinstance(v, int) and not isinstance(v, bool):
            g[key] = max(0, min(3, v))
    return g


# ---------------------------------------------------------------------------
# Flats, spec section 2 (model), 3.5 (status) and 4.4 (this mock)
# ---------------------------------------------------------------------------

def make_flat(name, building, ftype, weekend, room_lamps_roles):
    """Seed helper, the flat equivalent of make_group: setPreset(ftype) then
    the fixed fields, no further overrides."""
    preset = FLAT_HOUSEHOLD_PRESETS["family" if ftype == "custom" else ftype]
    f = dict(preset)
    f["name"] = name
    f["building"] = building
    f["type"] = ftype
    f["weekend"] = weekend
    f["rooms"] = [{"lamp": lamp, "role": role} for lamp, role in room_lamps_roles]
    return f


def flat_to_json(f):
    return {
        "name": f["name"],
        "building": f["building"],
        "type": f["type"],
        "weekend": f["weekend"],
        "rooms": [dict(r) for r in f["rooms"]],
        "wake": list(f["wake"]),
        "leave": list(f["leave"]),
        "home": list(f["home"]),
        "bed": list(f["bed"]),
        "outPercent": f["outPercent"],
        "tvPercent": f["tvPercent"],
        "level": f["level"],
        "fadeMs": f["fadeMs"],
        "dayActivity": f["dayActivity"],
        "nightActivity": f["nightActivity"],
    }


def flat_from_json(o, index):
    """FlatConfig::fromJson: setPreset(type) first, then present fields
    override (absent-means-preset, like groups), then clamp() (spec 2)."""
    if not isinstance(o, dict):
        raise ApiError(400, "flat must be an object")

    ftype = o.get("type", "family")
    if ftype not in FLAT_TYPES:
        raise ApiError(400, "unknown household type")
    preset = FLAT_HOUSEHOLD_PRESETS["family" if ftype == "custom" else ftype]
    f = dict(preset)
    f["type"] = ftype

    # clamp(): name non-empty, name defaults to "Flat n"; both truncated to
    # SCENE_NAME_LEN - 1 like group names.
    name = str(o.get("name", ""))[:23].strip()
    f["name"] = name if name else "Flat %d" % (index + 1)
    f["building"] = str(o.get("building", ""))[:23]
    f["weekend"] = bool(o.get("weekend", False))

    rooms_in = o.get("rooms", [])
    if not isinstance(rooms_in, list):
        raise ApiError(400, "rooms must be an array")
    # clamp(): roomCount 0..12.
    rooms = []
    for r in rooms_in[:FLAT_MAX_ROOMS]:
        if not isinstance(r, dict):
            raise ApiError(400, "room must be an object")
        lamp = r.get("lamp", 0)
        if not isinstance(lamp, int) or isinstance(lamp, bool):
            lamp = 0
        # clamp(): lamp below LAMPS_MAX_LAMPS.
        lamp = max(0, min(LAMPS_MAX_LAMPS - 1, lamp))
        role = r.get("role")
        if role not in ROOM_NAMES:
            role = "other"
        rooms.append({"lamp": lamp, "role": role})
    f["rooms"] = rooms

    for key in ("wake", "leave", "home", "bed"):
        v = o.get(key)
        if (isinstance(v, list) and len(v) == 2
                and all(isinstance(x, int) and not isinstance(x, bool) for x in v)):
            frm, to = v
            # clamp(): ranges ordered (to >= from).
            if to < frm:
                to = frm
            f[key] = (frm, to)

    for key in ("outPercent", "tvPercent"):
        v = o.get(key)
        if isinstance(v, int) and not isinstance(v, bool):
            f[key] = max(0, min(100, v))
    if isinstance(o.get("level"), int) and not isinstance(o.get("level"), bool):
        f["level"] = max(0, min(255, o["level"]))
    if isinstance(o.get("fadeMs"), int) and not isinstance(o.get("fadeMs"), bool):
        f["fadeMs"] = max(0, o["fadeMs"])
    for key in ("dayActivity", "nightActivity"):
        v = o.get(key)
        if isinstance(v, int) and not isinstance(v, bool):
            f[key] = max(0, min(3, v))

    return f


def _in_range(x, lo, hi):
    """True when x falls in [lo, hi] mod 1440; the interval may cross
    midnight (lo > hi), same semantics as the firmware's inWindow()."""
    lo %= 1440
    hi %= 1440
    x %= 1440
    if lo <= hi:
        return lo <= x <= hi
    return x >= lo or x <= hi


def _range_mid(pair):
    a, b = pair
    if a < 0 or b < 0:
        return None
    return (a + b) / 2.0


def flat_status(f, minutes, is_weekday):
    """Spec 3.5, simplified to the flat's range midpoints (no per-day draw,
    no per-room jitter): state from the simulated minute, lit letters from
    the room table (spec 3.2) evaluated at those same midpoints, plus a
    once-in-ten-minutes bathroom night visit so a slept-in flat still looks
    alive under the scrubber."""
    m = minutes % 1440

    if f["type"] == "away":
        lit = []
        if _in_range(m, 19 * 60, 22 * 60 + 30):
            roles_present = {r["role"] for r in f["rooms"]}
            # "a living or other room, if present": living wins when both
            # exist, same as the "first flat wins" ownership tie-break.
            if "living" in roles_present:
                lit.append(ROOM_LETTERS["living"])
            elif "other" in roles_present:
                lit.append(ROOM_LETTERS["other"])
        return "away", "".join(lit)

    wake_mid = _range_mid(f["wake"])
    leave_mid = _range_mid(f["leave"])
    home_mid = _range_mid(f["home"])
    bed_mid = _range_mid(f["bed"])

    asleep = (wake_mid is not None and bed_mid is not None
              and _in_range(m, bed_mid, wake_mid))
    leave_valid = f["leave"][0] >= 0 and f["leave"][1] >= 0
    is_out = (not asleep and is_weekday and leave_valid
              and leave_mid is not None and home_mid is not None
              and _in_range(m, leave_mid, home_mid))

    if asleep:
        state = "asleep"
    elif is_out:
        state = "out"
    else:
        state = "awake"

    if is_out:
        return state, ""

    # dinner = home + lerp(45, 90, u4); the mock has no daily draw, so it
    # uses the midpoint of that lerp, 45 + (90-45)/2 = 67.5.
    dinner_mid = (home_mid + 67.5) if home_mid is not None else None
    is_weekend_day = not is_weekday

    # lit is one letter per lit *role*, in the canonical ROOM_NAMES order
    # (living, kitchen, bedroom, bathroom, hall, other), not the order the
    # flat's rooms happen to be listed in; a role appears at most once even
    # if the flat has more than one room of it.
    roles_present = {r["role"] for r in f["rooms"]}
    lit = []
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
            if f["weekend"] and is_weekend_day and _in_range(m, 12 * 60, 13 * 60):
                on = True
        elif role == "hall":
            if leave_mid is not None and _in_range(m, leave_mid - 5, leave_mid + 2):
                on = True
            if home_mid is not None and _in_range(m, home_mid - 1, home_mid + 6):
                on = True
        elif role in ("living", "other"):
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
            if f["nightActivity"] > 0 and asleep and int(round(m)) % 10 == 0:
                on = True
        if on:
            lit.append(ROOM_LETTERS.get(role, ""))

    return state, "".join(lit)


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

        # The seeded example town, same ranges and names as the sketch.
        self.groups = [
            make_group("Street lights", "street", 0, 15),
            make_group("Flats", "home", 16, 95),
            make_group("Shops", "shop", 96, 119),
            make_group("Pub and grill", "late", 120, 127),
            make_group("Kiosk, church", "allnight", 128, 143),
        ]

        # The seeded example households, spec 4.4: two buildings, five flats,
        # each with kitchen/living/bedroom/bathroom/hall on lamps 16..40 (also
        # inside the "Flats" group above, since a flat-owned lamp is only a
        # /api/scene ownership detail the engine acts on, not the mock).
        self.flats = [
            make_flat("Andersson", "Storgatan 3", "family", True, [
                (16, "kitchen"), (17, "living"), (18, "bedroom"),
                (19, "bathroom"), (20, "hall"),
            ]),
            make_flat("Karlsson", "Storgatan 3", "elderly", True, [
                (21, "kitchen"), (22, "living"), (23, "bedroom"),
                (24, "bathroom"), (25, "hall"),
            ]),
            make_flat("Nilsson", "Storgatan 3", "nightowl", True, [
                (26, "kitchen"), (27, "living"), (28, "bedroom"),
                (29, "bathroom"), (30, "hall"),
            ]),
            make_flat("Persson", "Kyrkogatan 1", "family", True, [
                (31, "kitchen"), (32, "living"), (33, "bedroom"),
                (34, "bathroom"), (35, "hall"),
            ]),
            make_flat("Svensson", "Kyrkogatan 1", "away", False, [
                (36, "kitchen"), (37, "living"), (38, "bedroom"),
                (39, "bathroom"), (40, "hall"),
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
        for g in self.groups:
            lamps |= g["lamps"]
        return len(lamps)

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
        # The seeded groups cover more lamps than the seeded hardware has,
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
                "groups": [group_to_json(g) for g in self.groups],
                "flats": [flat_to_json(f) for f in self.flats],
            }

    def status_json(self):
        minutes = self.sim_minutes()
        dusk, dawn = self.dusk_dawn()
        sunset = (dusk - 25) % 1440
        sunrise = (dawn + 25) % 1440
        with self.lock:
            # Weekday, spec 3.1: tm_wday derived from the simulated day of
            # year with 2026-01-01 (doy 1) a Thursday; wday 0/6 is the
            # weekend, used by flat_status() for the "out" state.
            wday = (self.day_of_year() + 3) % 7
            is_weekday = wday not in (0, 6)
            flats_status = []
            for f in self.flats:
                state, lit = flat_status(f, minutes, is_weekday)
                flats_status.append({"name": f["name"], "state": state, "lit": lit})
            return {
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
                "groups": len(self.groups),
                "flats": flats_status,
            }

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

        new_groups = self.groups
        if "groups" in data:
            groups_in = data["groups"]
            if not isinstance(groups_in, list):
                raise ApiError(400, "groups must be an array")
            if len(groups_in) > SCENE_MAX_GROUPS:
                raise ApiError(400, "too many groups")
            new_groups = [group_from_json(o) for o in groups_in]

        new_flats = self.flats
        if "flats" in data:
            flats_in = data["flats"]
            if not isinstance(flats_in, list):
                raise ApiError(400, "flats must be an array")
            if len(flats_in) > SCENE_MAX_FLATS:
                raise ApiError(400, "too many flats")
            new_flats = [flat_from_json(o, i) for i, o in enumerate(flats_in)]

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
            self.groups = new_groups
            self.flats = new_flats
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
        out = {}
        for name in ("street", "home", "shop", "late", "allnight",
                     "family", "elderly", "nightowl", "away"):
            p = BEHAVIOUR_PRESETS[name]
            out[name] = {
                "onAnchor": p["onAnchor"],
                "on": list(p["on"]),
                "offAnchor": p["offAnchor"],
                "off": list(p["off"]),
                "litPercent": p["litPercent"],
                "flickerPercent": p["flickerPercent"],
                "morning": p["morning"],
                "level": p["level"],
                "fadeMs": p["fadeMs"],
                "dayActivity": p["dayActivity"],
                "nightActivity": p["nightActivity"],
            }
        # Households: setPreset() rhythm fields only, no "custom" entry
        # (spec 3.5: '"households": {...} (no entry for custom)').
        out["households"] = {}
        for name in ("family", "elderly", "nightowl", "away"):
            p = FLAT_HOUSEHOLD_PRESETS[name]
            out["households"][name] = {
                "wake": list(p["wake"]),
                "leave": list(p["leave"]),
                "home": list(p["home"]),
                "bed": list(p["bed"]),
                "outPercent": p["outPercent"],
                "tvPercent": p["tvPercent"],
                "level": p["level"],
                "fadeMs": p["fadeMs"],
                "dayActivity": p["dayActivity"],
                "nightActivity": p["nightActivity"],
            }
        out["rooms"] = list(ROOM_NAMES)
        return out


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
        "build": "mock",
        "uptime": int(time.time() - BOOT_TIME),
        "heap": 180000,
        "time": time.strftime("%Y-%m-%dT%H:%M:%S", now),
        "timeValid": True,
    }


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
    # does apart from the light itself: it checks the number and says so on
    # stderr, which is enough to watch the GUI's debounce from the log.
    def _identify(self, data):
        if not isinstance(data, dict):
            raise ApiError(400, "invalid JSON")
        count = LAMPS.lamp_count()
        lamp = data.get("lamp")
        if not isinstance(lamp, int) or isinstance(lamp, bool) \
                or lamp < 0 or lamp >= count:
            if count == 0:
                raise ApiError(400, "no lamps are fitted")
            raise ApiError(400, "lamp must be 0..%d" % (count - 1))
        sys.stderr.write("mock: identify lamp %d\n" % lamp)
        return {"ok": True, "lamp": lamp}

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
