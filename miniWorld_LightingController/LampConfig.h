/*
    LampConfig - which lamp hardware is fitted, and how it is wired.

    This is the one piece of state the web GUI edits. It is stored in
    LittleFS as /lamps.json and applied by Lamps.begin() at boot. Changing it
    at runtime goes through Lamps.apply().

    The controller drives up to LAMPS_NUM_BUSES independent I2C buses. Any
    bus can carry one SX1503 (on/off, address 0x20) and up to
    LAMPS_MAX_AL5887_PER_BUS AL5887 (12-bit PWM, addresses 0x30..0x33), in
    any combination, per HANDOFF.md section 5.1.

    JSON shape, which is also the GUI contract:

        {
          "busSpeed":  100000 | 400000 | 1000000,
          "activeLow": true | false,     sx1503 only: LED cathode on the IO
          "rgb":       true | false,     al5887 only: wired as RGB triplets
          "buses": [
            { "sx1503": true, "al5887": 0 },
            ...12 entries, bus 0 first...
          ]
        }

    Dependencies: ArduinoJson 7, LittleFS (arduino-pico core, set a filesystem
    size in the board menu or every save will fail).

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>

#define LAMPS_NUM_BUSES 12
#define LAMPS_MAX_AL5887_PER_BUS 4

// What is fitted on one bus. Either or both may be present.
struct BusConfig {
    bool sx1503 = false;       // one SX1503 at 0x20, on or off
    uint8_t al5887 = 0;        // 0..LAMPS_MAX_AL5887_PER_BUS, at 0x30, 0x31, ...
};

struct LampConfig {
    uint32_t busSpeed = 400000;
    bool activeLow = false;
    bool rgb = false;
    BusConfig buses[LAMPS_NUM_BUSES];

    // Total lamps: 16 per fitted sx1503, 36 per fitted al5887, summed over buses.
    uint16_t lampCount() const;

    // Total devices (chips), summed over buses.
    uint8_t deviceCount() const;

    // True if any bus has anything fitted.
    bool anyHardware() const;

    // Pull every field into its legal range. Returns true if nothing changed.
    bool clamp();

    bool operator==(const LampConfig &o) const;
    bool operator!=(const LampConfig &o) const { return !(*this == o); }

    void toJson(String &out) const;
    // Merges in whatever fields are present; a field absent from the JSON
    // keeps its current value, including buses (absent means keep the
    // current 12 buses; present as an array replaces all 12, with any
    // index the array does not cover becoming an empty bus). On failure,
    // error (if given) says which field and why.
    bool fromJson(const String &in, String *error = nullptr);
};

class LampConfigStore {
public:
    static const char *path() { return "/lamps.json"; }

    // Returns false if there is no stored config or it did not parse.
    // cfg is left untouched in that case.
    static bool load(LampConfig &cfg);
    static bool save(const LampConfig &cfg);
    static bool erase();
};
