# Behavioural models and units: design

Date: 2026-09-22. Owner: Pontus, Invector Embedded Systems AB.
Status: approved 2026-09-22. Implementation plan:
`docs/superpowers/plans/2026-09-22-models-and-units.md`.

## 1. Goal

Make the scene read the way a person thinks about a model town. There is
a list of ways a building can behave (an elderly couple, a family, a
convenience store, a pub, a street light) and there is a list of things
on the layout (the Anderssons at Storgatan 3, the store on the corner,
the lamps along Main street), each of which behaves in one of those
ways.

Today those two ideas are spread over three constructs: a group, which
is a behaviour and a lamp list in one; a flat, which is a household with
a rhythm and rooms; and a household type, which is a preset for a flat.
A shop in a building has no home in any of them, which is why the board
carries a "Convenience store" flat with a family bedtime.

After this change there are two constructs:

- A **model** is a named way of behaving. It has no lamps.
- A **unit** is a thing on the layout: a name, a building, a model, and
  rooms with lamps.

Groups and flats go. The Scene tab keeps the clock and the location. A
new Models tab holds the models. The Houses tab holds the units.

Out of scope: per-unit overrides of a model (a unit is exactly its
model), a sign that is lit outside opening hours, and buildings as
entities with their own properties. Building stays a label.

## 2. Model

### 2.1 Limits

```
#define SCENE_MAX_MODELS   16
#define SCENE_MAX_UNITS    32
#define UNIT_MAX_ROOMS     12
#define ROOM_MAX_RANGES     8
#define SCENE_NAME_LEN     24     unchanged
```

A room holds up to eight lamp ranges instead of eight lamps. "4, 6-8" is
two ranges; "0-63" is one. That is what lets a street of sixty lamps be
one unit with one room, which is what makes groups redundant. A room
with more than eight ranges is an error on load, as nine lamps is
today. Identify still blinks at most eight lamps: the first eight of the
room.

Memory. A unit is about 460 bytes (two names, a model index, twelve
rooms of eight ranges), so 32 units are about 14.7 kB. A model is about
50 bytes. Today groups and flats together are about 13.2 kB, so
`SceneConfig` stays at roughly its current size and the five static
copies stay within the 39 % RAM the board reports now. If that is tight
on the bench, `SCENE_MAX_UNITS` drops to 24 before anything else moves.

### 2.2 A model

```
{
  "name": "Elderly couple",
  "kind": "rhythm" | "hours",
  "level": 180, "fadeMs": 400,
  "dayActivity": 2, "nightActivity": 3,

  rhythm only:
  "weekend": true,
  "wake": [345, 390], "leave": [570, 630], "home": [720, 810],
  "bed": [1275, 1335],
  "outPercent": 7, "tvPercent": 40,

  hours only:
  "onAnchor": "dusk", "on": [0, 240],
  "offAnchor": "clock", "off": [1320, 1470],
  "litPercent": 85, "flickerPercent": 12, "morning": true,
  "individual": true
}
```

The two kinds are the two simulations the engine already has. A rhythm
model is today's `FlatConfig` without its name, building and rooms. An
hours model is today's `GroupConfig` without its name and lamp bitmap.
Every field keeps its meaning, its range and its clamp. Level and fade
move onto the model, so a unit has no brightness of its own.

`individual` is new and only on hours models. True means every lamp in
the unit keeps its own moment inside the window, its own chance of
taking part and its own television, exactly as a group lamp does today.
False means a room draws once and all its lamps switch together, as a
flat's room does today. Street light, Shop and Pub default to false; the
generic Home model defaults to true, so a block of windows keeps coming
on spread over an hour. Rhythm models always draw per room.

Names are unique within the scene, compared case-insensitively, and a
model is referred to by name everywhere. The firmware resolves names to
indices on load, so renaming a model in the GUI has to rename it in the
units that use it in the same save.

### 2.3 Built-in templates

A fresh device with no `/scene.json` has these models and no units. The
Models tab also offers them under "New model, start from", and
`/api/scene/presets` returns them under `"models"`:

| name | kind | from today's preset |
|---|---|---|
| Family | rhythm | Household::Family |
| Elderly couple | rhythm | Household::Elderly |
| Night owl | rhythm | Household::NightOwl |
| Away | rhythm | Household::Away |
| Home | hours, individual | Behaviour::Home |
| Shop | hours | Behaviour::Shop |
| Pub | hours | Behaviour::Late |
| Street light | hours | Behaviour::Street |
| All night | hours | Behaviour::AllNight |

