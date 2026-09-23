#!/usr/bin/env python3
"""
test_mockserver.py - unit tests for mockserver.py's models and units, spec
                     2026-09-22-models-and-units-design.md sections 2, 3.3,
                     4.5, 5, 6 and 9.

Each test builds its own mockserver.SceneState() so tests never share the
seeded fixture's mutations. Run with:

    python3 -m unittest tools/test_mockserver.py -v      (from the repo root)
    python3 -m unittest test_mockserver -v                (from tools/)

Invector Embedded Systems AB
"""

import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mockserver  # noqa: E402


class TemplatesTest(unittest.TestCase):
    def test_templates_present(self):
        state = mockserver.SceneState()
        presets = state.presets_json()
        models = presets["models"]
        self.assertEqual(
            set(models.keys()),
            {"family", "elderly", "nightowl", "away",
             "home", "shop", "pub", "street", "allnight"},
        )
        self.assertEqual(len(models), 9)
        self.assertEqual(
            set(presets["rooms"]),
            {"living", "kitchen", "bedroom", "bathroom", "hall",
             "front", "back", "sign", "other"},
        )
        self.assertEqual(len(presets["rooms"]), 9)


class ConfigRoundTripTest(unittest.TestCase):
    def test_config_round_trip(self):
        state = mockserver.SceneState()
        doc = {
            "models": [
                {"name": "Retirees", "kind": "rhythm"},
                {"name": "Corner shop", "kind": "hours", "individual": False},
            ],
            "units": [
                {"name": "Old folks", "building": "Bay St", "model": "Retirees",
                 "rooms": [{"lamps": [0, "4-15"], "role": "living"}]},
                {"name": "Shop unit", "building": "", "model": "Corner shop",
                 "rooms": [{"lamps": [20], "role": "front"}]},
            ],
        }
        state.apply_config(doc)
        out = state.config_json()

        self.assertEqual({m["name"] for m in out["models"]}, {"Retirees", "Corner shop"})
        self.assertEqual({u["name"] for u in out["units"]}, {"Old folks", "Shop unit"})

        old_folks = next(u for u in out["units"] if u["name"] == "Old folks")
        self.assertEqual(old_folks["model"], "Retirees")
        self.assertEqual(old_folks["rooms"][0]["lamps"], [0, "4-15"])

        corner = next(m for m in out["models"] if m["name"] == "Corner shop")
        self.assertEqual(corner["kind"], "hours")
        self.assertEqual(corner["individual"], False)


class RejectionTest(unittest.TestCase):
    def test_unknown_model_rejected(self):
        state = mockserver.SceneState()
        doc = {"units": [{"name": "X", "building": "", "model": "Nosuch", "rooms": []}]}
        with self.assertRaises(mockserver.ApiError) as cm:
            state.apply_config(doc)
        self.assertEqual(cm.exception.code, 400)
        self.assertEqual(cm.exception.message, "unknown model")

    def test_duplicate_model_name_rejected(self):
        state = mockserver.SceneState()
        doc = {"models": [
            {"name": "Dup", "kind": "hours"},
            {"name": "dup", "kind": "hours"},
        ]}
        with self.assertRaises(mockserver.ApiError) as cm:
            state.apply_config(doc)
        self.assertEqual(cm.exception.code, 400)
        self.assertEqual(cm.exception.message, "duplicate model name")

    def test_too_many_ranges_rejected(self):
        state = mockserver.SceneState()
        # Nine lamps, none touching its neighbour, so none of them merge
        # into a shared range: nine ranges in one room.
        lamps = [0, 2, 4, 6, 8, 10, 12, 14, 16]
        doc = {
            "models": [{"name": "M", "kind": "hours"}],
            "units": [{"name": "U", "building": "", "model": "M",
                       "rooms": [{"lamps": lamps, "role": "other"}]}],
        }
        with self.assertRaises(mockserver.ApiError) as cm:
            state.apply_config(doc)
        self.assertEqual(cm.exception.code, 400)
        self.assertEqual(cm.exception.message, "too many ranges")


