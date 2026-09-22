# Models and Units Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace groups, flats and household types with two constructs: a model (a named way of behaving, no lamps) and a unit (a thing on the layout with a building, a model and rooms of lamp ranges), with a Models tab, a slimmer Scene tab, a model selector on Houses, and a loader that migrates today's scene document.

**Architecture:** `ModelConfig` carries both simulations behind a `kind` field; `UnitConfig` owns rooms of lamp ranges and points at a model by index. `SceneEngine` keeps its two paths (habit per key, rhythm per room) and picks one per unit from the model's kind and `individual` flag, keyed by lamp index or room key. `fromJson()` reads the new shape and migrates the old one. The mock mirrors all of it and grows a unit test file. Three views change and one is new.

**Tech Stack:** arduino-pico core, ArduinoJson 7, vanilla JS, Python 3 (unittest), GNU make.

**Spec:** `docs/superpowers/specs/2026-09-22-models-and-units-design.md`. Every number, name and rule is there; section 9 has the implementation notes this plan relies on. Read it first.

## Global Constraints

- No em dashes anywhere. Every source file's header comment ends with `Invector Embedded Systems AB`.
- `make` is the lint step and must be warning-free with `--warnings default`. Nothing firmware-side runs off target.
- Every `SceneConfig`, `ModelConfig`, `UnitConfig` object is static or a member, never on the stack (8 kB core-0 stack).
- Web API handlers are pure functions of (method, path, body). Nothing external fetched by the UI. Whole page under 40 kB gzipped; `make web` prints the size.
- Levels are `uint16_t` through the driver interface; the scene scales `level * 257` as today.
- `LAMPS_MAX_LAMPS` stays 2048 and every per-lamp array in the engine stays sized by it.
- Flash only through `make upload`, and only the Challenger NB 2040 WiFi (USB id 2e8a:100d, `/dev/ttyACM2` at the time of writing). Other boards on the bench are off limits.
- The controller commits after each task, never the subagent. Nothing is pushed.

## File map

| File | Responsibility after this change |
|---|---|
| `miniWorld_LightingController/Scene.h/.cpp` | `ModelKind`, `Template`, `Room`, `LampRange`, `RoomConfig`, `ModelConfig`, `UnitConfig`, `SceneConfig` with `models[]` and `units[]`; names; templates; clamp; JSON; migration |
| `miniWorld_LightingController/SceneEngine.h/.cpp` | `_lampUnit[]`, `UnitDay`, `evaluateUnits()`, `evaluateUnitEvents()`, habit path keyed by lamp or room, `unitState()`, `unitLitRooms()`, `unitOwner()` |
| `miniWorld_LightingController/SceneWebApi.cpp` | status with `units`, presets with `models` and `rooms` |
| `miniWorld_LightingController/miniWorld_LightingController.ino` | first-boot seeding: nine template models, no units |
| `tools/mockserver.py` | `MODEL_TEMPLATES`, `models`, `units`, migration, status, presets |
| `tools/test_mockserver.py` | unittest for the mock's loader, migration, status and presets |
| `tools/apicheck.sh` | presets and config checks for the new shape |
| `web/view-scene.js/.css` | clock and location only, seed in the location card |
| `web/view-models.js/.css` | new: the Models table and the two editors |
| `web/view-houses.js/.css` | units with a model selector, new roles, no rhythm editor |
| `web/view-home.js/.css` | chips for every unit, four new states |
| `web/index.html` | sixth nav link |
| `HANDOFF.md` (two identical copies), `CLAUDE.md`, `README.md` | documentation |

## Tasks

### Task 1: Firmware model, engine, API

Files: `Scene.h`, `Scene.cpp`, `SceneEngine.h`, `SceneEngine.cpp`, `SceneWebApi.cpp`, `miniWorld_LightingController.ino`, all under `miniWorld_LightingController/`.

Interfaces produced (later tasks and the mock mirror these names exactly):

