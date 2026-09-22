/*
    SceneEngine - runs a Scene against a clock and drives the Lamps API.

    A unit follows its model, and the model says which of the two
    simulations runs it.

    Under an hours model with "individual", every lamp of the unit gets a
    personal habit: its own moment inside the model's on-window and
    off-window, whether it takes part at all, and whether there is a
    television in the room. Habits are derived from a hash of the lamp index
    and the scene seed, so they are stable from day to day and from boot to
    boot. A small daily jitter keeps evenings from being identical. Under an
    hours model without it the room, not the lamp, makes that draw, so a
    street switches in one sweep.

    On top of the habits sits the activity layer: short lights during the
    day, brief dips while a lamp is lit, and wake-ups at night. The day is
    cut into 288 slots of five minutes and one hash of seed, key, day and
    slot decides whether an event starts in that slot, where inside it, and
    how long it runs. Scrubbing the clock away and back therefore shows the
    same town twice, and accelerated time shows the same town as real time.
    Events are worked out once per simulated minute into two bitmaps; the
    per-lamp path in tick() only tests a bit.

    A rhythm model is the other way round. The household, not the lamp, has
    the habit: one rhythm of wake, leave, come home, dinner and bed is drawn
    once a day per unit, and the rooms of the unit light in the natural
    order around those moments, clipped so nobody switches the living room
    on at 19:00 in June. The life layer runs on those lamps too, with rates
    scaled by the room, so the loo gets the night visits and the kitchen the
    coffee. Units are worked out into two more bitmaps on the same schedule
    as the events, and a lamp belongs to the first unit that lists it.

    The room, not the lamp, is what behaves everywhere except under an
    individual hours model. Every draw a room makes, the minutes around its
    moments and its life events alike, is taken from the room's own key
    rather than from a lamp index, so all the lamps of a room switch
    together and adding one does not reshuffle the rest of the unit.

    Alongside the simulation sits identify(): up to eight lamps asked for by
    the GUI blink together for a second and a quarter so a lamp, or a whole
    room, can be found on the layout. The blink runs through the engine
    rather than around it, so the scene never fights it: tick() steps the
    phases and skips those lamps in the per-lamp loop while they blink.

    Call tick() from loop() as often as you like; it rate-limits itself.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include "Scene.h"

// What targetLevel() works out on the way to a level. The activity layer
// needs these moments for lamps that are dark now, and even for lamps that
// never take part, so they are computed before any of the early exits.
struct LampMoments {
    int on = 0;                     // personal on moment, minutes
    int off = 0;                    // personal off moment, may exceed 1440
    int morningOn = 0;              // start of the morning light, minutes
    int morningOff = 0;             // end of it
    bool morningApplies = false;    // this lamp is one of the early risers
    bool morningLit = false;        // the morning light is what has it lit now
};

// One day in the life of one unit with a rhythm model, drawn once when the
// day changes. Minutes since midnight on the same 0..2880 line the hours
// windows use; -1 means "no such moment today", which is what the away
// household has for all of them and what a weekday departure has on a
// Saturday.
struct UnitDay {
    int16_t wake = -1;
    int16_t leave = -1;             // -1 on a weekend, or "never leaves"
    int16_t home = -1;
    int16_t dinner = -1;
    int16_t bed = -1;
    int16_t ret = -1;               // back home on an evening out
    int16_t outLeave = -1;          // the weekend afternoon out, if any
    int16_t outHome = -1;
    bool out = false;               // this evening is spent out
    bool tv = false;                // there is television in the living room
    bool weekend = false;           // the weekend rhythm applies today
};

class SceneEngine {
public:
    // Load /scene.json and start. With nothing stored, runs an empty scene.
    bool begin();

    // Replace the running scene. With persist, it becomes the boot scene.
    bool apply(const SceneConfig &cfg, bool persist);

    const SceneConfig &config() const { return _cfg; }

    // Advance the simulation and push changes to the lamps.
    void tick();

    // Pause the engine without losing state. Lamps keep their last level.
    void setEnabled(bool on) { _enabled = on; }
    bool enabled() const { return _enabled; }

    // Blink lamps so they can be found on the layout: five phases of 250 ms,
    // on, off, on, off, on, and then every lamp goes back to what it was.
    // Nothing here waits: tick() drives the phases from millis(), and it
    // drives them whether or not the engine is enabled, so a lamp can be
    // found with the scene paused. Up to IDENT_MAX_LAMPS lamps blink
    // together, which is the first eight lamps of a room; one blink at a
    // time, and a second call gives every lamp of the first one back at
    // once and starts over. Lamps outside the fitted range are ignored, and
    // a call with nothing left in it does not disturb a blink already
    // running.
    static constexpr uint8_t IDENT_MAX_LAMPS = 8;
    void identify(const uint16_t *lamps, uint8_t n);
    void identify(uint16_t lamp) { identify(&lamp, 1); }

    // Clock control that does not touch flash. Used by the scrubber.
    void setMode(ClockMode m);
    void setManualTime(uint16_t minutes);
    void setDayMinutes(uint16_t minutes);
    void setDayOfYear(uint16_t doy);        // also turns off dateFromSystem

    // What the simulation thinks right now
    uint16_t simMinutes() const { return _sim; }
    uint16_t dayOfYear() const { return _doy; }
    int dusk() const { return _dusk; }      // minutes, civil
    int dawn() const { return _dawn; }
    int sunset() const { return _sunset; }
    int sunrise() const { return _sunrise; }
    bool clockValid() const { return _clockValid; }
    uint16_t litCount() const { return _lit; }
    uint16_t activeCount() const { return _active; }    // lamps in an event

    // What a unit is doing right now, for the status API and the GUI. A
    // rhythm unit is "asleep", "out", "awake", or "away" for the away
    // household. An hours unit is "open" or "closed" when its model is
    // clock anchored at both ends and "lit" or "dark" otherwise.
    const char *unitState(uint8_t unit) const;
    // One letter per role that has at least one lit room, in role order and
    // never repeated, whatever order the unit lists its rooms in: l living,
    // k kitchen, b bedroom, t bathroom, h hall, f front, r back, s sign,
    // o other. Events count as lit. len must be at least Room::COUNT + 1.
    void unitLitRooms(uint8_t unit, char *out, size_t len) const;
    // 0xFF when no unit lists this lamp. The first unit listing it wins.
    uint8_t unitOwner(uint16_t lamp) const {
        return lamp < LAMPS_MAX_LAMPS ? _lampUnit[lamp] : 0xFF;
    }
    // Why the stored scene did not load at boot, and an empty string when
    // there was nothing stored or it loaded. The console says this once;
    // the status API repeats it, because a board that came up with an empty
    // town should say so in the GUI and not only on a cable.
    const char *loadError() const { return _loadError; }

private:
    void rebuild();
    void updateClock(uint32_t nowMs);
    void recomputeSun();
    int localOffsetMinutes() const;

    uint32_t hash(uint32_t a, uint32_t b) const;
    // The key is a lamp index for an individual hours lamp and a room key
    // otherwise, which is why it is wider than a lamp.
    float unit(uint32_t key, uint32_t salt) const;
    float dayUnit(uint32_t key, uint32_t salt) const;
    // Per-unit, per-day value in [0, 1). Salts 1..8 are the day's draws.
    float unitUnit(uint8_t unit, uint16_t doy, uint32_t salt) const;

    void lampMoments(uint32_t key, const ModelConfig &M, LampMoments &m) const;
    uint16_t targetLevel(uint32_t key, const ModelConfig &M, bool &flicker,
                         LampMoments &m);
    static bool inWindow(int t, int on, int off);
    int anchorBase(Anchor a, bool forOff) const;

    // Which event, if any, covers minute m for this key: a lamp index for
    // an individual hours lamp, a room key for a room, so a room's lamps
    // share one event. kind: 0 day, 1 dip, 2 night. Returns true when an
    // event of that kind is active at m. windowFrom and windowTo are the
    // night window, used only by the night kind, whose rate is spread over
    // that window.
    bool eventAt(uint32_t key, int m, uint16_t doy, uint8_t kind, float rate,
                 int windowFrom, int windowTo) const;

    void evaluateEvents();

    // Units. The day is drawn once per rhythm unit when the date changes;
    // the two bitmaps are rebuilt whenever the events are.
    uint8_t weekday(uint16_t doy) const;
    void computeUnitDay(uint8_t unit, uint16_t doy, UnitDay &d) const;
    bool unitOut(const UnitDay &d, int t) const;
    // One interval of the room table. gateFrom is the earliest an evening
    // interval may start and gateTo the latest a morning interval may end;
    // -1 for either means the interval is not clipped by daylight.
    static bool span(int t, int from, int to, int gateFrom, int gateTo);
    // Asked once per room, with the room's key, so every lamp of the room
    // gets the same answer at the same minute.
    bool roomLit(const UnitDay &d, uint32_t key, Room role, int t, bool out,
                 bool &tv) const;
    // The answer a room drew, handed to every lamp the room still owns.
    void litRoom(uint8_t unitIndex, const RoomConfig &R, bool tv);
    void evaluateUnits();
    void evaluateUnitEvents(int m);

    // The identify blink, one phase at a time. Writes to the lamps only when
    // the phase changes, so calling it from every loop() costs a compare.
    void driveIdentify(uint32_t nowMs);
    // Stop blinking: every lamp gets the level it had back and the scene
    // carries on fading it from there. A lamp that is no longer fitted is
    // simply let go.
    void releaseIdentify();
    // Is this lamp blinking? Nothing blinking is no compares at all, which
    // is the case the per-lamp loop is in almost always.
    bool identifying(uint16_t lamp) const {
        for (uint8_t i = 0; i < _identCount; i++) {
            if (_identLamp[i] == lamp) {
                return true;
            }
        }
        return false;
    }

    SceneConfig _cfg;
    bool _enabled = true;
    // A fixed array and not a String: this is written once at boot and read
    // by every status request, and a String member would grow the heap for
    // a message that is never longer than a filename and a reason.
    char _loadError[64] = { 0 };

    uint8_t _lampUnit[LAMPS_MAX_LAMPS];     // 0xFF = not in any unit
    uint16_t _current[LAMPS_MAX_LAMPS];     // level actually sent
    uint16_t _target[LAMPS_MAX_LAMPS];      // level we are fading toward
    uint32_t _flickerNext[LAMPS_MAX_LAMPS / 32];   // bitmask: flicker lamps
    uint32_t _eventOn[LAMPS_MAX_LAMPS / 32];       // bitmask: forced on by an event
    uint32_t _eventOff[LAMPS_MAX_LAMPS / 32];      // bitmask: forced off by a dip

    uint32_t _unitLit[LAMPS_MAX_LAMPS / 32];       // bitmask: room lit by its unit
    uint32_t _unitTv[LAMPS_MAX_LAMPS / 32];        // bitmask: television in it
    UnitDay _unitDay[SCENE_MAX_UNITS];
    int _unitDoy = -1;              // day the unit days were drawn for

    uint16_t _sim = 0;
    uint16_t _doy = 172;
    int _dusk = 1260, _dawn = 300, _sunset = 1230, _sunrise = 330;
    bool _clockValid = false;
    uint16_t _lit = 0;
    uint16_t _active = 0;
    uint16_t _eventSim = 0xFFFF;    // simulated minute the bitmaps were built for
    uint16_t _eventDoy = 0;

    uint16_t _identLamp[IDENT_MAX_LAMPS];   // the lamps being blinked
    uint16_t _identSaved[IDENT_MAX_LAMPS];  // the levels to give back after
    uint8_t _identCount = 0;        // how many of them, 0 for no blink
    uint32_t _identStart = 0;       // millis() the blink started at
    uint8_t _identPhase = 0xFF;     // phase last written, 0xFF = none yet

    uint32_t _startMs = 0;          // accelerated: when the sim day started
    uint32_t _lastTickMs = 0;
    uint32_t _lastFlickerMs = 0;
    uint32_t _rng = 0x2545F491;
    int _sunForDoy = -1;
};

extern SceneEngine Scene;
