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

        street_unit = next(u for u in state.units if u["model"] == "Street light")
        street_model = mockserver.MODEL_TEMPLATES["street"]
        state_word, _ = mockserver.unit_status(street_unit, street_model, 22 * 60, True, dusk, dawn)
        self.assertIn(state_word, ("lit", "dark"))


if __name__ == "__main__":
    unittest.main()
