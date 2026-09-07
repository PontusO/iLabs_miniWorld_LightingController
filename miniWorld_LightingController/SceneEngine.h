/*
    SceneEngine - runs a Scene against a clock and drives the Lamps API.

    Every lamp in a group gets a personal habit: its own moment inside the
    group's on-window and off-window, whether it takes part at all, and
    whether there is a television in the room. Habits are derived from a
    hash of the lamp index and the scene seed, so they are stable from day
    to day and from boot to boot. A small daily jitter keeps evenings from
    being identical.

    On top of the habits sits the activity layer: short lights during the
    day, brief dips while a lamp is lit, and wake-ups at night. The day is
    cut into 288 slots of five minutes and one hash of seed, lamp, day and
    slot decides whether an event starts in that slot, where inside it, and
    how long it runs. Scrubbing the clock away and back therefore shows the
    same town twice, and accelerated time shows the same town as real time.
    Events are worked out once per simulated minute into two bitmaps; the
    per-lamp path in tick() only tests a bit.

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

private:
    void rebuild();
    void updateClock(uint32_t nowMs);
    void recomputeSun();
    int localOffsetMinutes() const;

    uint32_t hash(uint32_t a, uint32_t b) const;
    float unit(uint16_t lamp, uint32_t salt) const;
    float dayUnit(uint16_t lamp, uint32_t salt) const;

    void lampMoments(uint16_t lamp, const GroupConfig &G, LampMoments &m) const;
    uint16_t targetLevel(uint16_t lamp, const GroupConfig &G, bool &flicker,
                         LampMoments &m);
    static bool inWindow(int t, int on, int off);
    int anchorBase(Anchor a, bool forOff) const;

    // Which event, if any, covers minute m for this lamp. kind: 0 day, 1 dip,
    // 2 night. Returns true when an event of that kind is active at m.
    // windowFrom and windowTo are the lamp's night window, used only by the
    // night kind, whose rate is spread over the length of that window.
    bool eventAt(uint16_t lamp, int m, uint16_t doy, uint8_t kind, float rate,
                 int windowFrom, int windowTo) const;

    void evaluateEvents();

    SceneConfig _cfg;
    bool _enabled = true;

    uint8_t _lampGroup[LAMPS_MAX_LAMPS];    // 0xFF = not in any group
    uint16_t _current[LAMPS_MAX_LAMPS];     // level actually sent
    uint16_t _target[LAMPS_MAX_LAMPS];      // level we are fading toward
    uint32_t _flickerNext[LAMPS_MAX_LAMPS / 32];   // bitmask: flicker lamps
    uint32_t _eventOn[LAMPS_MAX_LAMPS / 32];       // bitmask: forced on by an event
    uint32_t _eventOff[LAMPS_MAX_LAMPS / 32];      // bitmask: forced off by a dip

    uint16_t _sim = 0;
    uint16_t _doy = 172;
    int _dusk = 1260, _dawn = 300, _sunset = 1230, _sunrise = 330;
    bool _clockValid = false;
    uint16_t _lit = 0;
    uint16_t _active = 0;
    uint16_t _eventSim = 0xFFFF;    // simulated minute the bitmaps were built for
    uint16_t _eventDoy = 0;

    uint32_t _startMs = 0;          // accelerated: when the sim day started
    uint32_t _lastTickMs = 0;
    uint32_t _lastFlickerMs = 0;
    uint32_t _rng = 0x2545F491;
    int _sunForDoy = -1;
};

extern SceneEngine Scene;