class MigrationTest(unittest.TestCase):
    def test_migration_flats_first(self):
        state = mockserver.SceneState()
        doc = {
            "flats": [
                {"name": "Andersson", "building": "Storgatan 3", "type": "family",
                 "rooms": [{"lamps": [16], "role": "kitchen"}]},
                {"name": "Weird", "building": "Storgatan 3", "type": "custom",
                 "wake": [100, 110],
                 "rooms": [{"lamps": [17], "role": "other"}]},
            ],
            "groups": [
                {"name": "Street", "behaviour": "street", "lamps": [0, 1, 2]},
            ],
        }
        state.apply_config(doc)
        out = state.config_json()

        unit_names = [u["name"] for u in out["units"]]
        self.assertEqual(unit_names, ["Andersson", "Weird", "Street"])

        andersson = out["units"][0]
        self.assertEqual(andersson["model"], "Family")
        weird = out["units"][1]
        self.assertEqual(weird["model"], "Weird")
        street = out["units"][2]
        self.assertEqual(street["model"], "Street")

        model_names = {m["name"] for m in out["models"]}
        self.assertIn("Family", model_names)
        self.assertIn("Weird", model_names)
        self.assertIn("Street", model_names)

    def test_migration_retired_behaviour(self):
        state = mockserver.SceneState()
        doc = {"groups": [{"name": "Oldgroup", "behaviour": "elderly", "lamps": [5]}]}
        state.apply_config(doc)
        out = state.config_json()

        model = next(m for m in out["models"] if m["name"] == "Oldgroup")
        self.assertEqual(model["kind"], "hours")
        # The Home template's hours half, per the retired-name migration rule.
        self.assertEqual(model["onAnchor"], "dusk")
        self.assertEqual(model["on"], [0, 240])
        self.assertEqual(model["offAnchor"], "clock")
        self.assertEqual(model["off"], [1320, 1470])
        self.assertEqual(model["litPercent"], 85)
        self.assertEqual(model["individual"], True)

    def test_migration_of_an_empty_old_scene_seeds(self):
        # An old board that was never set up stored {"groups": [], "flats":
        # []}. It takes the migration branch and nothing crosses over, so
        # the nine templates go in rather than an empty Models tab.
        state = mockserver.SceneState()
        state.apply_config({"groups": [], "flats": []})
        out = state.config_json()
        self.assertEqual(len(out["models"]), 9)
        self.assertEqual(out["models"][0]["name"], "Family")
        self.assertEqual(out["units"], [])

    def test_migration_off_group_keeps_its_level(self):
        # litPercent 0 is what keeps an Off group dark. The level stays the
        # template's, so raising litPercent gives a usable model.
        state = mockserver.SceneState()
        state.apply_config({"groups": [{"name": "Dark", "behaviour": "off", "lamps": [5]}]})
        out = state.config_json()
        model = next(m for m in out["models"] if m["name"] == "Dark")
        self.assertEqual(model["litPercent"], 0)
        self.assertGreater(model["level"], 0)


