/*
    LampConfig - see LampConfig.h

    Invector Embedded Systems AB
*/

#include "LampConfig.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

// ---------------------------------------------------------------------------

uint16_t LampConfig::lampCount() const {
    uint32_t total = 0;
    for (uint8_t i = 0; i < LAMPS_NUM_BUSES; i++) {
        total += (uint32_t)(16 * buses[i].sx1503) + (uint32_t)(36 * buses[i].al5887);
    }
    return (uint16_t)total;
}

uint8_t LampConfig::deviceCount() const {
    uint16_t total = 0;
    for (uint8_t i = 0; i < LAMPS_NUM_BUSES; i++) {
        total = (uint16_t)(total + (buses[i].sx1503 ? 1 : 0) + buses[i].al5887);
    }
    return (uint8_t)total;
}

bool LampConfig::anyHardware() const {
    for (uint8_t i = 0; i < LAMPS_NUM_BUSES; i++) {
        if (buses[i].sx1503 || buses[i].al5887) {
            return true;
        }
    }
    return false;
}

bool LampConfig::clamp() {
    LampConfig before = *this;

    for (uint8_t i = 0; i < LAMPS_NUM_BUSES; i++) {
        if (buses[i].al5887 > LAMPS_MAX_AL5887_PER_BUS) {
            buses[i].al5887 = LAMPS_MAX_AL5887_PER_BUS;
        }
    }

    if (busSpeed != 100000 && busSpeed != 400000 && busSpeed != 1000000) {
        busSpeed = 400000;
    }

    return *this == before;
}

bool LampConfig::operator==(const LampConfig &o) const {
    if (busSpeed != o.busSpeed || activeLow != o.activeLow || rgb != o.rgb) {
        return false;
    }
    for (uint8_t i = 0; i < LAMPS_NUM_BUSES; i++) {
        if (buses[i].sx1503 != o.buses[i].sx1503 || buses[i].al5887 != o.buses[i].al5887) {
            return false;
        }
    }
    return true;
}

void LampConfig::toJson(String &out) const {
    JsonDocument doc;
    doc["busSpeed"] = busSpeed;
    doc["activeLow"] = activeLow;
    doc["rgb"] = rgb;

    JsonArray arr = doc["buses"].to<JsonArray>();
    for (uint8_t i = 0; i < LAMPS_NUM_BUSES; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["sx1503"] = buses[i].sx1503;
        o["al5887"] = buses[i].al5887;
    }

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

    if (doc["busSpeed"].is<uint32_t>()) {
        next.busSpeed = doc["busSpeed"];
    }
    if (doc["activeLow"].is<bool>()) {
        next.activeLow = doc["activeLow"];
    }
    if (doc["rgb"].is<bool>()) {
        next.rgb = doc["rgb"];
    }

    // buses follows the same merge rule as busSpeed/activeLow/rgb: absent
    // (or explicit null) keeps the existing buses untouched. Present as an
    // array replaces all 12 entries: any index the array does not cover
    // becomes an empty bus. Present as anything else is an error.
    JsonVariant busesField = doc["buses"];
    if (!busesField.isNull()) {
        if (!busesField.is<JsonArray>()) {
            if (error) {
                *error = "buses must be an array";
            }
            return false;
        }
        JsonArray arr = busesField.as<JsonArray>();
        if (arr.size() > LAMPS_NUM_BUSES) {
            if (error) {
                *error = "buses: at most 12 entries";
            }
            return false;
        }
        for (uint8_t i = 0; i < LAMPS_NUM_BUSES; i++) {
            next.buses[i] = BusConfig();
        }
        uint8_t i = 0;
        for (JsonVariant v : arr) {
            BusConfig b;
            if (v["sx1503"].is<bool>()) {
                b.sx1503 = v["sx1503"];
            }
            if (v["al5887"].is<int>()) {
                int n = v["al5887"];
                if (n < 0 || n > LAMPS_MAX_AL5887_PER_BUS) {
                    if (error) {
                        *error = String("buses[") + i + "].al5887 must be 0..4";
                    }
                    return false;
                }
                b.al5887 = (uint8_t)n;
            }
            next.buses[i] = b;
            i++;
        }
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
