/*
    Scene - what the town does over a day.

    A scene is a list of models, a list of units, a clock and a location so
    dusk and dawn can be computed. A model is a named way of behaving and
    owns no lamps; a unit is a thing on the layout: a name, a building
    label, a model and rooms of lamps. This file is the data model and its
    JSON form; SceneEngine is what runs it.

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
        "models": [
          { "name": "Elderly couple",            unique, case-insensitively
            "kind": "rhythm" | "hours",
            "level": 180, "fadeMs": 400,
            "dayActivity": 0..3,       short lights and dips through the day
            "nightActivity": 0..3,     wake-ups through the night

            rhythm only, the household's day in minutes since midnight:
            "weekend": true,           weekend rhythm on Saturday and Sunday
            "wake":  [345, 390],       a negative from means "no such moment"
            "leave": [570, 630],
            "home":  [720, 810],
            "bed":   [1275, 1335],     may exceed 1440
            "outPercent": 7,           evenings out
            "tvPercent": 40,           evenings with television

            hours only:
            "onAnchor": "dusk" | "dawn" | "clock", "on": [0, 240],
            "offAnchor": "clock", "off": [1320, 1470],
            "litPercent": 85,          share of lamps that take part at all
            "flickerPercent": 12,      share of lit lamps with a television
            "morning": true,           lights before dawn on winter mornings
            "individual": true }       every lamp keeps its own moment
        ],
        "units": [
          { "name": "Andersson",
            "building": "Storgatan 3", GUI grouping only, may be empty
            "model": "Family",         a model name, not an index
            "rooms": [ { "lamps": [4, "6-8"], "role": "kitchen" } ] }
        ]
      }

    Times and windows are minutes. A window is two values: the lamp, or the
    room, picks its own personal moment somewhere between them and keeps
    it. Windows anchored to dusk or dawn are offsets from that event;
    windows anchored to the clock are absolute minutes since midnight and
    may exceed 1440 to mean "past midnight".

    dayActivity and nightActivity are the "life" layer: how often a lamp
    pops on for a few minutes during the day, dips off for a moment while it
    is lit, and wakes up in the night. An absent key takes the value the
    template gives, so a model written before the layer existed loads with
    the life its buildings would have had.

    The two kinds are the two simulations the engine has. A rhythm model is
    a household: one day of wake, leave, come home, dinner and bed, with the
    rooms of the unit lighting in the natural order around those moments. An
    hours model is a shop, a street or a block of windows: an on window and
    an off window, with a share of the lamps taking part at all.
    "individual" is only on an hours model: true means every lamp keeps its
    own moment inside the window and its own television, false means a room
    draws once and all its lamps switch together. A rhythm model always
    draws per room. A rhythm model with no wake and no bed is the away
    household: a timer lamp in the evening and no life on top.

    A room carries up to eight lamp ranges and the lamps of one room behave
    as one: the room draws its intervals and its life events once and every
    lamp in it gets the same bits, so a ceiling light and two wall lamps are
    one room and not three. Ranges are written the way a person would, as
    single numbers and "a-b" strings, so a street of sixty lamps is one
    room; the old one-lamp form "lamp": n still loads. A run that carries on
    from the one before it is merged onto it, so [0, "1-3"] is stored, and
    written back, as ["0-3"]: the eight are eight runs of addresses and not
    eight ways of writing them. A lamp belongs to one
    unit only and the first unit listing it wins. Model names are resolved
    to indices on load, so renaming a model has to rename it in the units
    that use it in the same save.

    A document may carry either list, both or neither. Only "units" applies
    them against the models already held. Only "models" re-resolves the
    units held by the model names they were following, and drops a unit
    whose model name is no longer in the list, so a rename that does not
    send the units drops the units that followed the old name. Neither
    leaves both lists as they were. A document whose units name a model the
    document does not keep is refused with "unknown model", which is what
    stops a save from deleting a model still in use.

    Groups and flats, which models and units replaced, are still read: a
    document with "groups" or "flats" and no "models" is migrated on load,
    and the next save writes the new shape. There is no path back.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include "Lamps.h"

#define SCENE_NAME_LEN     24
#define SCENE_MAX_MODELS   16
// Twenty-four and not thirty-two: a unit is about 460 bytes, the five
// static SceneConfig copies carry all of them, and thirty-two put the
// board at 41 % of its RAM against the 39 % it had before models and units.
#define SCENE_MAX_UNITS    24
#define UNIT_MAX_ROOMS     12
#define ROOM_MAX_RANGES     8

// The two simulations. A model is one or the other and carries the fields
// of both, so switching kind in the GUI never loses what was typed.
enum class ModelKind : uint8_t {
    Rhythm = 0,     // a household: one daily rhythm, rooms in order
    Hours,          // a shop, a street, a block of windows: on and off windows
    COUNT
};

// The models a fresh device comes up with, and the list the GUI offers
// under "New model, start from". The first four are rhythms, the last five
// are hours.
enum class Template : uint8_t {
    Family = 0,
    Elderly,
    NightOwl,
    Away,
    Home,
    Shop,
    Pub,
    Street,
    AllNight,
    COUNT
};

enum class Anchor : uint8_t {
    Dusk = 0,
    Dawn,
    Clock,
};

// The rooms a unit can have. This order, not the unit's own room list, is
// the order of the letters in the status "lit" string, and it is the order
// of the rate tables in the engine.
enum class Room : uint8_t {
    Living = 0,
    Kitchen,
    Bedroom,
    Bathroom,
    Hall,
    Front,
    Back,
    Sign,
    Other,
    COUNT
};

// One run of lamps, both ends included.
struct LampRange {
    uint16_t from, to;
};

// One room of a unit: the lamps that light together and what the room is
// for. Every lamp here gets the same intervals and the same events, so the
// ranges are one room and not several.
struct RoomConfig {
    LampRange ranges[ROOM_MAX_RANGES];
    uint8_t rangeCount;
    Room role;

    bool has(uint16_t lamp) const;
    bool add(uint16_t from, uint16_t to);   // false when the room is full
    uint16_t count() const;                 // lamps over all the ranges
    uint16_t lamp(uint16_t i) const;        // the i-th lamp, 0xFFFF past the end
    uint16_t first() const;                 // 0xFFFF when the room is empty
};

// A named way of behaving. No lamps: a unit says which model it follows.
struct ModelConfig {
    char name[SCENE_NAME_LEN];          // "Elderly couple", unique in the scene
    ModelKind kind;

    uint8_t level;                      // brightness when lit, 0..255
    uint16_t fadeMs;                    // real-time fade, 0 = instant
    uint8_t dayActivity;                // daytime lights and dips: 0 none, 1 low, 2 normal, 3 high
    uint8_t nightActivity;              // night wake-ups: 0 none, 1 rare, 2 normal, 3 often

    // The rhythm half. Minutes since midnight; a range is [from, to] and
    // the day draws one moment inside it. A negative from means "no such
    // moment": no wake and no bed together is the away household.
    bool weekend;                       // weekend rhythm on Saturday and Sunday
    int16_t wakeFrom, wakeTo;
    int16_t leaveFrom, leaveTo;
    int16_t homeFrom, homeTo;
    int16_t bedFrom, bedTo;             // may exceed 1440
    uint8_t outPercent;                 // evenings out, 0..100
    uint8_t tvPercent;                  // evenings with television, 0..100

    // The hours half.
    Anchor onAnchor;
    int16_t onFrom, onTo;
    Anchor offAnchor;
    int16_t offFrom, offTo;
    uint8_t litPercent;                 // share of lamps that take part at all
    uint8_t flickerPercent;             // share of lit lamps with a television
    bool morning;                       // lights before dawn on winter mornings
    bool individual;                    // every lamp keeps its own moment

    // Both halves, so every field is defined whatever the kind. The name is
    // cleared: the caller names the model.
    void setTemplate(Template t);
    // The away household: a rhythm with nowhere to be and no bedtime.
    bool isAway() const;
};

// A thing on the layout. The model is an index into SceneConfig::models;
// the JSON carries the model's name.
struct UnitConfig {
    char name[SCENE_NAME_LEN];          // "Andersson"
    char building[SCENE_NAME_LEN];      // "Storgatan 3", GUI grouping only
    uint8_t model;
    uint8_t roomCount;
    RoomConfig rooms[UNIT_MAX_ROOMS];
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

    uint8_t modelCount = 0;
    ModelConfig models[SCENE_MAX_MODELS];

    uint8_t unitCount = 0;
    UnitConfig units[SCENE_MAX_UNITS];

    // The nine templates and no units, which is what a fresh device runs.
    void seedTemplates();
    // The model with this name, compared case-insensitively; -1 when there
    // is none. This is how a unit's "model" key is resolved.
    int findModel(const char *name) const;

    void clamp();
    void toJson(String &out) const;
    bool fromJson(const String &in, String *error = nullptr);

    static const char *kindName(ModelKind k);
    static bool parseKind(const char *s, ModelKind &out);
    static const char *templateKey(Template t);     // "elderly"
    static const char *templateTitle(Template t);   // "Elderly couple"
    static bool parseTemplate(const char *s, Template &out);
    static const char *anchorName(Anchor a);
    static bool parseAnchor(const char *s, Anchor &out);
    static const char *modeName(ClockMode m);
    static bool parseMode(const char *s, ClockMode &out);
    static const char *roomName(Room r);
    static bool parseRoom(const char *s, Room &out);

    // "19:40" or "1180" to minutes; returns false on nonsense.
    static bool parseTime(const char *s, uint16_t &minutes);
    static void formatTime(int minutes, char *out, size_t len);
};

class SceneStore {
public:
    static const char *path() { return "/scene.json"; }
    // Is there a stored scene at all? This, and not "the running scene has
    // no models", is what says a device has never been set up: a stored
    // scene that will not load must not be written over.
    static bool exists();
    // With error, why the stored scene would not load, which the boot log
    // prints rather than swallowing.
    static bool load(SceneConfig &cfg, String *error = nullptr);
    static bool save(const SceneConfig &cfg);
    static bool erase();
};
