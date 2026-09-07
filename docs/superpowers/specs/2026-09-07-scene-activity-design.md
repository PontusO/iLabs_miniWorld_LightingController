# Scene activity ("life" layer) and household presets: design

Date: 2026-09-07. Owner: Pontus, Invector Embedded Systems AB.
Status: approved in conversation, implementation follows this document.

## 1. Goal

Make the town breathe. Today a lamp's state is a pure function of the
clock: off until its personal on-moment, on until its off-moment, plus
the morning light and the television flicker. Add a sparse, deterministic
layer of short events on top: lights that pop on for a few minutes during
the day (the coffee, the search for the keys), lights that go off briefly
in the evening (someone left the room), and lights that come on at night
(the loo visit). Add household presets so a flat can be "family",
"elderly couple", "night owl" or "away" with one choice.

Out of scope for this step: rooms and per-lamp roles (kitchen, bathroom),
a shared household bedtime, live per-lamp state in the GUI. Those are the
next step once this has been seen on a real house.

## 2. Model

`GroupConfig` gains two fields:

```
uint8_t dayActivity;    // 0 none, 1 low, 2 normal, 3 high
uint8_t nightActivity;  // 0 none, 1 rare, 2 normal, 3 often
```

JSON: `"dayActivity": 0..3, "nightActivity": 0..3` on every group. On
load, an absent key takes the value `setPreset(behaviour)` gives, so
existing scenes get the new life at their behaviour's default rather than
staying static. `clamp()` limits both to 3.

`Behaviour` gains four households after `AllNight`, keeping the existing
numbering: `Family`, `Elderly`, `NightOwl`, `Away`. Names in JSON:
`family`, `elderly`, `nightowl`, `away`. GUI labels: Family, Elderly
couple, Night owl, Away.

Presets (fields not listed keep the current defaults of `setPreset`):

| behaviour | on | off | lit % | flicker % | morning | level | fade | day | night |
|---|---|---|---|---|---|---|---|---|---|
| street | unchanged | | | | | | | 0 | 0 |
| home | unchanged | | | | | | | 2 | 1 |
| shop | unchanged | | | | | | | 1 | 0 |
| late | unchanged | | | | | | | 1 | 0 |
| allnight | unchanged | | | | | | | 0 | 0 |
| family | dusk 0..180 | clock 22:30..00:00 (1350..1440) | 90 | 25 | true | 210 | 300 | 3 | 1 |
| elderly | dusk -30..60 | clock 21:00..22:15 (1260..1335) | 90 | 8 | true | 180 | 400 | 2 | 3 |
| nightowl | dusk 60..240 | clock 00:30..02:30 (1470..1590) | 80 | 30 | false | 200 | 300 | 1 | 1 |
| away | clock 19:00..19:10 (1140..1150) | clock 22:30..22:40 (1350..1360) | 25 | 0 | false | 200 | 0 | 0 | 0 |
| off | unchanged | | | | | | | 0 | 0 |

`away` is a timer lamp: one lamp in four, same minutes every evening,
no life at all.

## 3. Engine

### 3.1 Base state, unchanged

`targetLevel()` keeps computing the lamp's on and off moments (`on`,
`off` in minutes, `off` may exceed 1440), participation, the morning
light and the flicker exactly as today. It now also reports the moments
so the activity layer can use them.

### 3.2 Events

Three kinds of event, all per lamp, all of the group's `level` and
`fadeMs`:

| kind | applies when base is | length | rate |
|---|---|---|---|
| day | off, outside the night window | 3..15 min | per lamp per hour, times the daily curve `w(t)` |
| dip | on (and not the morning light) | 2..5 min | per lamp per hour while lit |
| night | off, inside the night window | 1..4 min | expected count per lamp per night |

Rates by level:

| level | day per hour at w=1 | dip per hour | night per night |
|---|---|---|---|
| 0 | 0 | 0 | 0 |
| 1 | 0.15 | 0.08 | 0.4 |
| 2 | 0.35 | 0.15 | 0.8 |
| 3 | 0.70 | 0.30 | 1.6 |

`dayActivity` sets the day and dip rates; `nightActivity` sets the night
rate.

Daily curve `w(t)`, minute of day:

| from | to | w |
|---|---|---|
| 05:30 | 06:30 | 0.4 |
| 06:30 | 08:30 | 1.0 |
| 08:30 | 11:00 | 0.5 |
| 11:00 | 16:00 | 0.3 |
| 16:00 | 21:00 | 1.0 |
| 21:00 | 23:00 | 0.6 |
| 23:00 | 05:30 | 0.1 |

Night window per lamp: from the lamp's off moment `off` (as computed by
the base, whether or not the lamp participates) to `1440 + 330` (05:30
next day), or to the start of the lamp's morning light when `morning`
applies to it, whichever is earlier. Minutes are compared modulo the day
the same way `inWindow()` does.

### 3.3 Determinism: five-minute slots

The day is 288 slots of five minutes. For lamp `L`, day of year `D` and
slot `s`, one hash decides everything about that slot:

