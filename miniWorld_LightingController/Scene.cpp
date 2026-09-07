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
    dayActivity = 0;
    nightActivity = 0;

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
            dayActivity = 2;
            nightActivity = 1;
            break;

        case Behaviour::Shop:
            onAnchor = Anchor::Clock;  onFrom = 510;  onTo = 540;      // 08:30 to 09:00
            offAnchor = Anchor::Clock; offFrom = 1080; offTo = 1110;   // 18:00 to 18:30
            level = 230;
            fadeMs = 200;
            dayActivity = 1;
            break;

        case Behaviour::Late:
            onAnchor = Anchor::Dusk;   onFrom = -30;  onTo = 30;
            offAnchor = Anchor::Clock; offFrom = 1380; offTo = 1560;   // 23:00 to 02:00
            level = 220;
            dayActivity = 1;
            break;

        case Behaviour::AllNight:
            onAnchor = Anchor::Dusk;   onFrom = -15;  onTo = 0;
            offAnchor = Anchor::Dawn;  offFrom = 0;   offTo = 15;
            break;

        // The four households. They fill the same fields as the buildings
        // above, so everything stays tunable per group.

        case Behaviour::Family:
            onAnchor = Anchor::Dusk;   onFrom = 0;    onTo = 180;
            offAnchor = Anchor::Clock; offFrom = 1350; offTo = 1440;   // 22:30 to 00:00
            litPercent = 90;
            flickerPercent = 25;
            morning = true;
            level = 210;
            fadeMs = 300;
            dayActivity = 3;
            nightActivity = 1;
            break;

        case Behaviour::Elderly:
            onAnchor = Anchor::Dusk;   onFrom = -30;  onTo = 60;
            offAnchor = Anchor::Clock; offFrom = 1260; offTo = 1335;   // 21:00 to 22:15
            litPercent = 90;
            flickerPercent = 8;
            morning = true;
            level = 180;
            fadeMs = 400;
            dayActivity = 2;
            nightActivity = 3;
            break;

        case Behaviour::NightOwl:
            onAnchor = Anchor::Dusk;   onFrom = 60;   onTo = 240;
            offAnchor = Anchor::Clock; offFrom = 1470; offTo = 1590;   // 00:30 to 02:30
            litPercent = 80;
            flickerPercent = 30;
            level = 200;
            fadeMs = 300;
            dayActivity = 1;
            nightActivity = 1;
            break;

        case Behaviour::Away:
            // A timer lamp: one flat in four, the same minutes every evening
            // and no life at all, which is exactly how it should read.
            onAnchor = Anchor::Clock;  onFrom = 1140; onTo = 1150;    // 19:00 to 19:10
            offAnchor = Anchor::Clock; offFrom = 1350; offTo = 1360;   // 22:30 to 22:40
            litPercent = 25;
            level = 200;
            fadeMs = 0;
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
// Flat presets. The rhythm of a household on a weekday, in minutes since
// midnight. Everything here can be overridden per flat in JSON; the weekend
// variations and the daily draw live in the engine.
// ---------------------------------------------------------------------------