class StatusTest(unittest.TestCase):
    def test_status_units(self):
        state = mockserver.SceneState()
        status = state.status_json()

        self.assertIn("units", status)
        self.assertNotIn("groups", status)
        self.assertNotIn("flats", status)

        valid_states = {"asleep", "out", "awake", "away", "open", "closed", "lit", "dark"}
        valid_letters = set("lkbthfrso")
        self.assertTrue(status["units"])
        for u in status["units"]:
            self.assertIn("name", u)
            self.assertIn(u["state"], valid_states)
            self.assertTrue(set(u["lit"]) <= valid_letters)

    def test_hours_state_open_closed(self):
        state = mockserver.SceneState()
        dusk, dawn = 19 * 60 + 58, 5 * 60 + 47

        shop_unit = next(u for u in state.units if u["model"] == "Shop")
        shop_model = mockserver.MODEL_TEMPLATES["shop"]
        noon, _ = mockserver.unit_status(shop_unit, shop_model, 12 * 60, True, dusk, dawn)
        self.assertEqual(noon, "open")
        night, _ = mockserver.unit_status(shop_unit, shop_model, 22 * 60, True, dusk, dawn)
        self.assertEqual(night, "closed")

        # Anchored to dusk and dawn, so the words are lit and dark and not
        # open and closed, and with dusk at 19:58 both are decided: 22:00 is
        # inside the window and noon is not.
        street_unit = next(u for u in state.units if u["model"] == "Street light")
        street_model = mockserver.MODEL_TEMPLATES["street"]
        night, _ = mockserver.unit_status(street_unit, street_model, 22 * 60, True, dusk, dawn)
        self.assertEqual(night, "lit")
        day, _ = mockserver.unit_status(street_unit, street_model, 12 * 60, True, dusk, dawn)
        self.assertEqual(day, "dark")

    def test_hours_state_nobody_takes_part(self):
        # litPercent 0, which is what an old Off group migrates to: clock
        # windows of [0, 0] and [0, 0], which would otherwise read as open
        # the whole day.
        state = mockserver.SceneState()
        dusk, dawn = 19 * 60 + 58, 5 * 60 + 47
        unit = next(u for u in state.units if u["model"] == "Shop")
        model = dict(mockserver.MODEL_TEMPLATES["shop"])
        model["litPercent"] = 0
        model["onAnchor"] = "clock"
        model["offAnchor"] = "clock"
        model["on"] = (0, 0)
        model["off"] = (0, 0)
        for minute in (0, 12 * 60, 22 * 60):
            word, letters = mockserver.unit_status(unit, model, minute, True, dusk, dawn)
            self.assertEqual(word, "closed")
            self.assertEqual(letters, "")

        model["onAnchor"] = "dusk"
        word, _ = mockserver.unit_status(unit, model, 22 * 60, True, dusk, dawn)
        self.assertEqual(word, "dark")


class LoadErrorTest(unittest.TestCase):
    """SceneEngine::loadError(), reported by /api/scene/status only when a
    stored scene existed at boot and would not load."""

    def test_status_has_no_load_error_by_default(self):
        state = mockserver.SceneState()
        self.assertIsNone(state.load_error)
        self.assertNotIn("loadError", state.status_json())

    def test_status_carries_the_load_error(self):
        state = mockserver.SceneState()
        state.load_error = "invalid JSON: IncompleteInput"
        self.assertEqual(state.status_json()["loadError"],
                         "invalid JSON: IncompleteInput")


class ClampUnitsTest(unittest.TestCase):
    """clamp_units() mirrors dedupUnitRooms(), which works inside one unit.
    Two units overlapping is left alone here and settled by rebuild(), which
    gives the lamp to the first unit that lists it."""

    def test_overlap_between_units_is_kept(self):
        units = [
            {"name": "A", "building": "", "model": "Home",
             "rooms": [{"ranges": [(0, 3)], "role": "other"}]},
            {"name": "B", "building": "", "model": "Home",
             "rooms": [{"ranges": [(2, 5)], "role": "other"}]},
        ]
        mockserver.clamp_units(units)
        self.assertEqual(units[0]["rooms"][0]["ranges"], [(0, 3)])
        self.assertEqual(units[1]["rooms"][0]["ranges"], [(2, 5)])
        self.assertEqual(mockserver.unit_to_json(units[1])["rooms"][0]["lamps"],
                         ["2-5"])

    def test_overlap_inside_one_unit_is_removed(self):
        units = [
            {"name": "A", "building": "", "model": "Home",
             "rooms": [{"ranges": [(0, 3)], "role": "other"},
                       {"ranges": [(2, 5)], "role": "living"}]},
        ]
        mockserver.clamp_units(units)
        self.assertEqual(units[0]["rooms"][0]["ranges"], [(0, 3)])
        self.assertEqual(units[0]["rooms"][1]["ranges"], [(4, 5)])


def _md5(data):
    return hashlib.md5(data).hexdigest()


