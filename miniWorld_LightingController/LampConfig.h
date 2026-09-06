/*
    LampConfig - which lamp hardware is fitted, and how it is wired.

    This is the one piece of state the web GUI edits. It is stored in
    LittleFS as /lamps.json and applied by Lamps.begin() at boot. Changing it
    at runtime goes through Lamps.apply().

    JSON shape, which is also the GUI contract:

        {
          "hardware":  "none" | "sx1503" | "al5887",
          "devices":   1..12 for sx1503, 1..4 for al5887,
          "busSpeed":  100000 | 400000 | 1000000,
          "activeLow": true | false,     sx1503 only: LED cathode on the IO
          "rgb":       true | false      al5887 only: wired as 12 RGB modules
        }

    Dependencies: ArduinoJson 7, LittleFS (arduino-pico core, set a filesystem
    size in the board menu or every save will fail).

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>

enum class LampHardware : uint8_t {
    None = 0,
    SX1503 = 1,     // single address, one device per bus, on/off
    AL5887 = 2,     // four addresses on one bus, 12-bit PWM
};

struct LampConfig {
    LampHardware hardware = LampHardware::None;
    uint8_t devices = 0;
    uint32_t busSpeed = 400000;
    bool activeLow = false;
    bool rgb = false;

    // Limits imposed by the board and the parts, not by preference.
    uint8_t maxDevices() const;
    uint16_t lampsPerDevice() const;
    uint16_t lampCount() const { return (uint16_t)(devices * lampsPerDevice()); }

    // Pull every field into its legal range. Returns true if nothing changed.
    bool clamp();

    bool operator==(const LampConfig &o) const;
    bool operator!=(const LampConfig &o) const { return !(*this == o); }

    void toJson(String &out) const;
    // On failure, error (if given) says which field and why.
    bool fromJson(const String &in, String *error = nullptr);

    static const char *hardwareName(LampHardware h);
    static bool parseHardware(const char *name, LampHardware &out);
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
