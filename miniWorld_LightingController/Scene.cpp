/*
    Scene - see Scene.h

    Invector Embedded Systems AB
*/

#include "Scene.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Group presets. These are the "habits" of each kind of building. Everything
// here can be overridden per group in JSON, but the defaults are meant to be
// plausible straight out of the box.
// ---------------------------------------------------------------------------

void GroupConfig::setPreset(Behaviour b) {
    behaviour = b;
    litPercent = 100;
    flickerPercent = 0;
    morning = false;
    level = 255;
    fadeMs = 800;

    switch (b) {
        case Behaviour::Street:
            onAnchor = Anchor::Dusk;   onFrom = -10;  onTo = 5;
            offAnchor = Anchor::Dawn;  offFrom = -5;  offTo = 15;
            fadeMs = 4000;              // sodium warm-up
            break;

        case Behaviour::Home:
            onAnchor = Anchor::Dusk;   onFrom = 0;    onTo = 240;
            offAnchor = Anchor::Clock; offFrom = 1320; offTo = 1470;   // 22:00 to 00:30
            litPercent = 85;
            flickerPercent = 12;
            morning = true;
            level = 200;
            fadeMs = 300;
            break;

        case Behaviour::Shop:
            onAnchor = Anchor::Clock;  onFrom = 510;  onTo = 540;      // 08:30 to 09:00
            offAnchor = Anchor::Clock; offFrom = 1080; offTo = 1110;   // 18:00 to 18:30
            level = 230;
            fadeMs = 200;
            break;

        case Behaviour::Late:
            onAnchor = Anchor::Dusk;   onFrom = -30;  onTo = 30;
            offAnchor = Anchor::Clock; offFrom = 1380; offTo = 1560;   // 23:00 to 02:00
            level = 220;
            break;

        case Behaviour::AllNight:
            onAnchor = Anchor::Dusk;   onFrom = -15;  onTo = 0;
            offAnchor = Anchor::Dawn;  offFrom = 0;   offTo = 15;
            break;

        default:
            onAnchor = Anchor::Clock;  onFrom = 0;    onTo = 0;
            offAnchor = Anchor::Clock; offFrom = 0;   offTo = 0;
            litPercent = 0;
            level = 0;
            break;
    }
}

bool GroupConfig::has(uint16_t lamp) const {
    if (lamp >= LAMPS_MAX_LAMPS) {
        return false;
    }
    return (lampBits[lamp >> 3] >> (lamp & 7)) & 1;
}

void GroupConfig::add(uint16_t lamp) {
    if (lamp < LAMPS_MAX_LAMPS) {
        lampBits[lamp >> 3] |= (uint8_t)(1u << (lamp & 7));
    }
}

void GroupConfig::clearLamps() {
    memset(lampBits, 0, sizeof(lampBits));
}

