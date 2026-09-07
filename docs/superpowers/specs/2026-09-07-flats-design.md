# Flats (households with rooms): design

Date: 2026-09-07. Owner: Pontus, Invector Embedded Systems AB.
Status: approved in conversation, implementation follows this document.

## 1. Goal

Give a building personality. A flat is a household: a name (Andersson),
one daily rhythm (wake, leave, come home, dinner, bed), rooms that light
in the natural order, and the life layer from the previous step made
room-aware, so the loo gets the night visits and the kitchen the coffee.
Groups stay for everything that is not a household.

## 2. Model

```
#define SCENE_MAX_FLATS  32
#define FLAT_MAX_ROOMS   12

enum class Household : uint8_t { Family = 0, Elderly, NightOwl, Away, Custom, COUNT };
enum class Room      : uint8_t { Living = 0, Kitchen, Bedroom, Bathroom, Hall, Other, COUNT };

struct RoomConfig { uint16_t lamp; Room role; };

struct FlatConfig {
    char name[SCENE_NAME_LEN];        // "Andersson"
    char building[SCENE_NAME_LEN];    // "Storgatan 3", GUI grouping only, may be empty
    Household type;
    bool weekend;                     // weekend rhythm on Saturday and Sunday
    uint8_t roomCount;
    RoomConfig rooms[FLAT_MAX_ROOMS];
    // Rhythm, minutes since midnight; a range is [from, to]. Filled by
    // setPreset(type) and editable (type Custom keeps whatever is set).
    int16_t wakeFrom, wakeTo;
    int16_t leaveFrom, leaveTo;       // leaveFrom < 0: never leaves on weekdays
    int16_t homeFrom, homeTo;
    int16_t bedFrom, bedTo;           // may exceed 1440
    uint8_t outPercent;               // evenings out, 0..100
    uint8_t tvPercent;                // evenings with television, 0..100
    uint8_t level;                    // 0..255
    uint16_t fadeMs;
    uint8_t dayActivity, nightActivity;   // 0..3, same meaning as groups
};
```

`SceneConfig` gains `uint8_t flatCount; FlatConfig flats[SCENE_MAX_FLATS];`.
About 32 x 110 bytes; every `SceneConfig` instance is static already.

JSON, inside `/scene.json` next to `groups`:

```json
"flats": [
  { "name": "Andersson", "building": "Storgatan 3", "type": "family",
    "weekend": true,
    "rooms": [ {"lamp": 4, "role": "kitchen"}, {"lamp": 5, "role": "living"} ],
    "wake": [390, 435], "leave": [450, 495], "home": [960, 1050], "bed": [1350, 1410],
    "outPercent": 14, "tvPercent": 70, "level": 210, "fadeMs": 300,
    "dayActivity": 3, "nightActivity": 1 }
]
```

Type names: `family`, `elderly`, `nightowl`, `away`, `custom`. Room names:
`living`, `kitchen`, `bedroom`, `bathroom`, `hall`, `other`. On load a
flat is built with `setPreset(type)` first, then present fields
override, like groups. Missing `flats` means none. `clamp()`: name and
building non-empty (name defaults to "Flat n"), roomCount 0..12, lamp
below `LAMPS_MAX_LAMPS`, percentages 0..100, activities 0..3, ranges
ordered (`to >= from`).

Presets (`setPreset`), weekday values:

| type | wake | leave | home | bed | out % | tv % | level | fade | day | night |
|---|---|---|---|---|---|---|---|---|---|---|
| family | 06:30..07:15 | 07:30..08:15 | 16:00..17:30 | 22:30..23:30 | 14 | 70 | 210 | 300 | 3 | 1 |
| elderly | 05:45..06:30 | 09:30..10:30 | 12:00..13:30 | 21:15..22:15 | 7 | 40 | 180 | 400 | 2 | 3 |
| nightowl | 09:30..11:00 | 11:30..12:30 | 19:00..21:00 | 00:45..02:15 | 28 | 80 | 200 | 300 | 1 | 1 |
| away | none (-1) | none | none | none | 0 | 0 | 200 | 0 | 0 | 0 |
| custom | family values | | | | | | | | | |

In minutes: family 390..435, 450..495, 960..1050, 1350..1410; elderly
345..390, 570..630, 720..810, 1275..1335; nightowl 570..660, 690..750,
1140..1260, 1485..1575. `away` sets all eight range values to -1.