```cpp
#define SCENE_MAX_MODELS   16
#define SCENE_MAX_UNITS    32
#define UNIT_MAX_ROOMS     12
#define ROOM_MAX_RANGES     8

enum class ModelKind : uint8_t { Rhythm = 0, Hours, COUNT };          // "rhythm", "hours"
enum class Template  : uint8_t { Family = 0, Elderly, NightOwl, Away,
                                 Home, Shop, Pub, Street, AllNight, COUNT };
// JSON keys: family elderly nightowl away home shop pub street allnight
// Model names: Family, Elderly couple, Night owl, Away, Home, Shop, Pub,
//              Street light, All night
enum class Room : uint8_t { Living = 0, Kitchen, Bedroom, Bathroom, Hall,
                            Front, Back, Sign, Other, COUNT };
// JSON: living kitchen bedroom bathroom hall front back sign other
// Letters: l k b t h f r s o

struct LampRange { uint16_t from, to; };                 // inclusive

struct RoomConfig {
    LampRange ranges[ROOM_MAX_RANGES];
    uint8_t rangeCount;
    Room role;
    bool has(uint16_t lamp) const;
    bool add(uint16_t from, uint16_t to);               // false when full
    uint16_t count() const;                              // lamps, all ranges
    uint16_t lamp(uint16_t i) const;                     // i-th lamp, walks ranges
    uint16_t first() const;                              // 0xFFFF when empty
};

struct ModelConfig {
    char name[SCENE_NAME_LEN];
    ModelKind kind;
    uint8_t level; uint16_t fadeMs;
    uint8_t dayActivity, nightActivity;
    // rhythm
    bool weekend;
    int16_t wakeFrom, wakeTo, leaveFrom, leaveTo, homeFrom, homeTo, bedFrom, bedTo;
    uint8_t outPercent, tvPercent;
    // hours
    Anchor onAnchor; int16_t onFrom, onTo;
    Anchor offAnchor; int16_t offFrom, offTo;
    uint8_t litPercent, flickerPercent;
    bool morning;
    bool individual;
    void setTemplate(Template t);                        // every field, both halves
    bool isAway() const;                                 // rhythm with no wake and no bed
};

struct UnitConfig {
    char name[SCENE_NAME_LEN];
    char building[SCENE_NAME_LEN];
    uint8_t model;                                       // index into models[]
    uint8_t roomCount;
    RoomConfig rooms[UNIT_MAX_ROOMS];
};

struct SceneConfig {
    ... location, clock, seed as today ...
    uint8_t modelCount; ModelConfig models[SCENE_MAX_MODELS];
    uint8_t unitCount;  UnitConfig  units[SCENE_MAX_UNITS];
    void seedTemplates();                                // the nine, no units
    int findModel(const char *name) const;               // -1 when absent, case-insensitive
    void clamp(); void toJson(String &) const; bool fromJson(const String &, String *);
    static const char *kindName(ModelKind); static bool parseKind(const char *, ModelKind &);
    static const char *templateKey(Template); static const char *templateTitle(Template);
    static bool parseTemplate(const char *, Template &);
    static const char *roomName(Room); static bool parseRoom(const char *, Room &);
    static const char *anchorName(Anchor); static bool parseAnchor(const char *, Anchor &);
    ... modeName, parseMode, parseTime, formatTime as today ...
};
```

Engine surface:

```cpp
class SceneEngine {
    const char *unitState(uint8_t unit) const;           // asleep out awake away open closed lit dark
    void unitLitRooms(uint8_t unit, char *out, size_t len) const;   // len >= Room::COUNT + 1
    uint8_t unitOwner(uint16_t lamp) const;              // 0xFF when no unit lists it
    ... identify, clock, counts as today ...
};
```