uint16_t GroupConfig::lampCount() const {
    uint16_t n = 0;
    for (size_t i = 0; i < sizeof(lampBits); i++) {
        uint8_t b = lampBits[i];
        while (b) {
            n += (b & 1);
            b >>= 1;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

static const char *const behaviourNames[] = {
    "off", "street", "home", "shop", "late", "allnight"
};
static const char *const anchorNames[] = { "dusk", "dawn", "clock" };
static const char *const modeNames[] = { "real", "accelerated", "manual" };

const char *SceneConfig::behaviourName(Behaviour b) {
    uint8_t i = (uint8_t)b;
    return i < (uint8_t)Behaviour::COUNT ? behaviourNames[i] : "off";
}

bool SceneConfig::parseBehaviour(const char *s, Behaviour &out) {
    if (!s) return false;
    for (uint8_t i = 0; i < (uint8_t)Behaviour::COUNT; i++) {
        if (!strcasecmp(s, behaviourNames[i])) {
            out = (Behaviour)i;
            return true;
        }
    }
    return false;
}

const char *SceneConfig::anchorName(Anchor a) {
    return anchorNames[(uint8_t)a < 3 ? (uint8_t)a : 2];
}

bool SceneConfig::parseAnchor(const char *s, Anchor &out) {
    if (!s) return false;
    for (uint8_t i = 0; i < 3; i++) {
        if (!strcasecmp(s, anchorNames[i])) {
            out = (Anchor)i;
            return true;
        }
    }
    return false;
}

const char *SceneConfig::modeName(ClockMode m) {
    return modeNames[(uint8_t)m < 3 ? (uint8_t)m : 0];
}

bool SceneConfig::parseMode(const char *s, ClockMode &out) {
    if (!s) return false;
    for (uint8_t i = 0; i < 3; i++) {
        if (!strcasecmp(s, modeNames[i])) {
            out = (ClockMode)i;
            return true;
        }
    }
    return false;
}

bool SceneConfig::parseTime(const char *s, uint16_t &minutes) {
    if (!s || !*s) return false;
    const char *colon = strchr(s, ':');
    long v;
    if (colon) {
        long h = strtol(s, nullptr, 10);
        long m = strtol(colon + 1, nullptr, 10);
        if (h < 0 || h > 47 || m < 0 || m > 59) return false;
        v = h * 60 + m;
    } else {
        v = strtol(s, nullptr, 10);
        if (v < 0 || v > 2879) return false;
    }
    minutes = (uint16_t)v;
    return true;
}

void SceneConfig::formatTime(int minutes, char *out, size_t len) {
    if (minutes < 0) {
        snprintf(out, len, "--:--");
        return;
    }
    snprintf(out, len, "%02d:%02d", (minutes / 60) % 48, minutes % 60);
}

// ---------------------------------------------------------------------------

void SceneConfig::clamp() {
    if (latitude > 89.0f) latitude = 89.0f;
    if (latitude < -89.0f) latitude = -89.0f;
    if (longitude > 180.0f) longitude = 180.0f;
    if (longitude < -180.0f) longitude = -180.0f;
    if (utcOffsetMinutes < -14 * 60) utcOffsetMinutes = -14 * 60;
    if (utcOffsetMinutes > 14 * 60) utcOffsetMinutes = 14 * 60;

    if (dayMinutes < 1) dayMinutes = 1;
    if (dayMinutes > 24 * 60) dayMinutes = 24 * 60;
    if (manualTime >= 1440) manualTime = 1439;
    if (dayOfYear < 1) dayOfYear = 1;
    if (dayOfYear > 366) dayOfYear = 366;

    if (groupCount > SCENE_MAX_GROUPS) groupCount = SCENE_MAX_GROUPS;

    for (uint8_t g = 0; g < groupCount; g++) {
        GroupConfig &G = groups[g];
        G.name[SCENE_NAME_LEN - 1] = 0;
        if (G.litPercent > 100) G.litPercent = 100;
        if (G.flickerPercent > 100) G.flickerPercent = 100;
        if (G.onTo < G.onFrom) G.onTo = G.onFrom;
        if (G.offTo < G.offFrom) G.offTo = G.offFrom;
        if (G.fadeMs > 60000) G.fadeMs = 60000;
    }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

// Emit the lamp bitmap as ints and "a-b" ranges, which is what a person
// would write and what the GUI can show.
static void lampsToJson(const GroupConfig &G, JsonArray arr) {
    int start = -1;
    for (int i = 0; i <= LAMPS_MAX_LAMPS; i++) {
        bool on = (i < LAMPS_MAX_LAMPS) && G.has((uint16_t)i);
        if (on && start < 0) {
            start = i;
        } else if (!on && start >= 0) {
            int end = i - 1;
            if (end == start) {
                arr.add(start);
            } else if (end == start + 1) {
                arr.add(start);
                arr.add(end);
            } else {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d-%d", start, end);
                arr.add(buf);
            }
            start = -1;
        }
    }
}

static bool lampsFromJson(JsonArrayConst arr, GroupConfig &G, String *error) {
    G.clearLamps();
    for (JsonVariantConst v : arr) {
        if (v.is<int>()) {
            int n = v;
            if (n < 0 || n >= LAMPS_MAX_LAMPS) {
                if (error) *error = "lamp index out of range";
                return false;
            }
            G.add((uint16_t)n);
        } else if (v.is<const char *>()) {
            const char *s = v;
            const char *dash = strchr(s, '-');
            long a = strtol(s, nullptr, 10);
            long b = dash ? strtol(dash + 1, nullptr, 10) : a;
            if (a < 0 || b < a || b >= LAMPS_MAX_LAMPS) {
                if (error) *error = "lamp range out of range";
                return false;
            }
            for (long i = a; i <= b; i++) {
                G.add((uint16_t)i);
            }
        }
    }
    return true;
}

void SceneConfig::toJson(String &out) const {
    JsonDocument doc;

    JsonObject loc = doc["location"].to<JsonObject>();
    loc["lat"] = latitude;
    loc["lon"] = longitude;
    loc["autoTimezone"] = autoTimezone;
    loc["utcOffsetMinutes"] = utcOffsetMinutes;

    char tbuf[8];
    JsonObject clk = doc["clock"].to<JsonObject>();
    clk["mode"] = modeName(mode);
    clk["dayMinutes"] = dayMinutes;
    formatTime(manualTime, tbuf, sizeof(tbuf));
    clk["manualTime"] = tbuf;
    clk["dateFromSystem"] = dateFromSystem;
    clk["dayOfYear"] = dayOfYear;

    doc["seed"] = seed;

    JsonArray gs = doc["groups"].to<JsonArray>();
    for (uint8_t g = 0; g < groupCount; g++) {
        const GroupConfig &G = groups[g];
        JsonObject o = gs.add<JsonObject>();
        o["name"] = G.name;
        o["behaviour"] = behaviourName(G.behaviour);
        lampsToJson(G, o["lamps"].to<JsonArray>());
        o["onAnchor"] = anchorName(G.onAnchor);
        JsonArray onW = o["on"].to<JsonArray>();
        onW.add(G.onFrom);
        onW.add(G.onTo);
        o["offAnchor"] = anchorName(G.offAnchor);
        JsonArray offW = o["off"].to<JsonArray>();
        offW.add(G.offFrom);
        offW.add(G.offTo);
        o["litPercent"] = G.litPercent;
        o["flickerPercent"] = G.flickerPercent;
        o["morning"] = G.morning;
        o["level"] = G.level;
        o["fadeMs"] = G.fadeMs;
    }

    out = "";
    serializeJson(doc, out);
}

bool SceneConfig::fromJson(const String &in, String *error) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, in);
    if (err) {
        if (error) *error = String("invalid JSON: ") + err.c_str();
        return false;
    }

    SceneConfig next = *this;

    JsonObjectConst loc = doc["location"];
    if (!loc.isNull()) {
        if (loc["lat"].is<float>()) next.latitude = loc["lat"];
        if (loc["lon"].is<float>()) next.longitude = loc["lon"];
        if (loc["autoTimezone"].is<bool>()) next.autoTimezone = loc["autoTimezone"];
        if (loc["utcOffsetMinutes"].is<int>()) next.utcOffsetMinutes = loc["utcOffsetMinutes"];
    }

    JsonObjectConst clk = doc["clock"];
    if (!clk.isNull()) {
        if (clk["mode"].is<const char *>() && !parseMode(clk["mode"], next.mode)) {
            if (error) *error = "clock.mode must be real, accelerated or manual";
            return false;
        }
        if (clk["dayMinutes"].is<int>()) next.dayMinutes = clk["dayMinutes"];
        if (clk["manualTime"].is<const char *>()) {
            if (!parseTime(clk["manualTime"], next.manualTime)) {
                if (error) *error = "clock.manualTime must be HH:MM";
                return false;
            }
        } else if (clk["manualTime"].is<int>()) {
            next.manualTime = clk["manualTime"];
        }
        if (clk["dateFromSystem"].is<bool>()) next.dateFromSystem = clk["dateFromSystem"];
        if (clk["dayOfYear"].is<int>()) next.dayOfYear = clk["dayOfYear"];
    }

    if (doc["seed"].is<uint32_t>()) next.seed = doc["seed"];

    JsonArrayConst gs = doc["groups"];
    if (!gs.isNull()) {
        next.groupCount = 0;
        for (JsonObjectConst o : gs) {
            if (next.groupCount >= SCENE_MAX_GROUPS) {
                if (error) *error = "too many groups";
                return false;
            }
            GroupConfig &G = next.groups[next.groupCount];

            Behaviour b = Behaviour::Off;
            if (!parseBehaviour(o["behaviour"] | "off", b)) {
                if (error) *error = "unknown behaviour";
                return false;
            }
            G.setPreset(b);

            strlcpy(G.name, o["name"] | "", SCENE_NAME_LEN);

            if (!lampsFromJson(o["lamps"], G, error)) {
                return false;
            }

            if (o["onAnchor"].is<const char *>()) parseAnchor(o["onAnchor"], G.onAnchor);
            if (o["offAnchor"].is<const char *>()) parseAnchor(o["offAnchor"], G.offAnchor);
            JsonArrayConst onW = o["on"];
            if (onW.size() == 2) {
                G.onFrom = onW[0];
                G.onTo = onW[1];
            }
            JsonArrayConst offW = o["off"];
            if (offW.size() == 2) {
                G.offFrom = offW[0];
                G.offTo = offW[1];
            }
            if (o["litPercent"].is<int>()) G.litPercent = o["litPercent"];
            if (o["flickerPercent"].is<int>()) G.flickerPercent = o["flickerPercent"];
            if (o["morning"].is<bool>()) G.morning = o["morning"];
            if (o["level"].is<int>()) G.level = o["level"];
            if (o["fadeMs"].is<int>()) G.fadeMs = o["fadeMs"];

            next.groupCount++;
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

bool SceneStore::load(SceneConfig &cfg) {
    if (!fsReady()) return false;
    File f = LittleFS.open(path(), "r");
    if (!f) return false;
    String body = f.readString();
    f.close();

    SceneConfig parsed;
    if (!parsed.fromJson(body)) return false;
    cfg = parsed;
    return true;
}

bool SceneStore::save(const SceneConfig &cfg) {
    if (!fsReady()) return false;
    String body;
    cfg.toJson(body);
    File f = LittleFS.open(path(), "w");
    if (!f) return false;
    size_t n = f.print(body);
    f.close();
    return n == body.length();
}

bool SceneStore::erase() {
    if (!fsReady()) return false;
    return LittleFS.remove(path());
}