There is no Off model. A unit that should be dark gets an hours model
with `litPercent` 0, or is removed. There is no Custom: every model is
custom once it is edited, and the template it started from is not
remembered.

### 2.4 A unit

```
{
  "name": "Andersson",
  "building": "Storgatan 3",
  "model": "Family",
  "rooms": [
    { "lamps": [0, 14], "role": "kitchen" },
    { "lamps": ["5-6"], "role": "living" }
  ]
}
```

Rooms are as today: a role and a lamp list in the range syntax. The
role list grows for non-households:

```
living, kitchen, bedroom, bathroom, hall,   as today
front, back, sign,                          new
other                                       as today
```

Under a rhythm model the three new roles behave as `other`. Under an
hours model every room follows the hours; the roles are for the GUI and
the status letters, and the sign getting its own hours is a later step.
A lamp may appear in one unit only; the first unit listing it wins, as
between flats today. Building is a label used to group rows on the
Houses tab and may be empty; a street has no building.

### 2.5 The scene document

```
{
  "location": { ... unchanged ... },
  "clock":    { ... unchanged ... },
  "seed": 1,
  "models": [ ... ],
  "units":  [ ... ]
}
```

`groups` and `flats` are no longer written. They are still read, see
section 5.

## 3. Engine

### 3.1 Ownership

`_lampGroup[]` and `_lampFlat[]` become one array, `_lampUnit[]`, 0xFF
for a lamp no unit lists. `rebuild()` fills it from the units in order,
first listing wins. The unit's model index is read through the unit.

### 3.2 Per-lamp path

The per-lamp loop in `tick()` keeps its shape. For a lamp with a unit:

- Rhythm model: two bit tests in `_unitLit` and `_unitTv`, which are
  today's `_flatLit` and `_flatTv`. `evaluateFlats()` becomes
  `evaluateUnits()` and runs the rhythm draw for rhythm units only.
- Hours model, `individual` true: `targetLevel(lamp, M, ...)` as a
  group lamp today, keyed by the lamp index.
- Hours model, `individual` false: `targetLevel(roomKey, M, ...)` once
  per room per simulated minute, cached in the same `_unitLit` and
  `_unitTv` bitmaps, so the 40 Hz loop still only tests bits.

The activity layer is unchanged: `eventAt()` takes the lamp index for an
individual lamp and the room key otherwise, with the room's rate scaling
for the roles that have one.

`roomKey` stays `0x30000 + unit * 16 + room`. Units are numbered in the
order they are listed, so removing a unit reshuffles the draws of the
units after it, as removing a flat does today.

### 3.3 Status

`/api/scene/status` reports `"units"` instead of `"flats"` and drops
the `"groups"` count:

```
"units": [
  { "name": "Andersson", "state": "awake", "lit": "kl" },
  { "name": "Ica Nära",  "state": "open",  "lit": "fs" },
  { "name": "Main street", "state": "lit", "lit": "o" }
]
```

A rhythm unit reports asleep, out, awake or away as a flat does today.
An hours unit reports open or closed when its model is clock anchored
on both ends, and lit or dark otherwise. The letters are one per role
with a lit room: l k b t h and the new f (front), r (back), s (sign),
o (other).

`SceneEngine::flatOwner()` becomes `unitOwner()`. `identify()` is
unchanged.

## 4. GUI

Six tabs: Home, WiFi, Lamps, Scene, Models, Houses.

### 4.1 Scene

The clock card and the location card as today. The groups card goes,
and with it the taken-lamp discs. The seed moves into the location
card, since it is the other thing that changes everyone's habits.

### 4.2 Models, the new tab

A card with one row per model, in the order stored:

```
Elderly couple   rhythm   wakes 05:45..06:30, bed 21:15..22:15, TV 40 %   3 units
Shop             hours    08:30..09:00 to 18:00..18:30                    1 unit
Street light     hours    dusk -10..+5 to dawn -5..+15                    1 unit
```

