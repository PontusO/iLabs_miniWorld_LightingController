/*
    Scene - what the town does over a day.

    A scene is a set of lamp groups, each with a behaviour, plus a clock and a
    location so dusk and dawn can be computed. This file is the data model
    and its JSON form; SceneEngine is what runs it.

    JSON, which is also the GUI contract:

      {
        "location": { "lat": 55.7, "lon": 13.2,
                      "autoTimezone": true, "utcOffsetMinutes": 60 },
        "clock":    { "mode": "real" | "accelerated" | "manual",
                      "dayMinutes": 20,          real minutes per simulated day
                      "manualTime": "19:40",     or minutes since midnight
                      "dateFromSystem": true,
                      "dayOfYear": 172 },        used when dateFromSystem is false
        "seed": 1,                               changes everyone's habits
        "groups": [
          { "name": "Main street",
            "behaviour": "street" | "home" | "shop" | "late" | "allnight" |
                         "family" | "elderly" | "nightowl" | "away" | "off",
            "lamps": [0, 1, "4-15", 20],
            "dayActivity": 0..3,       short lights and dips through the day
            "nightActivity": 0..3,     wake-ups through the night
            ...optional overrides, see GroupConfig...
          }
        ]
      }

    Times and windows are minutes. A window is two values: the lamp picks
    its own personal moment somewhere between them, and keeps it. Windows
    anchored to dusk or dawn are offsets from that event; windows anchored
    to the clock are absolute minutes since midnight and may exceed 1440 to
    mean "past midnight".

    dayActivity and nightActivity are the "life" layer: how often a lamp pops
    on for a few minutes during the day, dips off for a moment while it is
    lit, and wakes up in the night. An absent key takes the value the
    behaviour's preset gives, so a scene written before the layer existed
    loads with the life its buildings would have had.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include "Lamps.h"

#define SCENE_MAX_GROUPS   16
#define SCENE_NAME_LEN     24

enum class Behaviour : uint8_t {
    Off = 0,        // never lit
    Street,         // on at dusk, off at dawn, near-simultaneous
    Home,           // on some time after dusk, off around bedtime, some flicker
    Shop,           // lit during opening hours
    Late,           // pub, restaurant: on at dusk, off in the small hours
    AllNight,       // on at dusk, off at dawn, always participates
    Family,         // household: late bedtime, television, busy days
    Elderly,        // household: early bedtime, quiet days, restless nights
    NightOwl,       // household: up past midnight, no morning light
    Away,           // timer lamp: the same two moments every evening, no life
    COUNT
};

enum class Anchor : uint8_t {
    Dusk = 0,
    Dawn,
    Clock,
};

struct GroupConfig {
    char name[SCENE_NAME_LEN];
    Behaviour behaviour;

    Anchor onAnchor;
    int16_t onFrom, onTo;
    Anchor offAnchor;
    int16_t offFrom, offTo;

    uint8_t litPercent;         // share of lamps that take part at all
    uint8_t flickerPercent;     // share of lit lamps with a television
    bool morning;               // lights before dawn on winter mornings
    uint8_t level;              // brightness when lit, 0..255
    uint16_t fadeMs;            // real-time fade, 0 = instant

    uint8_t dayActivity;        // daytime lights and dips: 0 none, 1 low, 2 normal, 3 high
    uint8_t nightActivity;      // night wake-ups: 0 none, 1 rare, 2 normal, 3 often

    uint8_t lampBits[LAMPS_MAX_LAMPS / 8];

    void setPreset(Behaviour b);
    bool has(uint16_t lamp) const;
    void add(uint16_t lamp);
    void clearLamps();
    uint16_t lampCount() const;
};

enum class ClockMode : uint8_t {
    Real = 0,
    Accelerated,
    Manual,
};

struct SceneConfig {
    float latitude = 55.7f;
    float longitude = 13.2f;
    bool autoTimezone = true;
    int16_t utcOffsetMinutes = 60;

    ClockMode mode = ClockMode::Real;
    uint16_t dayMinutes = 20;
    uint16_t manualTime = 20 * 60;
    bool dateFromSystem = true;
    uint16_t dayOfYear = 172;

    uint32_t seed = 1;

    uint8_t groupCount = 0;
    GroupConfig groups[SCENE_MAX_GROUPS];

    void clamp();
    void toJson(String &out) const;
    bool fromJson(const String &in, String *error = nullptr);

    static const char *behaviourName(Behaviour b);
    static bool parseBehaviour(const char *s, Behaviour &out);
    static const char *anchorName(Anchor a);
    static bool parseAnchor(const char *s, Anchor &out);
    static const char *modeName(ClockMode m);
    static bool parseMode(const char *s, ClockMode &out);

    // "19:40" or "1180" to minutes; returns false on nonsense.
    static bool parseTime(const char *s, uint16_t &minutes);
    static void formatTime(int minutes, char *out, size_t len);
};

class SceneStore {
public:
    static const char *path() { return "/scene.json"; }
    static bool load(SceneConfig &cfg);
    static bool save(const SceneConfig &cfg);
    static bool erase();
};