- [ ] **Model.** In `Scene.h` and `Scene.cpp` replace `Behaviour`, `Household`, `GroupConfig`, `FlatConfig` and the old `RoomConfig` with the structures above. Delete `retiredBehaviourNames`, `behaviourNames`, `householdNames`, `GroupConfig::setPreset`, `FlatConfig::setPreset`. Write `ModelConfig::setTemplate()` from spec 2.3: the four rhythm templates take the values today's `FlatConfig::setPreset()` gives for the matching household, the five hours templates take today's `GroupConfig::setPreset()` values for the matching behaviour (Home from Home, Shop from Shop, Pub from Late, Street from Street, AllNight from AllNight); `individual` is true for Home and false for the other four; a rhythm template fills the hours half with the Home values and an hours template fills the rhythm half with the Family values, so every field is defined whatever the kind. `isAway()` is `kind == Rhythm && wakeFrom < 0 && bedFrom < 0`. `seedTemplates()` sets `modelCount = 9`, each model `setTemplate(t)` with `name = templateTitle(t)`, and `unitCount = 0`.
- [ ] **RoomConfig.** `add(from, to)` rejects `from > to` and `to >= LAMPS_MAX_LAMPS` and a full room. `count()` sums `to - from + 1`. `lamp(i)` walks the ranges. `first()` returns `ranges[0].from` or 0xFFFF. Update the file comment in `Scene.h` with the new JSON from spec 2.2, 2.4 and 2.5.
- [ ] **clamp().** Per model: `level` unchanged (uint8_t), `fadeMs <= 60000`, activity `<= 3`, percents `<= 100`, rhythm ranges on -1..2879 with `to >= from` exactly as today's flat clamp, hours windows on -720..2879 as today's group clamp. Per unit: `model < modelCount` else the unit is dropped; per room: drop any range whose lamps all appear in an earlier room of the same unit or an earlier range of the same room (today's dedup, done on ranges: a lamp already seen shrinks or splits the range; if a range would split into more pieces than fit, the tail is dropped); a room with no ranges left is dropped; a unit keeps existing with zero rooms. Names: a model whose name is empty gets `templateTitle(Home)`; a duplicate model name (case-insensitive) gets a number suffix ` 2`, ` 3` in `SCENE_NAME_LEN`.
- [ ] **toJson().** Spec 2.5. A model writes every common field, then the rhythm half when `kind == Rhythm` and the hours half when `kind == Hours`. A unit writes `name`, `building`, `model` (the model's name) and `rooms`, each room `{"lamps": [...], "role": "..."}` using the existing `runToJson()` so `[0, "4-15"]` comes out as it went in. Windows as two-element arrays as today.
- [ ] **fromJson(), new shape.** `models` array: each object needs `name` (error `model name required` when empty), `kind` (error `unknown model kind`), then optional fields over the Home template for hours and the Family template for rhythm (so an absent field means the template's value). `unknown model kind`, `too many models`, `duplicate model name`. `units` array: `model` by name via `findModel()` (error `unknown model`), rooms with the existing range syntax through a new `roomRangesFromJson()` that fills `ranges[]` directly (error `too many ranges` past eight), `role` via `parseRoom()`, `too many units`, `too many rooms in a unit`. Reject a save that removes a model still referenced: this is covered by `unknown model` since units reference by name.
- [ ] **fromJson(), migration.** When the document has no `models` key and has `groups` or `flats`, build `next.models` and `next.units` from them per spec 5, in this order. (1) Flats: for each flat, `setTemplate()` for its type (custom uses Family), apply its fields as today's flat loader does, then compare the rhythm half and level, fade, activity against the template; equal means the unit references the template model (created on first use, by title), otherwise a rhythm model named after the flat is appended. The unit takes name, building and rooms; each room's lamp list is converted to ranges through `runToJson`-style run detection (consecutive lamps become one range). (2) Groups: parse as today's group loader does (retired behaviour names map to Home), then append an hours model named after the group with `individual = true` and the group's fields; if the group has lamps, append a unit of the same name, empty building, rooms of role `other` holding the bitmap's runs, eight per room. `Off` becomes an hours model with `litPercent = 0`. (3) Name collisions get numbers via the same clamp rule. Keep the old parsers as `static` helpers in `Scene.cpp` named `migrateFlat()` and `migrateGroup()` so the new-shape loader stays readable. A document that has neither `models` nor `groups` nor `flats` leaves the models and units as they were, as today.
- [ ] **Engine.** In `SceneEngine.h` and `.cpp`: rename `FlatDay` to `UnitDay` and `_flatDay` to `_unitDay[SCENE_MAX_UNITS]`; replace `_lampGroup` and `_lampFlat` by `uint8_t _lampUnit[LAMPS_MAX_LAMPS]`; rename `_flatLit`/`_flatTv` to `_unitLit`/`_unitTv`. `rebuild()` fills `_lampUnit` from units in order, first wins, walking `RoomConfig::count()`/`lamp(i)`. Change `lampMoments()`, `targetLevel()` and `dayUnit()` to take `uint32_t key` and a `const ModelConfig &M`; the `Behaviour::Off` test becomes `M.litPercent == 0`. `weekendWakeShift()` and `weekendOutPercent()` take a `const ModelConfig &` and use spec 9.1. `computeFlatDay()` becomes `computeUnitDay()` and reads the model through the unit; `Household::Away` tests become `M.isAway()`. `evaluateFlats()` becomes `evaluateUnits()`: rhythm units as today; hours units with `individual == false` draw once per room with `targetLevel(roomKey(u, r), M, ...)` into `_unitLit` and `_unitTv`; hours units with `individual == true` are skipped here. `evaluateEvents()` loops lamps whose unit's model is hours and individual, keyed by lamp, as today's group loop; then `evaluateUnitEvents(m)` does rhythm units as today's `evaluateFlatEvents()` and non-individual hours units per room with the room key, the room rates of spec 9.2, and the night window from the room's `LampMoments` as the group loop derives it. `tick()`'s per-lamp path: no unit, skip; rhythm or non-individual hours, two bit tests; individual hours, `targetLevel(lamp, M, ...)`; level and fade from the model.
- [ ] **Status.** `unitState()`: rhythm as today's `flatState()` with `isAway()`; hours per spec 9.3. `unitLitRooms()` per spec 9.4 with letters `lkbthfrso`. `unitOwner()` replaces `flatOwner()`. Update `identify()`'s saved-level test to `_lampUnit[lamp] != 0xFF`.
- [ ] **API.** `getStatus()`: drop `groups`, replace `flats` with `units` (name, state, lit). `getPresets()`: `models` object keyed by template key, each a complete model object as `toJson()` writes it with `name` set to the title; `rooms` array of nine names; drop the per-behaviour keys and `households`. `putConfig()` unchanged.
- [ ] **Sketch.** In `.ino`, `sceneEmpty` becomes `Scene.config().modelCount == 0`; seeding becomes `s.seedTemplates(); Scene.apply(s, true); Serial.println("scene: seeded the nine models");`. No example units.
- [ ] **Header comments.** `Scene.h`, `SceneEngine.h` and `SceneWebApi.cpp` describe models and units, not groups and flats. `SceneEngine.h` keeps the paragraph on the room key.
- [ ] `make`. Warning-free. Report the flash and RAM lines; RAM must be at or under 39 %. If over, drop `SCENE_MAX_UNITS` to 24 and say so.

### Task 2: Mock and its tests

Files: `tools/mockserver.py`, `tools/test_mockserver.py` (new), `tools/apicheck.sh`, `Makefile`.

Interfaces consumed: the JSON of spec 2 and 6, the error strings of Task 1.

- [ ] **Tests first.** Write `tools/test_mockserver.py` with `unittest`, importing `mockserver` from the same directory. Tests, each a method: `test_templates_present` (nine keys in `presets_json()["models"]`, `rooms` has nine names); `test_config_round_trip` (a document with one rhythm model, one hours model with `individual` false, and two units survives `apply_config()` then `config_json()` with `model` names intact and `lamps` as `[0, "4-15"]`); `test_unknown_model_rejected` (`ApiError` 400 `unknown model`); `test_duplicate_model_name_rejected`; `test_too_many_ranges_rejected` (nine ranges in a room); `test_migration_flats_first` (a document with today's `groups` and `flats` keys and no `models`: units come out with the flats first in order, the Anderssons on the `Family` model, a custom flat on a model named after it, then a unit per group with lamps, and a model per group); `test_migration_retired_behaviour` (`"behaviour": "elderly"` on a group becomes a Home-valued hours model); `test_status_units` (status has `units` with `state` in the eight words and `lit` letters from `lkbthfrso`, no `groups` key); `test_hours_state_open_closed` (a clock-anchored shop unit at 12:00 is `open`, at 22:00 `closed`; a dusk-anchored street unit is `lit` or `dark`).
- [ ] Run: `python3 -m unittest tools/test_mockserver.py -v`. Expected: every test fails with `AttributeError` or `KeyError`, none errors on import.
- [ ] **Mock.** Replace `BEHAVIOUR_PRESETS`, `RETIRED_BEHAVIOURS`, `FLAT_TYPES`, `FLAT_HOUSEHOLD_PRESETS`, `make_group`, `group_to_json`, `group_from_json`, `make_flat`, `flat_to_json`, `flat_from_json`, `flat_status` with: `MODEL_TEMPLATES` (nine dicts keyed by template key, each with `name` set to the title and every field of both halves), `ROOM_NAMES` and `ROOM_LETTERS` with nine entries, `make_model(key, **overrides)`, `model_to_json`, `model_from_json`, `make_unit(name, building, model, rooms)`, `unit_to_json`, `unit_from_json(o, models)`, `unit_status(u, model, minutes, is_weekday, dusk, dawn)`, `migrate_old(data)` returning `(models, units)` from `groups`/`flats`. Keep `compress_lamps`/`expand_lamps`; a room stores its lamps as a list of `(from, to)` tuples and `expand_lamps` output is run-detected into at most eight, error `too many ranges`. `SceneState` holds `self.models` and `self.units`; the fixture is spec 4.5 (nine templates, Andersson, Karlsson, Nilsson, Persson, Svensson as today on the four rhythm templates, `Ica Nära` on Shop with front `[96, 97]`, back `[98]`, sign `[99]` at Storgatan 3, `Main street` on Street light with one room `other` `[0-15]` and no building). `status_json` drops `groups` and emits `units`. `presets_json` emits `models` and `rooms`. `lit_count_at` and `active_count_at` walk units instead of groups.
- [ ] Run the unit tests. Expected: all pass.
- [ ] **apicheck.** In `tools/apicheck.sh` change the identify line to `'{"lamps":[0,1]}'` and add `check PUT /api/scene/config 400 '{"units":[{"name":"x","model":"nosuch","rooms":[]}]}'` after the existing config PUT. Add `test-mock:` to the Makefile running `python3 -m unittest tools/test_mockserver.py`, listed in the help comment and `.PHONY`.
- [ ] Start the mock on port 8093 and run `tools/apicheck.sh http://localhost:8093`. Expected: no line other than the documented skips shows an unexpected code. Kill the mock.

### Task 3: GUI

Files: `web/index.html`, `web/view-scene.js`, `web/view-scene.css`, `web/view-models.js` (new), `web/view-models.css` (new), `web/view-houses.js`, `web/view-houses.css`, `web/view-home.js`, `web/view-home.css`.

Interfaces consumed: `/api/scene/config` per spec 2.5, `/api/scene/status` per spec 3.3, `/api/scene/presets` per spec 6, `App.el`, `App.api`, `App.toast`, `App.register`, `App.go`, `App.parseRanges`, `App.rangesText`, `App.lampDisc`.

- [ ] **Nav.** `index.html`: add `<a href="#models" data-view="models">Models</a>` between Scene and Houses. `buildweb.py` picks up `view-models.*` by glob; nothing else to register.
- [ ] **Scene.** Remove `groupsCard()`, `groupRow()`, `groupForm()`, `renderGroups()`, `addGroup()`, `lampsJson()`, `disc()`, `fillDiscs()`, `countText()`, `applyPreset()`, the `taken` map, `BEHAVIOURS`, `BEH_LABELS`, `PRESET_KEYS`, `OVR`, `MAX_DISCS`, `MAX_GROUPS` and the presets fetch. Move the seed number field into `locationCard()` under the coordinates with the label "Seed" and the hint "changes everyone's habits". `saveScene()` still sends the whole config object as fetched, with only location, clock and seed changed. Drop the `.taken` and disc rules from `view-scene.css`. Header comment: "the town clock over a horizon scrubber and the location dusk and dawn are computed from".
- [ ] **Models view.** `view-models.js` registers `"models"`. State: `cfg` (the whole scene config), `presets` (the templates), `changed`, `open` (index of the expanded row). Constants mirror the firmware: `MAX_MODELS = 16`, `KINDS = ["rhythm", "hours"]`, `KIND_LABELS = {rhythm: "rhythm", hours: "hours"}`, `TEMPLATES` in spec 2.3 order, the `DAY_WORDS`/`NIGHT_WORDS` arrays as the other views have. Render one card with a row per model: name, kind in a lighter face, the one-line summary (`rhythmText()` moved here from `view-houses.js` for rhythm; for hours `"dusk +0..+240 to 22:00..00:30"` built from the anchors and windows, with clock anchors formatted `HH:MM` and dusk or dawn anchors as signed minutes), and "n units" counted over `cfg.units`. Clicking a row opens the editor under it: name field; kind selector; for rhythm the four windows, weekend, out and TV percents lifted from today's `flatForm()`; for hours the anchors, windows, share lit, television, morning and a switch "Each lamp keeps its own moment" lifted from today's `groupForm()`; then brightness, fade, day and night activity. "Delete" on a row: refused with a toast "Model in use by n units" when referenced, otherwise removes it. "New model" opens a small chooser listing the nine templates by title; the new model copies the template and gets the title, with " 2", " 3" appended when taken. Renaming: on commit, every unit whose `model` equalled the old name gets the new one before the payload is built. Validation: name non-empty and unique, windows in range as the source forms do. Save: `PUT /api/scene/config` with the whole `cfg`, toast "Models saved". Mount: fetch presets and config together as `view-houses.js` does; `unmount()` clears state. Header comment as the other views. `view-models.css`: row grid at 390 px is name, kind, summary on the second line, units count right-aligned; the chooser is a list of buttons.
- [ ] **Houses view.** In `view-houses.js`: `TYPES` and `TYPE_LABELS` go; the selector lists `cfg.models` by name with the kind in a `<small>`; `ROOMS` and `ROOM_LABELS` and `ROOM_LETTER` grow to nine (front "Shop front", back "Back room", sign "Sign"); `STATES` gains open, closed, lit, dark; `rhythmText()`, `RANGES`, `SEED_KEYS`, `NUMS`, the rhythm part of `flatForm()`, brightness and fade go; a "Model" link in the row calls `App.go("models")`; `flatName()` becomes `unitName()` with "Unnamed unit"; `MAX_ROOM_LAMPS` becomes `MAX_ROOM_RANGES = 8` and `readRoom()` counts ranges from `App.parseRanges()` output rather than lamps (the parser returns the expanded list today, so count runs: consecutive numbers are one range); the identify button still sends the first eight lamps. Units are grouped by building with "No building" first. Empty state "No units yet". Toast "Houses saved". `payload()` writes `units` with `model` by name. Rename `flat`/`flats` identifiers to `unit`/`units` throughout, including CSS class names in `view-houses.css`.
- [ ] **Home strip.** `view-home.js`: `STATES` gains open, closed, lit, dark; `ROOM_WORDS` gains f "shop front", r "back room", s "sign"; `showFlats()` becomes `showUnits()` reading `status.units`; chip text "Unnamed unit". `view-home.css`: four new `.st.open`, `.st.closed`, `.st.lit`, `.st.dark` glyph rules (open and lit filled, closed and dark hollow, using the existing awake and asleep colours).
- [ ] Run the mock on port 8094 and check in a browser or with the CDP driver at 390 px: the six tabs fit on one line; Scene shows two cards; Models lists nine rows and both editors open; deleting a used model is refused; renaming a model updates the selector on Houses; Houses shows Ica Nära under Storgatan 3 and Main street under "No building"; Home shows a chip per unit with the shop open at 12:00 under a manual clock. Screenshots into the scratchpad.
- [ ] `make web`. Expected: size line under 40960 bytes gzipped. Report the number.

### Task 4: Documentation

Files: `HANDOFF.md`, `miniWorld_LightingController/HANDOFF.md` (keep identical, `diff` must be empty), `CLAUDE.md`, `README.md`.

- [ ] HANDOFF section 2: the HttpServer line lists the six views; the SceneEngine line says "models (rhythm and hours), units with rooms of lamp ranges". Section 3: the rows for `Scene.h/.cpp`, `SceneEngine.h/.cpp`, `SceneWebApi.h/.cpp`, `view-scene`, `view-houses`, `view-home` updated and a row for `view-models.js/.css` and one for `tools/test_mockserver.py` added. Section 5: a new item 5.8 "Models and units on the board" with the six verification steps of spec 7 and a "done" note left for Task 5. Section 5.6 step 1 now says "a unit on the Home model". Section 5.7's text refers to units. Section 6: replace the RAM-with-flats item with the measured figure from Task 1. Section 7: the bullet "Adding a household type or a room role" becomes "Adding a template or a room role touches the enum, the name table, `setTemplate`, the mock's `MODEL_TEMPLATES` and the GUI's lists together; a room role also touches the three rate tables and the letters". Section 8: replace the "Why the household is the unit" and "Why there is no family group behaviour" entries with one entry "Why models and units" carrying spec 8's reasons in short.
- [ ] `CLAUDE.md`: the two architecture bullets on scene groups and flats become one on models and units (config, engine paths, room key, `evaluateUnits()`, status), and the "adding a behaviour" sentence becomes the section 7 wording above. The three persisted documents bullet says `/scene.json` holds models, units and the clock.
- [ ] `README.md`: the Scene engine and Households bullets become "Models" and "Units" bullets in the same voice; the GUI line lists six tabs.
- [ ] A grep for the em dash character over the four files returns nothing.

### Task 5: Flash and bench (controller)

- [ ] **Before flashing**, with the board still on commit 0fbf281: set the clock to manual and record `state` and `lit` per flat from `/api/scene/status` at every minute from 05:00 to 23:30 into `scratchpad/before.json`, then put the clock back to real. This is the reference for the migration check and cannot be taken afterwards.
- [ ] Confirm the port by USB id (`udevadm info -q property -n /dev/ttyACM2 | grep ID_MODEL_ID` must print `100d`), then `make upload PORT=/dev/ttyACM2`.
- [ ] Read `/api/scene/config`: four rhythm units at Storgatan 3, Andersson on Family, Karlsson on Elderly couple, Nilsson on Night owl, Convenience store on its own model; models for Flats, Shops, Pub and grill, Kiosk church and New group; a Flats unit with lamps 0-15.
- [ ] Repeat the scrub into `scratchpad/after.json` and diff the three template households against `before.json` minute by minute. They must be identical. Report any difference verbatim; do not adjust the engine to hide one.
- [ ] Through the GUI on the phone or a browser: change Convenience store to the Shop model with a front room on lamps 7 and 8, scrub 08:00 to 09:30 and 17:30 to 19:00 and record when it opens and closes. Make Main street on Street light with lamps 0-15 in one room (after removing them from the Flats unit) and see the sweep at dusk under accelerated time.
- [ ] Record the outcome in HANDOFF 5.8 in both copies, commit.