Opening a row shows the editor for its kind. The rhythm editor is
today's flat rhythm editor lifted out of the Houses view: the four
windows, weekend, evenings out, television. The hours editor is
today's group editor without the lamp field: anchors, windows, share
lit, television, morning light, and the new "Each lamp keeps its own
moment" toggle. Both end with brightness, fade, and the day and night
activity words as today.

"New model" asks which template to start from, gives it the template's
name with a number if that name is taken, and opens the row. "Delete"
is refused with a toast, "model in use", while any unit uses the model;
that check is the GUI's own. The firmware refuses the same save with
`unknown model`, because what reaches it is a unit naming a model the
document does not carry. Renaming updates the references in the units in
the same payload before Save.

Save writes the whole scene document, as the other tabs do.

### 4.3 Houses

As today, with these changes:

- The type selector becomes the model selector, listing the models by
  name with their kind in a lighter face.
- The rhythm editor is gone from the row. A "Model" link opens the
  Models tab on that model.
- Brightness and fade are gone from the row; they are on the model.
- The role selector offers the three new roles.
- The empty-state text says "No units yet" and the Save toast says
  "Houses saved".

A unit without a building is listed under "No building" at the top,
which is where the street goes.

### 4.4 Home strip

One chip per unit, as one per flat today. The state glyph gains the
open, closed, lit and dark states. A layout with a street and three
households shows four chips.

### 4.5 Mock

`tools/mockserver.py` gets `MODEL_TEMPLATES`, `models` and `units` in
its state, the migration of section 5 in its loader, and the new status
shape. Its fixture has the nine templates, the four households from
today's fixture, an "Ica Nära" shop at Storgatan 3 with front, back and
sign rooms, and a "Main street" unit with no building.

## 5. Migration

`SceneConfig::fromJson()` accepts either shape. When `"models"` is
absent and `"groups"` or `"flats"` is present:

1. Flats first, so their unit indices, and therefore their room keys and
   their household draws, stay what they are today. Each flat becomes a
   unit with its name, building and rooms. Its model is the template for
   its household type when every rhythm field equals that template's
   value, otherwise a new rhythm model named after the flat. Custom
   always gets its own model.
2. Then groups. Each group with at least one lamp becomes an hours model
   named after the group, `individual` true, and a unit of the same name
   with no building and rooms of role `other` holding the group's lamps
   as ranges, eight ranges per room, more rooms as needed. A group with
   no lamps becomes a model only. `Off` becomes a model with
   `litPercent` 0.
3. A model that ends up with a name already taken gets a number.
4. The retired names family, elderly, nightowl and away on a group still
   load as Home first, then migrate as Home.

The next save writes the new shape. The migration lives in the firmware
and in the mock and is exercised by loading today's board scene and
comparing the lit set minute by minute against the pre-migration image.
There is no path back: an image before this change does not read
`"units"`, and comes up with an empty scene.

On the board today this produces: four rhythm units at Storgatan 3
(Andersson, Karlsson, Nilsson on the templates; Convenience store on its
own model, which the user then edits into an hours model or replaces),
and five hours models Flats, Shops, Pub and grill, Kiosk church, New
group, of which only Flats also has a unit.

## 6. API

- `GET /api/scene/config` and `PUT` carry the new document.
- `GET /api/scene/status` as in 3.3.
- `GET /api/scene/presets` returns `"models"` (the nine templates, each
  a complete model object) and `"rooms"` as today. The old per-behaviour
  and `"households"` keys go.
- `POST /api/scene/identify` and `PUT /api/scene/clock` unchanged.
- Errors added: `unknown model`, `duplicate model name`, `too many
  models`, `too many units`, `too many ranges`. There is no `model in
  use`: a save that drops a model some unit still names is refused with
  `unknown model`, and "model in use" is a toast the GUI raises before
  such a save is ever sent.

## 7. Verification

1. Mock: `make check` passes; loading the pre-change fixture through the
   migration gives the units and models of section 5.
2. Firmware: `make` clean, RAM at or under today's 39 %.
3. Board: flash, watch the boot load migrate the stored scene, then
   scrub 05:00 to 23:30 in one-minute steps and compare each flat's
   `state` and `lit` against a scrub recorded before flashing. They
   must be identical for the three households on templates.
4. Board: make "Ica Nära" with the Shop model and lamps 7 and 8, scrub
   through 08:00 to 09:30 and 17:30 to 19:00, and see it open and close.
5. Board: make "Main street" with Street light and lamps 0-15 in one
   room, and see the whole street switch at dusk in one sweep.