void FlatConfig::setPreset(Household t) {
    type = t;
    weekend = true;
    roomCount = 0;
    name[0] = 0;
    building[0] = 0;
    outPercent = 14;
    tvPercent = 70;
    level = 210;
    fadeMs = 300;
    dayActivity = 3;
    nightActivity = 1;

    switch (t) {
        case Household::Elderly:
            wakeFrom = 345;  wakeTo = 390;      // 05:45 to 06:30
            leaveFrom = 570; leaveTo = 630;     // 09:30 to 10:30
            homeFrom = 720;  homeTo = 810;      // 12:00 to 13:30
            bedFrom = 1275;  bedTo = 1335;      // 21:15 to 22:15
            outPercent = 7;
            tvPercent = 40;
            level = 180;
            fadeMs = 400;
            dayActivity = 2;
            nightActivity = 3;
            break;

        case Household::NightOwl:
            wakeFrom = 570;  wakeTo = 660;      // 09:30 to 11:00
            leaveFrom = 690; leaveTo = 750;     // 11:30 to 12:30
            homeFrom = 1140; homeTo = 1260;     // 19:00 to 21:00
            bedFrom = 1485;  bedTo = 1575;      // 00:45 to 02:15
            outPercent = 28;
            tvPercent = 80;
            level = 200;
            fadeMs = 300;
            dayActivity = 1;
            nightActivity = 1;
            break;

        case Household::Away:
            // Nobody lives here this week. No rhythm at all: the engine
            // gives a living or other room the timer evening and leaves
            // every other room dark, with no life on top.
            wakeFrom = wakeTo = -1;
            leaveFrom = leaveTo = -1;
            homeFrom = homeTo = -1;
            bedFrom = bedTo = -1;
            outPercent = 0;
            tvPercent = 0;
            level = 200;
            fadeMs = 0;
            dayActivity = 0;
            nightActivity = 0;
            break;

        // Family, and Custom, which starts from the family rhythm and is
        // then left alone.
        default:
            wakeFrom = 390;  wakeTo = 435;      // 06:30 to 07:15
            leaveFrom = 450; leaveTo = 495;     // 07:30 to 08:15
            homeFrom = 960;  homeTo = 1050;     // 16:00 to 17:30
            bedFrom = 1350;  bedTo = 1410;      // 22:30 to 23:30
            break;
    }
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

static const char *const behaviourNames[] = {
    "off", "street", "home", "shop", "late", "allnight",
    "family", "elderly", "nightowl", "away"
};
static const char *const anchorNames[] = { "dusk", "dawn", "clock" };
static const char *const modeNames[] = { "real", "accelerated", "manual" };
static const char *const householdNames[] = {
    "family", "elderly", "nightowl", "away", "custom"
};
static const char *const roomNames[] = {
    "living", "kitchen", "bedroom", "bathroom", "hall", "other"
};

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

const char *SceneConfig::householdName(Household t) {
    uint8_t i = (uint8_t)t;
    return i < (uint8_t)Household::COUNT ? householdNames[i] : "family";
}

bool SceneConfig::parseHousehold(const char *s, Household &out) {
    if (!s) return false;
    for (uint8_t i = 0; i < (uint8_t)Household::COUNT; i++) {
        if (!strcasecmp(s, householdNames[i])) {
            out = (Household)i;
            return true;
        }
    }
    return false;
}

const char *SceneConfig::roomName(Room r) {
    uint8_t i = (uint8_t)r;
    return i < (uint8_t)Room::COUNT ? roomNames[i] : "other";
}

bool SceneConfig::parseRoom(const char *s, Room &out) {
    if (!s) return false;
    for (uint8_t i = 0; i < (uint8_t)Room::COUNT; i++) {
        if (!strcasecmp(s, roomNames[i])) {
            out = (Room)i;
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
        if (G.dayActivity > 3) G.dayActivity = 3;
        if (G.nightActivity > 3) G.nightActivity = 3;
    }

    if (flatCount > SCENE_MAX_FLATS) flatCount = SCENE_MAX_FLATS;

    for (uint8_t f = 0; f < flatCount; f++) {
        FlatConfig &F = flats[f];
        F.name[SCENE_NAME_LEN - 1] = 0;
        F.building[SCENE_NAME_LEN - 1] = 0;
        if (!F.name[0]) {
            // A flat always has something to call it in the list.
            snprintf(F.name, SCENE_NAME_LEN, "Flat %u", (unsigned)(f + 1));
        }
        if ((uint8_t)F.type >= (uint8_t)Household::COUNT) F.type = Household::Family;
        if (F.roomCount > FLAT_MAX_ROOMS) F.roomCount = FLAT_MAX_ROOMS;
        for (uint8_t r = 0; r < F.roomCount; ) {
            if (F.rooms[r].lamp >= LAMPS_MAX_LAMPS) F.rooms[r].lamp = LAMPS_MAX_LAMPS - 1;
            if ((uint8_t)F.rooms[r].role >= (uint8_t)Room::COUNT) F.rooms[r].role = Room::Other;
            // One lamp, one role. A lamp listed twice in the same flat keeps
            // its first row, so the engine never ORs two rooms onto it.
            bool dup = false;
            for (uint8_t q = 0; q < r; q++) {
                if (F.rooms[q].lamp == F.rooms[r].lamp) { dup = true; break; }
            }
            if (dup) {
                for (uint8_t q = r; q + 1 < F.roomCount; q++) F.rooms[q] = F.rooms[q + 1];
                F.roomCount--;
            } else {
                r++;
            }
        }
        if (F.outPercent > 100) F.outPercent = 100;
        if (F.tvPercent > 100) F.tvPercent = 100;
        if (F.fadeMs > 60000) F.fadeMs = 60000;
        if (F.dayActivity > 3) F.dayActivity = 3;
        if (F.nightActivity > 3) F.nightActivity = 3;

        // Minus one is the only meaningful negative: "no such moment".
        // Everything else lives on the same 0..2879 line as a group window.
        int16_t *range[8] = { &F.wakeFrom, &F.wakeTo, &F.leaveFrom, &F.leaveTo,
                              &F.homeFrom, &F.homeTo, &F.bedFrom, &F.bedTo };
        for (uint8_t i = 0; i < 8; i++) {
            if (*range[i] < -1) *range[i] = -1;
            if (*range[i] > 2879) *range[i] = 2879;
        }
        for (uint8_t i = 0; i < 8; i += 2) {
            if (*range[i + 1] < *range[i]) *range[i + 1] = *range[i];
        }
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
        o["dayActivity"] = G.dayActivity;
        o["nightActivity"] = G.nightActivity;
    }

    JsonArray fs = doc["flats"].to<JsonArray>();
    for (uint8_t f = 0; f < flatCount; f++) {
        const FlatConfig &F = flats[f];
        JsonObject o = fs.add<JsonObject>();
        o["name"] = F.name;
        o["building"] = F.building;
        o["type"] = householdName(F.type);
        o["weekend"] = F.weekend;
        JsonArray rs = o["rooms"].to<JsonArray>();
        for (uint8_t r = 0; r < F.roomCount; r++) {
            JsonObject ro = rs.add<JsonObject>();
            ro["lamp"] = F.rooms[r].lamp;
            ro["role"] = roomName(F.rooms[r].role);
        }
        JsonArray wk = o["wake"].to<JsonArray>();
        wk.add(F.wakeFrom);
        wk.add(F.wakeTo);
        JsonArray lv = o["leave"].to<JsonArray>();
        lv.add(F.leaveFrom);
        lv.add(F.leaveTo);
        JsonArray hm = o["home"].to<JsonArray>();
        hm.add(F.homeFrom);
        hm.add(F.homeTo);
        JsonArray bd = o["bed"].to<JsonArray>();
        bd.add(F.bedFrom);
        bd.add(F.bedTo);
        o["outPercent"] = F.outPercent;
        o["tvPercent"] = F.tvPercent;
        o["level"] = F.level;
        o["fadeMs"] = F.fadeMs;
        o["dayActivity"] = F.dayActivity;
        o["nightActivity"] = F.nightActivity;
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

    // Static: a SceneConfig is about 4.8 kB and the core-0 stack is far
    // smaller. The sketch is single-threaded and fromJson() never nests with
    // itself, so one working copy can be shared.
    static SceneConfig next;
    next = *this;

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

            // Absent means "whatever this behaviour's preset says", which
            // setPreset() has already put in place above.
            if (o["dayActivity"].is<int>()) G.dayActivity = o["dayActivity"];
            if (o["nightActivity"].is<int>()) G.nightActivity = o["nightActivity"];

            next.groupCount++;
        }
    }

    // Absent means no flats at all, which is what every scene written before
    // households existed looks like.
    JsonArrayConst fs = doc["flats"];
    if (!fs.isNull()) {
        next.flatCount = 0;
        for (JsonObjectConst o : fs) {
            if (next.flatCount >= SCENE_MAX_FLATS) {
                if (error) *error = "too many flats";
                return false;
            }
            FlatConfig &F = next.flats[next.flatCount];

            Household t = Household::Family;
            if (!parseHousehold(o["type"] | "family", t)) {
                if (error) *error = "unknown household type";
                return false;
            }
            F.setPreset(t);

            strlcpy(F.name, o["name"] | "", SCENE_NAME_LEN);
            strlcpy(F.building, o["building"] | "", SCENE_NAME_LEN);
            if (o["weekend"].is<bool>()) F.weekend = o["weekend"];

            JsonArrayConst rs = o["rooms"];
            if (!rs.isNull()) {
                F.roomCount = 0;
                for (JsonObjectConst ro : rs) {
                    if (F.roomCount >= FLAT_MAX_ROOMS) {
                        if (error) *error = "too many rooms in a flat";
                        return false;
                    }
                    int lamp = ro["lamp"] | -1;
                    if (lamp < 0 || lamp >= LAMPS_MAX_LAMPS) {
                        if (error) *error = "room lamp index out of range";
                        return false;
                    }
                    Room role = Room::Other;
                    if (!parseRoom(ro["role"] | "other", role)) {
                        if (error) *error = "unknown room role";
                        return false;
                    }
                    F.rooms[F.roomCount].lamp = (uint16_t)lamp;
                    F.rooms[F.roomCount].role = role;
                    F.roomCount++;
                }
            }

            // The rhythm. Absent means "whatever this type's preset says",
            // which setPreset() has already put in place above.
            struct { const char *key; int16_t *from; int16_t *to; } ranges[4] = {
                { "wake",  &F.wakeFrom,  &F.wakeTo  },
                { "leave", &F.leaveFrom, &F.leaveTo },
                { "home",  &F.homeFrom,  &F.homeTo  },
                { "bed",   &F.bedFrom,   &F.bedTo   },
            };
            for (uint8_t i = 0; i < 4; i++) {
                JsonArrayConst w = o[ranges[i].key];
                if (w.size() == 2) {
                    *ranges[i].from = w[0];
                    *ranges[i].to = w[1];
                }
            }

            if (o["outPercent"].is<int>()) F.outPercent = o["outPercent"];
            if (o["tvPercent"].is<int>()) F.tvPercent = o["tvPercent"];
            if (o["level"].is<int>()) F.level = o["level"];
            if (o["fadeMs"].is<int>()) F.fadeMs = o["fadeMs"];
            if (o["dayActivity"].is<int>()) F.dayActivity = o["dayActivity"];
            if (o["nightActivity"].is<int>()) F.nightActivity = o["nightActivity"];

            next.flatCount++;
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

    // Parsed straight into cfg, with no intermediate copy: a SceneConfig is
    // about 4.8 kB, too much for the stack, and fromJson() only commits on
    // success, so cfg is untouched when the file does not parse.
    return cfg.fromJson(body);
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
