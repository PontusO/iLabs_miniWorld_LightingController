# Scene Activity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add deterministic short lighting events (day, dip, night) and four household presets to the scene engine, API, mock and GUI.

**Architecture:** Two new per-group fields and four new behaviours in the model; a slot-hashed event layer in `SceneEngine` evaluated once per simulated minute into two bitmaps that `targetLevel()` reads; presets and JSON extended; the Scene view gains an Activity row and the new behaviours; the mock mirrors the API.

**Tech Stack:** arduino-pico core 5.5.1, ArduinoJson 7, vanilla JS, Python 3 standard library, GNU make.

**Spec:** `docs/superpowers/specs/2026-09-07-scene-activity-design.md`. Read it first; every number is there.

## Global Constraints

- No em dashes anywhere: code, comments, UI text, docs.
- Source file header comments end with `Invector Embedded Systems AB` (existing files keep theirs).
- Web API handlers stay `(method, path, body) -> (code, json)`.
- Nothing served from the device fetches anything external.
- Build and lint is `make` at the repo root, warning-free. Flash only through `make upload`.
- Existing behaviour numbering and JSON names are unchanged; new behaviours append.
- The engine's event decision is one pure function (spec 6).
- Commits are made by the controller at the end, not by tasks.

## File structure

| File | Responsibility |
|---|---|
| `miniWorld_LightingController/Scene.h/.cpp` (modify) | fields, behaviours, presets, JSON, clamp |
| `miniWorld_LightingController/SceneEngine.h/.cpp` (modify) | event layer, bitmaps, `active` count |
| `miniWorld_LightingController/SceneWebApi.cpp` (modify) | presets and status fields |
| `tools/mockserver.py` (modify) | presets, fields, `active` |
| `web/view-scene.js`, `web/view-scene.css` (modify) | behaviours, Activity row |
| `HANDOFF.md` x2, `CLAUDE.md` (modify) | docs and bench item |

Tasks 1 and 2 touch disjoint files and run in parallel; Task 3 follows.

---

### Task 1: Model, engine and API

**Files:** `Scene.h`, `Scene.cpp`, `SceneEngine.h`, `SceneEngine.cpp`, `SceneWebApi.cpp` in `miniWorld_LightingController/`.

**Interfaces produced:**

```cpp
// Scene.h
enum class Behaviour : uint8_t { Off = 0, Street, Home, Shop, Late, AllNight,
                                 Family, Elderly, NightOwl, Away, COUNT };
struct GroupConfig { ...existing...; uint8_t dayActivity; uint8_t nightActivity; };
// SceneEngine.h
uint16_t activeCount() const;          // lamps currently in an event
```

- [ ] **Step 1: Model.** Add the enum values and names (`family`, `elderly`, `nightowl`, `away`), the two fields, `setPreset()` values from spec section 2 (table), `clamp()` limits (0..3), `toJson` fields, `fromJson`: after `setPreset(behaviour)` is applied to a fresh group (check how `fromJson` builds a group today and keep that order), read `dayActivity`/`nightActivity` only if present.
- [ ] **Step 2: Engine.** In `SceneEngine.h` add `_eventOn`, `_eventOff` bitmaps, `_active`, `_eventSim`, `_eventDoy`, and the pure function declaration:

```cpp
// Which event, if any, covers minute m for this lamp. kind: 0 day, 1 dip, 2 night.
// Returns true when an event of that kind is active at m.
bool eventAt(uint16_t lamp, int m, uint16_t doy, uint8_t kind, float rate,
             int windowFrom, int windowTo) const;
```

  In `SceneEngine.cpp`: `w(t)` table, rate tables, the slot hash exactly as spec 3.3, the four-slot scan with midnight wrap (previous day's slots use `doy - 1`, or 366 when doy is 1), lengths per kind, cap 0.5. `targetLevel()` gets two extra outputs (the computed `on` and `off` moments and whether the morning light applied) through reference parameters or a small struct; keep its existing logic byte for byte otherwise. A new `evaluateEvents()` runs when `_sim` or `_doy` changed since the last evaluation: for each lamp in a group with any activity, compute base state through `targetLevel()`, determine the night window (spec 3.2), pick the applicable kind, call `eventAt()`, and set the bit in `_eventOn` (base off, event on) or `_eventOff` (base on, dip). Count `_active`. `tick()` calls `evaluateEvents()` after `updateClock()`; in the per-lamp loop, an `_eventOn` bit overrides `base` to `G.level * 257` (no flicker), an `_eventOff` bit overrides it to 0. `rebuild()` clears the bitmaps and forces re-evaluation.
- [ ] **Step 3: API.** `getPresets` adds the two fields; `statusJson` adds `"active"`. Update the header comment in `SceneWebApi.h` (status object) and `Scene.h` (JSON contract).
- [ ] **Step 4: Build.** `make`, warning-free. Report globals (expect +512 bytes).
- [ ] **Step 5: Self-check by reasoning.** Write in the report, for one lamp and one slot, the numbers: hash inputs, `u`, `p`, offset, length, and the resulting interval, showing the decision matches spec 3.3.

---

### Task 2: Mock and GUI

**Files:** `tools/mockserver.py`, `web/view-scene.js`, `web/view-scene.css`.

**Interfaces consumed:** the JSON names above; presets route shape.

- [ ] **Step 1: Mock.** `BEHAVIOUR_PRESETS` gains the four households with the spec table values and `dayActivity`/`nightActivity` on every entry; group creation and PUT accept the fields with absent-means-preset; `status_json` adds `active` (0 between 23:00 and 05:30, else 1..3 from a hash of the minute). `tools/apicheck.sh` unchanged.
- [ ] **Step 2: GUI.** `BEHAVIOURS` list and labels per spec 5; `PRESET_KEYS` gains the two fields; the Activity row with two selects and the sentence; re-seed on behaviour change; Add group seeds from `home`. CSS for the row under `.view-scene`.
- [ ] **Step 3: Check against the mock** on port 8092: presets carry the fields; switch a group to Elderly and see Night wake-ups become Often and the sentence update; Save and reload keeps both; a patched config without the fields loads with preset values. Screenshots at 390 px of the expanded group (CDP driver in the scratchpad, or write one). `python3 tools/buildweb.py web /tmp/x.h` prints the gzipped size, must stay under 40960.

---

### Task 3: Docs

**Files:** `HANDOFF.md` and `miniWorld_LightingController/HANDOFF.md` (identical), `CLAUDE.md`.

- [ ] Section 2 of HANDOFF: one line in the scene band mentioning the activity layer. Section 3: the `Scene`/`SceneEngine` rows mention events and households. Section 5: a new bench item with the scrub test from spec 6. Section 8: one decision paragraph on hash-per-slot determinism. CLAUDE.md architecture: one bullet on the event bitmaps and the once-per-minute evaluation.

---

### Task 4: Flash and bench (controller)

`make upload`, then the spec 6 scrub test through the GUI at 192.168.1.180, then commit and push.