```
h = hash(L + 0x10000 * D, 0x500 + s)        // same hash() as the habits
u = (h >> 8) / 2^24                          // 0..1, the draw
offsetMin = h & 0x07 mod 5                   // start minute within the slot
lenUnit   = ((h >> 3) & 0x1F) / 31.0         // 0..1, the length
```

An event starts in slot `s` when `u < p(s)`, where

```
p_day(s)   = dayRate * w(t_s) / 12             (12 slots per hour)
p_dip(s)   = dipRate / 12
p_night(s) = nightRate / (nightLen / 5)        (nightLen in minutes, per lamp)
```

each capped at 0.5. Which formula applies depends on the base state and
the night window at the slot's start `t_s`. Length is
`min + lenUnit * (max - min)` for the kind. At evaluation minute `m`, the
lamp checks the slots covering `m - 15 .. m` (four slots, wrapping across
midnight with `D - 1` for the previous day's slots) and takes the first
event whose interval contains `m`. Events never stack.

Because everything derives from `(seed, lamp, day, slot)`, scrubbing to
03:10 shows exactly the wake-ups of 03:10, scrubbing away and back shows
them again, and accelerated mode gives the same town as real time.

### 3.4 Cost and caching

`targetLevel()` runs 40 times a second per lamp. Events are evaluated
once per simulated minute instead: the engine keeps
`uint32_t _eventOn[LAMPS_MAX_LAMPS / 32]` (256 bytes) and
`uint32_t _eventOff[LAMPS_MAX_LAMPS / 32]` (256 bytes) and recomputes both
bitmaps for every lamp when `_sim` or `_doy` changes (at most 72 times a
real minute in accelerated mode). `targetLevel()` then only reads the
bits: an `_eventOn` bit forces the lamp on at the group's level, an
`_eventOff` bit forces it off. `rebuild()` clears both.

### 3.5 Status

`/api/scene/status` gains `"active": n`, the number of lamps currently in
an event of any kind. Home shows it as "n just changed" only if it is
cheap to add; otherwise it is for the bench and the mock.

## 4. Web API and mock

- `GET /api/scene/presets` includes `dayActivity` and `nightActivity`
  for every behaviour including the four households; the loop still
  starts at 1 (no preset for `off`).
- `GET/PUT /api/scene/config` carries the two fields per group.
- `tools/mockserver.py`: `BEHAVIOUR_PRESETS` gains the four households and
  the two fields on every entry; groups accept and echo the fields; an
  absent field takes the preset value like the firmware. `lit` stays as
  it is; `active` is a small plausible number (0 in deep night, 1..3 in
  the evening).

## 5. GUI (Scene view)

- The behaviour select lists, in this order and with these labels: Home,
  Family, Elderly couple, Night owl, Away, Shop, Pub, late (`late`),
  Street, All night, Off.
- The expanded group gets an "Activity" row below the override fields:
  two selects, *Daytime* (None, Low, Normal, High) bound to `dayActivity`
  and *Night wake-ups* (None, Rare, Normal, Often) bound to
  `nightActivity`, and under them one line of muted text derived from the
  levels: "about N short lights a day per lamp, a wake-up every K nights"
  where N is the day rate at w=1 times 8 (rounded) and K is 1/nightRate
  rounded (`0.4` gives "every second night" wording: use "every 2 nights",
  "every night", "1 or 2 a night" for 0.4, 0.8, 1.6). "No daytime
  activity" and "no wake-ups" for 0.
- Changing the behaviour re-seeds the two fields from the preset like the
  other override fields (`PRESET_KEYS` gains them). `Add group` seeds from
  `home`.
- The group row's summary shows the behaviour label; nothing else changes.
- Size stays within the existing budget (whole page under 40 kB gzipped).

## 6. Verification

- `make` warning-free; the two bitmaps add 512 bytes of globals.
- A host-side check of the slot logic is not required, but the engine's
  event decision must be written as one pure function
  `eventAt(lamp, minute, doy, kind, rate, window...)` so it can be read
  and reasoned about in one place.
- Against the mock: presets carry the new fields; the Activity row
  re-seeds on behaviour change; Save round-trips both fields; an old
  scene without the fields loads with preset values.
- On the board: flash, set a group to Elderly, switch the clock to Manual
  and scrub through 02:00..05:00 in one-minute steps: some lamps of that
  group must show one to four minute lights that are stable when scrubbing
  back; scrub through 07:00..09:00: short lights on the lamps that are
  off; switch to Accelerated at 20 minutes per day and watch Home's lit
  count move outside dusk and dawn. HANDOFF section 5 gets the bench
  item.

## 7. Decisions and reasons

- **Hash per slot, not a running random generator.** The town must be the
  same when scrubbed back and forth and the same in accelerated and real
  time; that is the principle the habits already follow.
- **Once per simulated minute.** Event evaluation is cheap, but not at
  40 Hz for 2048 lamps; two bitmaps make the per-tick cost a bit test.
- **Two levels instead of raw rates in the GUI.** Nobody tunes "0.35 per
  hour"; four words and a sentence that says what they mean are enough,
  and the presets carry the rest.
- **Households as behaviours, not a separate concept.** They fill the
  same fields the editor already exposes, so everything stays tunable
  and the model does not grow a second axis.