def _image(stamp, size=120000, banner=True, stamp_at=None):
    """A synthetic firmware body: filler, the banner format string and the
    build stamp as C strings. stamp_at places the stamp at a byte offset so
    a test can straddle the scanner's 4096-byte chunk boundary."""
    banner_bytes = mockserver.FIRMWARE_BANNER + b"\0" if banner else b""
    stamp_bytes = stamp.encode() + b"\0" if stamp else b""
    if stamp_at is None:
        head = b"\x7f" * 1000
        body = head + banner_bytes + stamp_bytes
    else:
        body = b"\x7f" * stamp_at + stamp_bytes + banner_bytes
    return body + b"\x7f" * (size - len(body))


class FirmwareTest(unittest.TestCase):
    STAMP = "Sep 23 2026 10:00:00"

    def test_info_shape(self):
        fw = mockserver.FirmwareState()
        info = fw.info_json()
        self.assertEqual(set(info), {"fsTotal", "fsFree", "maxSize", "minSize", "staged"})
        self.assertEqual(info["minSize"], 100000)
        self.assertLessEqual(info["maxSize"], info["fsFree"] - 65536)
        self.assertLessEqual(info["maxSize"], 1048576)
        self.assertFalse(info["staged"])

    def test_rejects_bad_headers(self):
        fw = mockserver.FirmwareState()
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.check(120000, "nothex", self.STAMP)
        self.assertEqual(cm.exception.code, 400)
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.check(120000, "0" * 32, "")
        self.assertEqual(cm.exception.code, 400)

    def test_rejects_outside_band(self):
        fw = mockserver.FirmwareState()
        for length in (0, 16, 99999, 2000000):
            with self.assertRaises(mockserver.ApiError) as cm:
                fw.check(length, "0" * 32, self.STAMP)
            self.assertEqual(cm.exception.code, 413, length)
        fw.check(100000, "0" * 32, self.STAMP)   # the lower bound itself passes

    def test_rejects_md5_mismatch(self):
        fw = mockserver.FirmwareState()
        body = _image(self.STAMP)
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.upload(body, "0" * 32, self.STAMP)
        self.assertEqual(cm.exception.code, 422)
        self.assertEqual(cm.exception.message, "md5 mismatch")
        self.assertEqual(fw.build, "mock")
        self.assertFalse(fw.staged)

    def test_rejects_foreign_image(self):
        fw = mockserver.FirmwareState()
        body = _image(self.STAMP, banner=False)
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.upload(body, _md5(body), self.STAMP)
        self.assertEqual(cm.exception.code, 422)
        self.assertEqual(cm.exception.message, "not this sketch")

    def test_rejects_missing_stamp(self):
        fw = mockserver.FirmwareState()
        body = _image("Sep 01 2026 00:00:00")
        with self.assertRaises(mockserver.ApiError) as cm:
            fw.upload(body, _md5(body), self.STAMP)
        self.assertEqual(cm.exception.code, 422)
        self.assertEqual(cm.exception.message, "build stamp not in image")

    def test_accepts_and_reports_build(self):
        fw = mockserver.FirmwareState()
        body = _image(self.STAMP)
        result = fw.upload(body, _md5(body), self.STAMP)
        self.assertEqual(result, {"ok": True, "size": len(body),
                                  "md5": _md5(body), "build": self.STAMP})
        self.assertEqual(fw.build, self.STAMP)
        self.assertFalse(fw.staged)   # the mock "reboots" at once

    def test_accepts_uppercase_md5(self):
        fw = mockserver.FirmwareState()
        body = _image(self.STAMP)
        result = fw.upload(body, _md5(body).upper(), self.STAMP)
        self.assertEqual(result["md5"], _md5(body))

    def test_finds_stamp_across_chunk_boundary(self):
        fw = mockserver.FirmwareState()
        # The stamp starts 5 bytes before the first 4096-byte boundary.
        body = _image(self.STAMP, stamp_at=4096 - 5)
        result = fw.upload(body, _md5(body), self.STAMP)
        self.assertEqual(result["build"], self.STAMP)


if __name__ == "__main__":
    unittest.main()