6. GUI at 390 px: the Models table, both editors, the model selector on
   Houses, and the six-tab nav all fit without horizontal scroll.

## 8. Decisions and reasons

- **Two kinds, not one.** A household day and an opening-hours day are
  different simulations and the engine has both. Forcing one shape on
  both would either give a shop a bedtime or take the rooms in order
  away from a family. The kind field is the honest answer.
- **Models have no lamps.** That is the whole point: a model is what a
  building does, a unit is where it is. The count of units using a
  model is shown so a model is never edited blind.
- **No per-unit overrides.** Two shops with different hours are two
  models. That is one more row in a table rather than a hidden override
  on a unit, and it keeps a unit to four fields.
- **Ranges in rooms.** Without them a street would be eight rooms of
  eight lamps and groups would not be redundant. Eight ranges is enough
  for any room that exists today and for any street that is one run of
  addresses.
- **Individual on the model.** Whether lamps behave as one is a property
  of the kind of building: a street switches together, a block of flats
  does not. Putting it on the room would ask the question twelve times.
- **Reference by name.** A unit reading `"model": "Shop"` is
  self-describing in the JSON and survives a model being moved in the
  table. The cost is a rename that has to touch the units, which the
  GUI does in one payload.
- **Flats first in the migration.** The household draws hang on the unit
  index. Keeping the flats at the front means the Anderssons keep their
  habits through the change, and only the groups, which never had
  per-unit draws, get new indices.
- **Building stays a label.** Nothing yet needs a building to be more
  than a heading on the Houses tab. When something does, for example a
  stairwell lamp shared by the households of one house, a building can
  become a unit of its own.

## 9. Implementation notes

Details settled while planning. They are part of the design.

### 9.1 Weekend rhythm without a household type

The engine today picks the Saturday wake shift and the weekend afternoon
out chance by household type. A model has no type, so both are read off
the rhythm itself, which gives the four templates the values they have
now:

| rhythm | wake shift | weekend out |
|---|---|---|
| away (no wake, no bed) | 0 | 0 % |
| `bedTo` at or before 22:30 (1350) | 30 min | 40 % |
| `wakeFrom` at or after 09:00 (540) | 90 min | 30 % |
| anything else | 90 min | 50 % |

### 9.2 Room rates for the new roles

The three per-role rate tables grow to nine entries in `Room` order:

| role | day | dip | night |
|---|---|---|---|
| living | 0.3 | 1.0 | 0.0 |
| kitchen | 1.0 | 0.3 | 0.0 |
| bedroom | 0.2 | 0.2 | 0.2 |
| bathroom | 0.5 | 0.0 | 1.0 |
| hall | 0.7 | 0.0 | 0.3 |
| front | 0.3 | 0.5 | 0.0 |
| back | 0.7 | 0.3 | 0.0 |
| sign | 0.0 | 0.0 | 0.0 |
| other | 0.3 | 0.5 | 0.0 |

A sign has no life. Under an hours model a room's night window is the
room's own off moment to its on moment, as a group lamp's is today.

### 9.3 State of an hours unit

`open` or `closed` when both anchors are `clock`, otherwise `lit` or
`dark`. The unit is open, or lit, when the simulated minute is inside
the window built from the middle of each range: `anchorBase(on) +
(onFrom + onTo) / 2` to `anchorBase(off) + (offFrom + offTo) / 2`. It
does not look at `litPercent`, so a shop whose every lamp opted out is
still "open"; the letters below say what is actually lit. The one
exception is `litPercent` 0, a unit no lamp takes part in at all: it
reports "closed" when both anchors are clock and "dark" otherwise, which
is what an old Off group migrates to.

### 9.4 Lit letters for any unit

A room's letter shows when one of its lamps is lit. A lamp in an event
counts as its event says, as today. Otherwise a rhythm lamp and a lamp
of a non-individual hours room read their `_unitLit` bit, and a lamp of
an individual hours unit is lit when its current level is not zero.

### 9.5 Away is a rhythm shape

There is no away flag. A rhythm model with no wake and no bed (`wake`
and `bed` both `[-1, -1]`) is the away household: the timer evening in
the living room, or an `other` room when there is no living room, and
no life on top. Editing the Away template's windows turns it into an
ordinary household, which is what a person would expect.