Weekend rules apply when `weekend` is true and the day is Saturday or
Sunday (see 3.1): wake shifts by +90 min (family), +30 (elderly), +90
(nightowl); no leave/home; instead an afternoon out with probability
50 % (family), 40 % (elderly, errands 10:00..11:30 to 12:30..14:00 for
elderly rather than afternoon), 30 % (nightowl): out 11:00..13:00, back
15:00..17:30; bed +30 min; out-evening probability doubles.

## 3. Engine

### 3.1 The day of the flat

Once per day per flat (cached in `FlatDay _flatDay[SCENE_MAX_FLATS]`,
recomputed when `_doy` changes or on `rebuild()`), draw from
`hash(0x20000 + flat + 0x10000 * doy, salt)` through the same `unit`
style helper, salts 1..8, seeded like everything else:

```
wake   = lerp(wakeFrom, wakeTo, u1)
leave  = lerp(leaveFrom, leaveTo, u2)      (-1 when the range is -1 or on a weekend day)
home   = lerp(homeFrom, homeTo, u3)
dinner = home + lerp(45, 90, u4)           (weekend: 17:30..18:30 by u4)
bed    = lerp(bedFrom, bedTo, u5)
out    = u6 * 100 < outPercent             (weekend: 2 x outPercent)
tv     = u7 * 100 < tvPercent
ret    = bed - lerp(15, 25, u8)            (return time on an out evening)
outAft = weekend and u2 * 100 < afternoon-out %   (leave 660..780, home 900..1050 by u3, u8)
```

Jitter as the habits: no separate jitter, the daily draw is the jitter.
Weekday: `tm_wday` from `localtime_r` when the clock is valid, else
derived from `_doy` with 2026-01-01 a Thursday: `wday = (doy + 3) % 7`.
Weekend is `wday == 0 || wday == 6`.

`away` type: no rhythm; a living or other room, if present, is a timer
lamp 19:00..22:30; every other room dark; no events.

### 3.2 Room schedule

Base state per room, intervals in minutes, `t` the simulated minute
compared with `inWindow()` semantics (intervals may cross midnight):

| room | intervals |
|---|---|
| kitchen | [wake, wake + 20..40 (u by room)] and [home + 5..15, dinner + 45..75]; on a weekend the evening interval is [dinner - 40..60, dinner + 45..75] whatever the afternoon held, plus [12:00, 13:00] when at home |
| hall | [leave - 5, leave + 2], [home - 1, home + 6] (on a weekend only around an afternoon out); out evening: [ret - 1, ret + 5] |
| living | [dinner, bed - 5..15]; out evening: [ret, bed] only; television flicker on this room when `tv` |
| bedroom | [bed - 10..20, bed + 5..10] and [wake - 2, wake + 8] |
| bathroom | [wake + 5, wake + 15]; otherwise events only |
| other | like living without television |

Per-room variation (the 20..40 style ranges) uses `unit(lamp, salt)`
so two kitchens in one building differ. When the household is out
(between leave and home, or an out evening before `ret`), every room is
dark regardless of the table.

Daylight clipping: an evening interval starts no earlier than
`sunset - 30`; a morning interval ends no later than `sunrise + 30`.
Nobody switches the living room on at 19:00 in June. The bathroom and
hall intervals are not clipped.

A lamp listed in a flat is owned by the flat: groups no longer drive it
(`_lampGroup` stays 0xFF for it, or a separate `_flatOwned` bitmap wins
in `tick()`). Rebuilt in `rebuild()`. The first flat listing a lamp wins.

### 3.3 Events by room

The life layer runs for flat lamps with rates scaled by room, using the
flat's `dayActivity` and `nightActivity` levels and the same slot hash,
kinds and lengths as the activity spec:

| room | day rate x | dip rate x | night rate x |
|---|---|---|---|
| bathroom | 0.5 | 0 | 1.0 |
| hall | 0.7 | 0 | 0.3 |
| kitchen | 1.0 | 0.3 | 0 |
| living | 0.3 | 1.0 | 0 |
| bedroom | 0.2 | 0.2 | 0.2 |
| other | 0.3 | 0.5 | 0 |

The night window of a flat lamp is [bed, wake + 1440] of the flat, not
the lamp's own off moment. No events while the household is out. No
events for `away`.

### 3.4 Evaluation and cost

`evaluateFlats()` runs when the simulated minute or the day changes,
before `evaluateEvents()`. It fills three bitmaps over lamps: `_flatLit`
(base on), `_flatTv` (flicker), and reuses `_eventOn` / `_eventOff` for
the events through the shared `eventAt()`. `tick()` for an owned lamp:
base = `_flatLit` ? flat level : 0, flicker = `_flatTv`, then the event
bits override as today. Cost: 32 flats x 12 rooms = 384 lamps at most.

