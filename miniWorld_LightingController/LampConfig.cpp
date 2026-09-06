/*
    LampConfig - see LampConfig.h

    Invector Embedded Systems AB
*/

#include "LampConfig.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

// ---------------------------------------------------------------------------

const char *LampConfig::hardwareName(LampHardware h) {
    switch (h) {
        case LampHardware::SX1503: return "sx1503";
        case LampHardware::AL5887: return "al5887";
        default:                   return "none";
    }
}

bool LampConfig::parseHardware(const char *name, LampHardware &out) {
    if (!name) {
        return false;
    }
    if (!strcasecmp(name, "sx1503")) { out = LampHardware::SX1503; return true; }
    if (!strcasecmp(name, "al5887")) { out = LampHardware::AL5887; return true; }
    if (!strcasecmp(name, "none"))   { out = LampHardware::None;   return true; }
    return false;
}

uint8_t LampConfig::maxDevices() const {
    switch (hardware) {
        case LampHardware::SX1503: return 12;   // 2 hw + 7 PIO + 3 bit-bang buses
        case LampHardware::AL5887: return 4;    // addresses 0x30..0x33
        default:                   return 0;
    }
}

uint16_t LampConfig::lampsPerDevice() const {
    switch (hardware) {
        case LampHardware::SX1503: return 16;
        case LampHardware::AL5887: return 36;
        default:                   return 0;
    }
}

bool LampConfig::clamp() {
    LampConfig before = *this;

    uint8_t max = maxDevices();
    if (devices > max) {
        devices = max;
    }
    if (hardware != LampHardware::None && devices == 0) {
        devices = 1;
    }
    if (hardware == LampHardware::None) {
        devices = 0;
    }

    if (busSpeed != 100000 && busSpeed != 400000 && busSpeed != 1000000) {
        busSpeed = 400000;
    }

    // Options that do not apply to the selected hardware are neutralised so
    // the stored file does not carry misleading state.
    if (hardware != LampHardware::SX1503) {
        activeLow = false;
    }
    if (hardware != LampHardware::AL5887) {
        rgb = false;
    }

    return *this == before;
}

bool LampConfig::operator==(const LampConfig &o) const {
    return hardware == o.hardware &&
           devices == o.devices &&
           busSpeed == o.busSpeed &&
           activeLow == o.activeLow &&
           rgb == o.rgb;
}

void LampConfig::toJson(String &out) const {
    JsonDocument doc;
    doc["hardware"] = hardwareName(hardware);
    doc["devices"] = devices;
    doc["busSpeed"] = busSpeed;
    doc["activeLow"] = activeLow;
    doc["rgb"] = rgb;
    out = "";
    serializeJson(doc, out);
}

bool LampConfig::fromJson(const String &in, String *error) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, in);
    if (err) {
        if (error) {
            *error = String("invalid JSON: ") + err.c_str();
        }
        return false;
    }

    LampConfig next = *this;

    if (doc["hardware"].is<const char *>()) {
        if (!parseHardware(doc["hardware"], next.hardware)) {
            if (error) {
                *error = "hardware must be none, sx1503 or al5887";
            }
            return false;
        }
    }
    if (doc["devices"].is<int>()) {
        int d = doc["devices"];
        if (d < 0 || d > 255) {
            if (error) {
                *error = "devices out of range";
            }
            return false;
        }
        next.devices = (uint8_t)d;
    }
    if (doc["busSpeed"].is<uint32_t>()) {
        next.busSpeed = doc["busSpeed"];
    }
    if (doc["activeLow"].is<bool>()) {
        next.activeLow = doc["activeLow"];
    }
    if (doc["rgb"].is<bool>()) {
        next.rgb = doc["rgb"];
    }

    // Report rather than silently fix a device count the hardware cannot do.
    if (next.hardware != LampHardware::None && next.devices > next.maxDevices()) {
        if (error) {
            *error = String("devices: ") + hardwareName(next.hardware) +
                     " supports at most " + next.maxDevices();
        }
        return false;
    }

    next.clamp();
    *this = next;
    return true;
}

// ---------------------------------------------------------------------------

static bool fsReady() {
    static bool begun = false;
    if (!begun) {
        begun = LittleFS.begin();
    }
    return begun;
}

bool LampConfigStore::load(LampConfig &cfg) {
    if (!fsReady()) {
        return false;
    }
    File f = LittleFS.open(path(), "r");
    if (!f) {
        return false;
    }
    String body = f.readString();
    f.close();

    LampConfig parsed;
    if (!parsed.fromJson(body)) {
        return false;
    }
    cfg = parsed;
    return true;
}

bool LampConfigStore::save(const LampConfig &cfg) {
    if (!fsReady()) {
        return false;
    }
    String body;
    cfg.toJson(body);

    File f = LittleFS.open(path(), "w");
    if (!f) {
        return false;
    }
    size_t n = f.print(body);
    f.close();
    return n == body.length();
}

bool LampConfigStore::erase() {
    if (!fsReady()) {
        return false;
    }
    return LittleFS.remove(path());
}