### 3.5 Status

`/api/scene/status` gains

```json
"flats": [ { "name": "Andersson", "state": "awake", "lit": "lk" } ]
```

`state` is `asleep` (bed..wake), `out` (leave..home, or an out evening
before `ret`), `awake` otherwise, `away` for the away type. `lit` is one
letter per role with at least one lit room, always in the canonical
order `l` living, `k` kitchen, `b` bedroom, `t` bathroom, `h` hall, `o`
other, regardless of the flat's room list order; empty string when dark.
Events count as lit.

`/api/scene/presets` gains `"households": { "family": {...all rhythm
fields...}, ... }` (no entry for `custom`) and `"rooms": ["living", ...]`.

## 4. GUI

### 4.1 Houses view, the fifth tab

`web/view-houses.js`, `web/view-houses.css`, nav link `Houses` after
Scene in `index.html`. Flats listed under their building label (flats
with an empty building under "Other"); a row shows name, type label, the
live state word and the lit rooms as small role glyphs (letters in
discs are fine). Tap to expand: name, building, type select (Family,
Elderly couple, Night owl, Away, Custom), weekend switch, the room table
(rows of lamp number input and role select, add row, remove row, up to
12), then a "Rhythm" block with the eight range fields shown as HH:MM
pairs, out %, TV %, level, fade, and the two activity selects; changing
the type re-seeds every rhythm field from the household presets except
when the type is Custom. Add flat (seeded from Family, name "Flat n"),
delete with confirm, Save writes the whole scene config (flats and
groups) with `PUT /api/scene/config`; a `changed` marker; validation:
a lamp number outside 0..lamps-1 or used twice within the flat is an
error and disables Save; a lamp used by another flat shows a warning
text but saves (the first flat wins).

The type labels and rhythm sentence: under the type, one muted line
such as "wakes around 06:50, leaves 07:50, home 16:40, bed 23:00; out
one evening in seven" computed from the midpoints of the ranges and
`outPercent` (1/outPercent rounded, "never out" for 0). Poll every 2 s
from status for the state words and lit letters, in place.

### 4.2 Home strip

Home gains a "Households" card between the buses and the network card:
one chip per flat with the name, a glyph for the state (moon asleep,
door out, lamp awake, dash away) and the lit letters. Hidden when there
are no flats. Updated in place from status.

### 4.3 Scene view

Lamp discs in the group editor render lamps owned by a flat in a
distinct "taken" style with a title attribute naming the flat; nothing
else changes. The group's lamp count line reads "16 lamps, 3 in flats".

### 4.4 Mock

`tools/mockserver.py`: `flats` in the config with absent-means-preset,
`households` and `rooms` in presets, `flats` in status with a state
derived from the simulated minute and the flat's midpoints (asleep
before wake and after bed, out between leave and home on weekdays,
awake otherwise, away for away) and `lit` letters that follow the room
table at the midpoints, so the view can be built and watched off-target.
Seeded example: two buildings, five flats (Andersson family,
Karlsson elderly, Nilsson nightowl, Persson family, Svensson away) on
lamps 16..40.

## 5. Verification

- `make` warning-free; report the globals delta.
- Engine: a host-side reasoning example for one family flat and one day
  in the report: the eight draws, the moments, each room's intervals.
- Mock and views: screenshots at 390 px of the Houses list and an
  expanded flat; Home strip with five chips; Scene discs showing taken
  lamps.
- Board: create the five example flats through the GUI or the API on
  lamps the board reports, scrub 05:30..08:30 and 21:00..00:30 in one
  minute steps and record `flats[].state` and `lit`: kitchen before
  living in the evening, bedroom last, asleep after bed, bathroom
  letters appearing briefly at night; re-scrub equal; no reboot; the
  Houses view shows the words changing under the scrubber.

## 6. Decisions and reasons

- **Rooms as a short lamp list, not a bitmap.** A flat has a handful of
  lamps; 32 flats of bitmaps would cost 80 kB across the static config
  copies.
- **One rhythm per flat, rooms derived.** That is the whole point: the
  household is the unit of behaviour, the lamp is not.
- **Daylight clipping on evening and morning intervals.** Clock-anchored
  rhythms would otherwise light living rooms at 19:00 in June.
- **Fifth tab.** The Scene page already carries the clock and the group
  editor; households deserve their own place and the Home strip gives
  the glance.
- **Presets fill editable fields.** Same principle as groups: nothing is
  locked behind a type, Custom is just "keep what I typed".
