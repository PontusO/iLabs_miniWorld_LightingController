/*
    Scene - see Scene.h

    Invector Embedded Systems AB
*/

#include "Scene.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>

static_assert(SCENE_MAX_MODELS >= (int)Template::COUNT,
              "the template models have to fit in the model table");

// ---------------------------------------------------------------------------
// Templates. These are the "habits" of each kind of building: four
// households and five sets of opening hours. Everything here can be
// overridden per model in JSON, but the defaults are meant to be plausible
// straight out of the box.
// ---------------------------------------------------------------------------

// The rhythm half, plus the brightness and the life that go with it. Any
// template that is not one of the four households takes the family rhythm,
// which is what an hours model carries so every field is defined whatever
// the kind.
static void fillRhythm(ModelConfig &M, Template t) {
    M.weekend = true;
    M.outPercent = 14;
    M.tvPercent = 70;
    M.level = 210;
    M.fadeMs = 300;
    M.dayActivity = 3;
    M.nightActivity = 1;

    switch (t) {
        case Template::Elderly:
            M.wakeFrom = 345;  M.wakeTo = 390;      // 05:45 to 06:30
            M.leaveFrom = 570; M.leaveTo = 630;     // 09:30 to 10:30
            M.homeFrom = 720;  M.homeTo = 810;      // 12:00 to 13:30
            M.bedFrom = 1275;  M.bedTo = 1335;      // 21:15 to 22:15
            M.outPercent = 7;
            M.tvPercent = 40;
            M.level = 180;
            M.fadeMs = 400;
            M.dayActivity = 2;
            M.nightActivity = 3;
            break;

        case Template::NightOwl:
            M.wakeFrom = 570;  M.wakeTo = 660;      // 09:30 to 11:00
            M.leaveFrom = 690; M.leaveTo = 750;     // 11:30 to 12:30
            M.homeFrom = 1140; M.homeTo = 1260;     // 19:00 to 21:00
            M.bedFrom = 1485;  M.bedTo = 1575;      // 00:45 to 02:15
            M.outPercent = 28;
            M.tvPercent = 80;
            M.level = 200;
            M.fadeMs = 300;
            M.dayActivity = 1;
            M.nightActivity = 1;
            break;

        case Template::Away:
            // Nobody lives here this week. No rhythm at all: the engine
            // gives a living or other room the timer evening and leaves
            // every other room dark, with no life on top.
            M.wakeFrom = M.wakeTo = -1;
            M.leaveFrom = M.leaveTo = -1;
            M.homeFrom = M.homeTo = -1;
            M.bedFrom = M.bedTo = -1;
            M.outPercent = 0;
            M.tvPercent = 0;
            M.level = 200;
            M.fadeMs = 0;
            M.dayActivity = 0;
            M.nightActivity = 0;
            break;

        // Family, and every hours template, which carries the family
        // rhythm behind its opening hours.
        default:
            M.wakeFrom = 390;  M.wakeTo = 435;      // 06:30 to 07:15
            M.leaveFrom = 450; M.leaveTo = 495;     // 07:30 to 08:15
            M.homeFrom = 960;  M.homeTo = 1050;     // 16:00 to 17:30
            M.bedFrom = 1350;  M.bedTo = 1410;      // 22:30 to 23:30
            break;
    }
}

// The hours half, plus the brightness and the life that go with it. Any
// template that is not one of the five hours templates takes the Home
// values, which is what a rhythm model carries.
static void fillHours(ModelConfig &M, Template t) {
    M.litPercent = 100;
    M.flickerPercent = 0;
    M.morning = false;
    M.individual = false;
    M.level = 255;
    M.fadeMs = 800;
    M.dayActivity = 0;
    M.nightActivity = 0;

    switch (t) {
        case Template::Shop:
            M.onAnchor = Anchor::Clock;  M.onFrom = 510;  M.onTo = 540;      // 08:30 to 09:00
            M.offAnchor = Anchor::Clock; M.offFrom = 1080; M.offTo = 1110;   // 18:00 to 18:30
            M.level = 230;
            M.fadeMs = 200;
            M.dayActivity = 1;
            break;

        case Template::Pub:
            M.onAnchor = Anchor::Dusk;   M.onFrom = -30;  M.onTo = 30;
            M.offAnchor = Anchor::Clock; M.offFrom = 1380; M.offTo = 1560;   // 23:00 to 02:00
            M.level = 220;
            M.dayActivity = 1;
            break;

        case Template::Street:
            M.onAnchor = Anchor::Dusk;   M.onFrom = -10;  M.onTo = 5;
            M.offAnchor = Anchor::Dawn;  M.offFrom = -5;  M.offTo = 15;
            M.fadeMs = 4000;            // sodium warm-up
            break;

        case Template::AllNight:
            M.onAnchor = Anchor::Dusk;   M.onFrom = -15;  M.onTo = 0;
            M.offAnchor = Anchor::Dawn;  M.offFrom = 0;   M.offTo = 15;
            break;

        // Home, and every rhythm template, which carries the Home hours
        // behind its household day.
        default:
            M.onAnchor = Anchor::Dusk;   M.onFrom = 0;    M.onTo = 240;
            M.offAnchor = Anchor::Clock; M.offFrom = 1320; M.offTo = 1470;   // 22:00 to 00:30
            M.litPercent = 85;
            M.flickerPercent = 12;
            M.morning = true;
            M.individual = true;        // a block of windows, not one switch
            M.level = 200;
            M.fadeMs = 300;
            M.dayActivity = 2;
            M.nightActivity = 1;
            break;
    }
}

void ModelConfig::setTemplate(Template t) {
    name[0] = 0;                // the caller names the model
    bool rhythm = (t == Template::Family || t == Template::Elderly ||
                   t == Template::NightOwl || t == Template::Away);
    kind = rhythm ? ModelKind::Rhythm : ModelKind::Hours;

    // Both halves, the model's own kind last so the brightness, the fade
    // and the life that survive are its own.
    if (rhythm) {
        fillHours(*this, Template::Home);
        fillRhythm(*this, t);
    } else {
        fillRhythm(*this, Template::Family);
        fillHours(*this, t);
    }
}

bool ModelConfig::isAway() const {
    return kind == ModelKind::Rhythm && wakeFrom < 0 && bedFrom < 0;
}

void SceneConfig::seedTemplates() {
    modelCount = (uint8_t)Template::COUNT;
    for (uint8_t i = 0; i < modelCount; i++) {
        models[i].setTemplate((Template)i);
        strlcpy(models[i].name, templateTitle((Template)i), SCENE_NAME_LEN);
    }
    unitCount = 0;
}

int SceneConfig::findModel(const char *name) const {
    if (!name || !*name) {
        return -1;
    }
    for (uint8_t i = 0; i < modelCount && i < SCENE_MAX_MODELS; i++) {
        if (!strcasecmp(models[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// A room's lamps. Ranges rather than single lamps: a room is a handful of
// runs of addresses, so a street of sixty lamps is one range and not sixty
// entries.
// ---------------------------------------------------------------------------

bool RoomConfig::has(uint16_t lamp) const {
    for (uint8_t i = 0; i < rangeCount && i < ROOM_MAX_RANGES; i++) {
        if (lamp >= ranges[i].from && lamp <= ranges[i].to) {
            return true;
        }
    }
    return false;
}

bool RoomConfig::add(uint16_t from, uint16_t to) {
    if (from > to || to >= LAMPS_MAX_LAMPS || rangeCount >= ROOM_MAX_RANGES) {
        return false;
    }
    ranges[rangeCount].from = from;
    ranges[rangeCount].to = to;
    rangeCount++;
    return true;
}

uint16_t RoomConfig::count() const {
    uint16_t n = 0;
    for (uint8_t i = 0; i < rangeCount && i < ROOM_MAX_RANGES; i++) {
        n = (uint16_t)(n + (ranges[i].to - ranges[i].from + 1));
    }
    return n;
}

uint16_t RoomConfig::lamp(uint16_t i) const {
    for (uint8_t r = 0; r < rangeCount && r < ROOM_MAX_RANGES; r++) {
        uint16_t n = (uint16_t)(ranges[r].to - ranges[r].from + 1);
        if (i < n) {
            return (uint16_t)(ranges[r].from + i);
        }
        i = (uint16_t)(i - n);
    }
    return 0xFFFF;
}

uint16_t RoomConfig::first() const {
    return rangeCount ? ranges[0].from : (uint16_t)0xFFFF;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

static const char *const kindNames[] = { "rhythm", "hours" };
static const char *const templateKeys[] = {
    "family", "elderly", "nightowl", "away",
    "home", "shop", "pub", "street", "allnight"
};
static const char *const templateTitles[] = {
    "Family", "Elderly couple", "Night owl", "Away",
    "Home", "Shop", "Pub", "Street light", "All night"
};
static const char *const anchorNames[] = { "dusk", "dawn", "clock" };
static const char *const modeNames[] = { "real", "accelerated", "manual" };
static const char *const roomNames[] = {
    "living", "kitchen", "bedroom", "bathroom", "hall",
    "front", "back", "sign", "other"
};

const char *SceneConfig::kindName(ModelKind k) {
    uint8_t i = (uint8_t)k;
    return i < (uint8_t)ModelKind::COUNT ? kindNames[i] : "hours";
}

bool SceneConfig::parseKind(const char *s, ModelKind &out) {
    if (!s) return false;
    for (uint8_t i = 0; i < (uint8_t)ModelKind::COUNT; i++) {
        if (!strcasecmp(s, kindNames[i])) {
            out = (ModelKind)i;
            return true;
        }
    }
    return false;
}

const char *SceneConfig::templateKey(Template t) {
    uint8_t i = (uint8_t)t;
    return i < (uint8_t)Template::COUNT ? templateKeys[i] : "home";
}

const char *SceneConfig::templateTitle(Template t) {
    uint8_t i = (uint8_t)t;
    return i < (uint8_t)Template::COUNT ? templateTitles[i] : "Home";
}

bool SceneConfig::parseTemplate(const char *s, Template &out) {
    if (!s) return false;
    for (uint8_t i = 0; i < (uint8_t)Template::COUNT; i++) {
        if (!strcasecmp(s, templateKeys[i])) {
            out = (Template)i;
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
// Clamp
// ---------------------------------------------------------------------------

// A name no other model carries, compared case-insensitively. An empty name
// starts from the Home title, and a name already taken gets " 2", " 3" and
// so on, with the base trimmed so the suffix fits in SCENE_NAME_LEN. self
// is the index the name belongs to, so a model is never compared against
// itself; a name being appended passes the count, which no index reaches.
static void uniqueName(const ModelConfig *models, uint8_t count, uint8_t self,
                       char *name) {
    if (!name[0]) {
        strlcpy(name, SceneConfig::templateTitle(Template::Home), SCENE_NAME_LEN);
    }
    char base[SCENE_NAME_LEN];
    strlcpy(base, name, SCENE_NAME_LEN);

    for (unsigned n = 2; n < 100; n++) {
        bool taken = false;
        for (uint8_t i = 0; i < count && i < SCENE_MAX_MODELS && !taken; i++) {
            if (i != self && !strcasecmp(models[i].name, name)) {
                taken = true;
            }
        }
        if (!taken) {
            return;
        }
        char suffix[8];
        snprintf(suffix, sizeof(suffix), " %u", n);
        size_t keep = SCENE_NAME_LEN - 1 - strlen(suffix);
        if (keep > strlen(base)) {
            keep = strlen(base);
        }
        snprintf(name, SCENE_NAME_LEN, "%.*s%s", (int)keep, base, suffix);
    }
}

// Cut [c, d] out of a list of pieces that are in order and do not overlap.
// A cut in the middle splits a piece in two, which is why the list can
// grow; anything past max is dropped, which is the rule for a range that
// shatters into more pieces than a room can hold.
static uint8_t subtractRange(LampRange *piece, uint8_t n, uint8_t max,
                             uint16_t c, uint16_t d) {
    LampRange out[ROOM_MAX_RANGES];
    uint8_t k = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint16_t a = piece[i].from, b = piece[i].to;
        if (d < a || c > b) {
            if (k < max) out[k++] = piece[i];
            continue;
        }
        if (c > a && k < max) {
            out[k].from = a;
            out[k].to = (uint16_t)(c - 1);
            k++;
        }
        if (d < b && k < max) {
            out[k].from = (uint16_t)(d + 1);
            out[k].to = b;
            k++;
        }
    }
    for (uint8_t i = 0; i < k; i++) {
        piece[i] = out[i];
    }
    return k;
}

// One lamp, one room. A lamp listed twice anywhere in the unit keeps its
// first appearance, so the engine never ORs two rooms onto it, and a room
// left with nothing in it is not a room. Clamping a range that runs past
// the fitted lamps can itself make a duplicate, which the cuts below then
// remove.
static void dedupUnitRooms(UnitConfig &U) {
    for (uint8_t r = 0; r < U.roomCount; ) {
        RoomConfig &R = U.rooms[r];
        if (R.rangeCount > ROOM_MAX_RANGES) R.rangeCount = ROOM_MAX_RANGES;
        if ((uint8_t)R.role >= (uint8_t)Room::COUNT) R.role = Room::Other;

        LampRange keep[ROOM_MAX_RANGES];
        uint8_t n = 0;
        for (uint8_t i = 0; i < R.rangeCount; i++) {
            uint16_t from = R.ranges[i].from;
            uint16_t to = R.ranges[i].to;
            if (from > to || from >= LAMPS_MAX_LAMPS) {
                continue;               // no lamp could be in it
            }
            if (to >= LAMPS_MAX_LAMPS) to = (uint16_t)(LAMPS_MAX_LAMPS - 1);

            LampRange piece[ROOM_MAX_RANGES];
            piece[0].from = from;
            piece[0].to = to;
            uint8_t np = 1;

            // Everything already spoken for: the earlier rooms of this
            // unit, which have been through this already, and then the
            // ranges of this room that survived.
            for (uint8_t q = 0; q < r && np; q++) {
                const RoomConfig &E = U.rooms[q];
                for (uint8_t j = 0; j < E.rangeCount && np; j++) {
                    np = subtractRange(piece, np, ROOM_MAX_RANGES,
                                       E.ranges[j].from, E.ranges[j].to);
                }
            }
            for (uint8_t q = 0; q < n && np; q++) {
                np = subtractRange(piece, np, ROOM_MAX_RANGES,
                                   keep[q].from, keep[q].to);
            }
            for (uint8_t k = 0; k < np && n < ROOM_MAX_RANGES; k++) {
                keep[n++] = piece[k];
            }
        }

        for (uint8_t i = 0; i < n; i++) {
            R.ranges[i] = keep[i];
        }
        R.rangeCount = n;

        if (n == 0) {
            for (uint8_t q = r; q + 1 < U.roomCount; q++) U.rooms[q] = U.rooms[q + 1];
            U.roomCount--;
        } else {
            r++;
        }
    }
}

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

    if (modelCount > SCENE_MAX_MODELS) modelCount = SCENE_MAX_MODELS;

    for (uint8_t m = 0; m < modelCount; m++) {
        ModelConfig &M = models[m];
        M.name[SCENE_NAME_LEN - 1] = 0;
        // A model without a name is never shown or referred to, so it gets
        // one. A model that has a name keeps it: a duplicate is the
        // loader's business and not the clamp's.
        if (!M.name[0]) {
            uniqueName(models, modelCount, m, M.name);
        }
        if ((uint8_t)M.kind >= (uint8_t)ModelKind::COUNT) M.kind = ModelKind::Hours;

        if (M.fadeMs > 60000) M.fadeMs = 60000;
        if (M.dayActivity > 3) M.dayActivity = 3;
        if (M.nightActivity > 3) M.nightActivity = 3;
        if (M.outPercent > 100) M.outPercent = 100;
        if (M.tvPercent > 100) M.tvPercent = 100;
        if (M.litPercent > 100) M.litPercent = 100;
        if (M.flickerPercent > 100) M.flickerPercent = 100;

        // The rhythm. Minus one is the only meaningful negative, "no such
        // moment"; everything else lives on the same 0..2879 line as an
        // hours window.
        int16_t *rhythm[8] = { &M.wakeFrom, &M.wakeTo, &M.leaveFrom, &M.leaveTo,
                               &M.homeFrom, &M.homeTo, &M.bedFrom, &M.bedTo };
        for (uint8_t i = 0; i < 8; i++) {
            if (*rhythm[i] < -1) *rhythm[i] = -1;
            if (*rhythm[i] > 2879) *rhythm[i] = 2879;
        }
        for (uint8_t i = 0; i < 8; i += 2) {
            if (*rhythm[i + 1] < *rhythm[i]) *rhythm[i + 1] = *rhythm[i];
        }

        // The hours. A window anchored to dusk or dawn may start half a day
        // before its anchor, so the line runs from -720.
        int16_t *hours[4] = { &M.onFrom, &M.onTo, &M.offFrom, &M.offTo };
        for (uint8_t i = 0; i < 4; i++) {
            if (*hours[i] < -720) *hours[i] = -720;
            if (*hours[i] > 2879) *hours[i] = 2879;
        }
        for (uint8_t i = 0; i < 4; i += 2) {
            if (*hours[i + 1] < *hours[i]) *hours[i + 1] = *hours[i];
        }
        if ((uint8_t)M.onAnchor > 2) M.onAnchor = Anchor::Clock;
        if ((uint8_t)M.offAnchor > 2) M.offAnchor = Anchor::Clock;
    }

    if (unitCount > SCENE_MAX_UNITS) unitCount = SCENE_MAX_UNITS;

    for (uint8_t u = 0; u < unitCount; ) {
        UnitConfig &U = units[u];
        U.name[SCENE_NAME_LEN - 1] = 0;
        U.building[SCENE_NAME_LEN - 1] = 0;
        // A unit is nothing without a model to follow, and there is no
        // model to give it that would be any more right than another.
        if (U.model >= modelCount) {
            for (uint8_t q = u; q + 1 < unitCount; q++) units[q] = units[q + 1];
            unitCount--;
            continue;
        }
        if (U.roomCount > UNIT_MAX_ROOMS) U.roomCount = UNIT_MAX_ROOMS;
        dedupUnitRooms(U);
        // A unit with no rooms left is kept: it is a thing on the layout
        // that has not been given its lamps yet.
        u++;
    }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

// One run of consecutive lamps, written the way a person would: a single
// number, a pair, or an "a-b" range.
static void runToJson(JsonArray arr, int start, int end) {
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
}

// A room's ranges, in the order they were given, so a list written as
// [0, "4-15"] comes back as [0, "4-15"].
static void roomLampsToJson(const RoomConfig &R, JsonArray arr) {
    for (uint8_t i = 0; i < R.rangeCount && i < ROOM_MAX_RANGES; i++) {
        runToJson(arr, R.ranges[i].from, R.ranges[i].to);
    }
}

// One element of a "lamps" array: a number, or an "a-b" string. Sets a and
// b to the run it names. Returns false with error set on nonsense; any is
// false for an element that is neither, which the caller skips.
static bool lampRunFromJson(JsonVariantConst v, long &a, long &b, bool &any,
                            String *error) {
    any = false;
    if (v.is<int>()) {
        long n = (int)v;
        if (n < 0 || n >= LAMPS_MAX_LAMPS) {
            if (error) *error = "lamp index out of range";
            return false;
        }
        a = b = n;
        any = true;
    } else if (v.is<const char *>()) {
        const char *s = v;
        const char *dash = strchr(s, '-');
        a = strtol(s, nullptr, 10);
        b = dash ? strtol(dash + 1, nullptr, 10) : a;
        if (a < 0 || b < a || b >= LAMPS_MAX_LAMPS) {
            if (error) *error = "lamp range out of range";
            return false;
        }
        any = true;
    }
    return true;
}

// A room's lamps straight into its ranges. A run that carries on from the
// one before is merged onto it, so [4, 5, 6] is one range and [0, "4-15"]
// is two.
static bool roomRangesFromJson(JsonArrayConst arr, RoomConfig &R, String *error) {
    R.rangeCount = 0;
    for (JsonVariantConst v : arr) {
        long a = 0, b = 0;
        bool any = false;
        if (!lampRunFromJson(v, a, b, any, error)) {
            return false;
        }
        if (!any) {
            continue;
        }
        if (R.rangeCount > 0 && R.ranges[R.rangeCount - 1].to + 1 == a) {
            R.ranges[R.rangeCount - 1].to = (uint16_t)b;
            continue;
        }
        if (!R.add((uint16_t)a, (uint16_t)b)) {
            if (error) *error = "too many ranges";
            return false;
        }
    }
    return true;
}

// One room object: the lamps in either form and the role. Shared by the
// unit loader and the migration, so an old flat's room and a new unit's
// room take the same syntax.
static bool roomFromJson(JsonObjectConst ro, RoomConfig &R, String *error) {
    R.rangeCount = 0;
    R.role = Room::Other;

    // "lamps" is the list form. "lamp" is what a scene file written before
    // a room could hold several lamps has, and it means a list of one.
    JsonArrayConst ls = ro["lamps"];
    if (!ls.isNull()) {
        if (!roomRangesFromJson(ls, R, error)) {
            return false;
        }
    } else if (ro["lamp"].is<int>()) {
        int lamp = ro["lamp"];
        if (lamp < 0 || lamp >= LAMPS_MAX_LAMPS) {
            if (error) *error = "room lamp index out of range";
            return false;
        }
        R.add((uint16_t)lamp, (uint16_t)lamp);
    }

    if (!SceneConfig::parseRoom(ro["role"] | "other", R.role)) {
        if (error) *error = "unknown room role";
        return false;
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

    JsonArray ms = doc["models"].to<JsonArray>();
    for (uint8_t m = 0; m < modelCount; m++) {
        const ModelConfig &M = models[m];
        JsonObject o = ms.add<JsonObject>();
        o["name"] = M.name;
        o["kind"] = kindName(M.kind);
        o["level"] = M.level;
        o["fadeMs"] = M.fadeMs;
        o["dayActivity"] = M.dayActivity;
        o["nightActivity"] = M.nightActivity;

        if (M.kind == ModelKind::Rhythm) {
            o["weekend"] = M.weekend;
            JsonArray wk = o["wake"].to<JsonArray>();
            wk.add(M.wakeFrom);
            wk.add(M.wakeTo);
            JsonArray lv = o["leave"].to<JsonArray>();
            lv.add(M.leaveFrom);
            lv.add(M.leaveTo);
            JsonArray hm = o["home"].to<JsonArray>();
            hm.add(M.homeFrom);
            hm.add(M.homeTo);
            JsonArray bd = o["bed"].to<JsonArray>();
            bd.add(M.bedFrom);
            bd.add(M.bedTo);
            o["outPercent"] = M.outPercent;
            o["tvPercent"] = M.tvPercent;
        } else {
            o["onAnchor"] = anchorName(M.onAnchor);
            JsonArray onW = o["on"].to<JsonArray>();
            onW.add(M.onFrom);
            onW.add(M.onTo);
            o["offAnchor"] = anchorName(M.offAnchor);
            JsonArray offW = o["off"].to<JsonArray>();
            offW.add(M.offFrom);
            offW.add(M.offTo);
            o["litPercent"] = M.litPercent;
            o["flickerPercent"] = M.flickerPercent;
            o["morning"] = M.morning;
            o["individual"] = M.individual;
        }
    }

    JsonArray us = doc["units"].to<JsonArray>();
    for (uint8_t u = 0; u < unitCount; u++) {
        const UnitConfig &U = units[u];
        JsonObject o = us.add<JsonObject>();
        o["name"] = U.name;
        o["building"] = U.building;
        // By name, so the document says what it means and a model moving in
        // the table does not rewrite every unit.
        o["model"] = U.model < modelCount ? models[U.model].name : "";
        JsonArray rs = o["rooms"].to<JsonArray>();
        for (uint8_t r = 0; r < U.roomCount; r++) {
            JsonObject ro = rs.add<JsonObject>();
            roomLampsToJson(U.rooms[r], ro["lamps"].to<JsonArray>());
            ro["role"] = roomName(U.rooms[r].role);
        }
    }

    out = "";
    serializeJson(doc, out);
}

// ---------------------------------------------------------------------------
// Migration. A document with "groups" or "flats" and no "models" was
// written before models and units existed. The two loaders below are the
// old ones, kept whole so the new-shape loader stays readable: each reads
// one old object into a model and a unit. The next save writes the new
// shape, and there is no path back.
// ---------------------------------------------------------------------------

// Did this flat keep the rhythm its household type gives? Everything the
// rhythm half carries, and the brightness and the life that go with it;
// the name is not part of it.
static bool sameRhythm(const ModelConfig &a, const ModelConfig &b) {
    return a.weekend == b.weekend &&
           a.wakeFrom == b.wakeFrom && a.wakeTo == b.wakeTo &&
           a.leaveFrom == b.leaveFrom && a.leaveTo == b.leaveTo &&
           a.homeFrom == b.homeFrom && a.homeTo == b.homeTo &&
           a.bedFrom == b.bedFrom && a.bedTo == b.bedTo &&
           a.outPercent == b.outPercent && a.tvPercent == b.tvPercent &&
           a.level == b.level && a.fadeMs == b.fadeMs &&
           a.dayActivity == b.dayActivity && a.nightActivity == b.nightActivity;
}

// One old flat: a rhythm model with the household's day, and a unit with
// the flat's name, building and rooms. tmpl is the template the flat's
// type named, and custom says it had none, which always gets a model of
// its own.
static bool migrateFlat(JsonObjectConst o, ModelConfig &M, UnitConfig &U,
                        Template &tmpl, bool &custom, String *error) {
    // The household types, in the order they had. The first four are the
    // first four templates; custom was "keep whatever is set", which the
    // family rhythm seeded.
    static const char *const householdNames[] = {
        "family", "elderly", "nightowl", "away", "custom"
    };
    const char *type = o["type"] | "family";
    int ti = -1;
    for (int i = 0; i < 5; i++) {
        if (!strcasecmp(type, householdNames[i])) {
            ti = i;
        }
    }
    if (ti < 0) {
        if (error) *error = "unknown household type";
        return false;
    }
    custom = (ti == 4);
    tmpl = custom ? Template::Family : (Template)ti;
    M.setTemplate(tmpl);

    strlcpy(U.name, o["name"] | "", SCENE_NAME_LEN);
    strlcpy(U.building, o["building"] | "", SCENE_NAME_LEN);
    U.model = 0;
    U.roomCount = 0;
    if (o["weekend"].is<bool>()) M.weekend = o["weekend"];

    JsonArrayConst rs = o["rooms"];
    for (JsonObjectConst ro : rs) {
        if (U.roomCount >= UNIT_MAX_ROOMS) {
            if (error) *error = "too many rooms in a unit";
            return false;
        }
        if (!roomFromJson(ro, U.rooms[U.roomCount], error)) {
            return false;
        }
        U.roomCount++;
    }

    // The rhythm. Absent means "whatever this type's preset says", which
    // setTemplate() has already put in place above.
    struct { const char *key; int16_t *from; int16_t *to; } ranges[4] = {
        { "wake",  &M.wakeFrom,  &M.wakeTo  },
        { "leave", &M.leaveFrom, &M.leaveTo },
        { "home",  &M.homeFrom,  &M.homeTo  },
        { "bed",   &M.bedFrom,   &M.bedTo   },
    };
    for (uint8_t i = 0; i < 4; i++) {
        JsonArrayConst w = o[ranges[i].key];
        if (w.size() == 2) {
            *ranges[i].from = w[0];
            *ranges[i].to = w[1];
        }
    }

    if (o["outPercent"].is<int>()) M.outPercent = o["outPercent"];
    if (o["tvPercent"].is<int>()) M.tvPercent = o["tvPercent"];
    if (o["level"].is<int>()) M.level = o["level"];
    if (o["fadeMs"].is<int>()) M.fadeMs = o["fadeMs"];
    if (o["dayActivity"].is<int>()) M.dayActivity = o["dayActivity"];
    if (o["nightActivity"].is<int>()) M.nightActivity = o["nightActivity"];
    return true;
}

// One old group: an hours model with the behaviour's habits and the
// group's name, and, when it had lamps, a unit of the same name whose
// rooms hold those lamps as ranges. U is null when there is no room for
// another unit, which reads the model and drops the lamps.
static bool migrateGroup(JsonObjectConst o, ModelConfig &M, UnitConfig *U,
                         String *error) {
    // The group behaviours, in the order they had, and what each one
    // becomes. "off" is not a template: it is a model that is never lit.
    static const char *const behaviourNames[] = {
        "off", "street", "home", "shop", "late", "allnight"
    };
    static const Template behaviourTemplate[] = {
        Template::Home, Template::Street, Template::Home,
        Template::Shop, Template::Pub, Template::AllNight
    };
    // The four household behaviours that were retired when flats arrived.
    // They loaded as "home" then and they migrate as Home now.
    static const char *const retiredNames[] = {
        "family", "elderly", "nightowl", "away"
    };

    const char *b = o["behaviour"] | "off";
    int bi = -1;
    for (int i = 0; i < 6; i++) {
        if (!strcasecmp(b, behaviourNames[i])) {
            bi = i;
        }
    }
    if (bi < 0) {
        for (const char *r : retiredNames) {
            if (!strcasecmp(b, r)) {
                bi = 2;             // home
            }
        }
    }
    if (bi < 0) {
        if (error) *error = "unknown behaviour";
        return false;
    }

    M.setTemplate(behaviourTemplate[bi]);
    if (bi == 0) {
        // Off: lit by nothing, at no time, with no life on top. A unit
        // that should be dark keeps its lamps and its place in the list.
        M.onAnchor = Anchor::Clock;  M.onFrom = 0;  M.onTo = 0;
        M.offAnchor = Anchor::Clock; M.offFrom = 0; M.offTo = 0;
        M.litPercent = 0;
        M.flickerPercent = 0;
        M.morning = false;
        M.level = 0;
        M.fadeMs = 800;
        M.dayActivity = 0;
        M.nightActivity = 0;
    }
    // Every lamp of a group kept its own moment inside the windows, its
    // own chance of taking part and its own television, which is what
    // individual means.
    M.individual = true;
    strlcpy(M.name, o["name"] | "", SCENE_NAME_LEN);

    if (o["onAnchor"].is<const char *>()) SceneConfig::parseAnchor(o["onAnchor"], M.onAnchor);
    if (o["offAnchor"].is<const char *>()) SceneConfig::parseAnchor(o["offAnchor"], M.offAnchor);
    JsonArrayConst onW = o["on"];
    if (onW.size() == 2) {
        M.onFrom = onW[0];
        M.onTo = onW[1];
    }
    JsonArrayConst offW = o["off"];
    if (offW.size() == 2) {
        M.offFrom = offW[0];
        M.offTo = offW[1];
    }
    if (o["litPercent"].is<int>()) M.litPercent = o["litPercent"];
    if (o["flickerPercent"].is<int>()) M.flickerPercent = o["flickerPercent"];
    if (o["morning"].is<bool>()) M.morning = o["morning"];
    if (o["level"].is<int>()) M.level = o["level"];
    if (o["fadeMs"].is<int>()) M.fadeMs = o["fadeMs"];

    // Absent means "whatever this behaviour's preset says", which
    // setTemplate() has already put in place above.
    if (o["dayActivity"].is<int>()) M.dayActivity = o["dayActivity"];
    if (o["nightActivity"].is<int>()) M.nightActivity = o["nightActivity"];

    if (!U) {
        return true;
    }
    strlcpy(U->name, o["name"] | "", SCENE_NAME_LEN);
    U->building[0] = 0;             // a group was never in a building
    U->model = 0;
    U->roomCount = 0;

    // The lamps, through a bitmap, so a list that repeats itself or runs
    // backwards comes out as the sorted runs the group loader made of it.
    // 256 bytes, which the core-0 stack can hold.
    uint8_t bits[LAMPS_MAX_LAMPS / 8];
    memset(bits, 0, sizeof(bits));
    JsonArrayConst ls = o["lamps"];
    for (JsonVariantConst v : ls) {
        long a = 0, b = 0;
        bool any = false;
        if (!lampRunFromJson(v, a, b, any, error)) {
            return false;
        }
        for (long i = a; any && i <= b; i++) {
            bits[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }

    // The runs, eight to a room and as many rooms as it takes. A run that
    // finds every room full is dropped: a group of more than ninety-six
    // runs was never a room in a building.
    int start = -1;
    for (int i = 0; i <= LAMPS_MAX_LAMPS; i++) {
        bool on = (i < LAMPS_MAX_LAMPS) && ((bits[i >> 3] >> (i & 7)) & 1);
        if (on && start < 0) {
            start = i;
        } else if (!on && start >= 0) {
            bool placed = U->roomCount > 0 &&
                          U->rooms[U->roomCount - 1].add((uint16_t)start,
                                                         (uint16_t)(i - 1));
            if (!placed && U->roomCount < UNIT_MAX_ROOMS) {
                RoomConfig &R = U->rooms[U->roomCount++];
                R.rangeCount = 0;
                R.role = Room::Other;
                R.add((uint16_t)start, (uint16_t)(i - 1));
            }
            start = -1;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------

bool SceneConfig::fromJson(const String &in, String *error) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, in);
    if (err) {
        if (error) *error = String("invalid JSON: ") + err.c_str();
        return false;
    }

    // Static: a SceneConfig is about 16 kB and the core-0 stack is far
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

    JsonArrayConst ms = doc["models"];
    if (!ms.isNull()) {
        next.modelCount = 0;
        for (JsonObjectConst o : ms) {
            if (next.modelCount >= SCENE_MAX_MODELS) {
                if (error) *error = "too many models";
                return false;
            }
            ModelConfig &M = next.models[next.modelCount];

            ModelKind k = ModelKind::Hours;
            if (!parseKind(o["kind"] | "", k)) {
                if (error) *error = "unknown model kind";
                return false;
            }
            // Absent means the template's value, so a model written by hand
            // only has to say what it does differently.
            M.setTemplate(k == ModelKind::Rhythm ? Template::Family : Template::Home);
            M.kind = k;

            const char *name = o["name"] | "";
            if (!name[0]) {
                if (error) *error = "model name required";
                return false;
            }
            strlcpy(M.name, name, SCENE_NAME_LEN);
            for (uint8_t i = 0; i < next.modelCount; i++) {
                if (!strcasecmp(next.models[i].name, M.name)) {
                    if (error) *error = "duplicate model name";
                    return false;
                }
            }

            if (o["level"].is<int>()) M.level = o["level"];
            if (o["fadeMs"].is<int>()) M.fadeMs = o["fadeMs"];
            if (o["dayActivity"].is<int>()) M.dayActivity = o["dayActivity"];
            if (o["nightActivity"].is<int>()) M.nightActivity = o["nightActivity"];

            if (k == ModelKind::Rhythm) {
                if (o["weekend"].is<bool>()) M.weekend = o["weekend"];
                struct { const char *key; int16_t *from; int16_t *to; } ranges[4] = {
                    { "wake",  &M.wakeFrom,  &M.wakeTo  },
                    { "leave", &M.leaveFrom, &M.leaveTo },
                    { "home",  &M.homeFrom,  &M.homeTo  },
                    { "bed",   &M.bedFrom,   &M.bedTo   },
                };
                for (uint8_t i = 0; i < 4; i++) {
                    JsonArrayConst w = o[ranges[i].key];
                    if (w.size() == 2) {
                        *ranges[i].from = w[0];
                        *ranges[i].to = w[1];
                    }
                }
                if (o["outPercent"].is<int>()) M.outPercent = o["outPercent"];
                if (o["tvPercent"].is<int>()) M.tvPercent = o["tvPercent"];
            } else {
                if (o["onAnchor"].is<const char *>()) parseAnchor(o["onAnchor"], M.onAnchor);
                if (o["offAnchor"].is<const char *>()) parseAnchor(o["offAnchor"], M.offAnchor);
                JsonArrayConst onW = o["on"];
                if (onW.size() == 2) {
                    M.onFrom = onW[0];
                    M.onTo = onW[1];
                }
                JsonArrayConst offW = o["off"];
                if (offW.size() == 2) {
                    M.offFrom = offW[0];
                    M.offTo = offW[1];
                }
                if (o["litPercent"].is<int>()) M.litPercent = o["litPercent"];
                if (o["flickerPercent"].is<int>()) M.flickerPercent = o["flickerPercent"];
                if (o["morning"].is<bool>()) M.morning = o["morning"];
                if (o["individual"].is<bool>()) M.individual = o["individual"];
            }

            next.modelCount++;
        }

        // The units, against the models just read. Absent means the units
        // stay as they are, which is what a document that only edits the
        // models looks like; clamp() then drops a unit whose model index
        // has gone. The GUI writes the whole document every time.
        JsonArrayConst us = doc["units"];
        if (!us.isNull()) {
            next.unitCount = 0;
            for (JsonObjectConst o : us) {
                if (next.unitCount >= SCENE_MAX_UNITS) {
                    if (error) *error = "too many units";
                    return false;
                }
                UnitConfig &U = next.units[next.unitCount];

                // By name, which is also what refuses a save that removes a
                // model some unit still follows.
                int mi = next.findModel(o["model"] | "");
                if (mi < 0) {
                    if (error) *error = "unknown model";
                    return false;
                }
                U.model = (uint8_t)mi;
                strlcpy(U.name, o["name"] | "", SCENE_NAME_LEN);
                strlcpy(U.building, o["building"] | "", SCENE_NAME_LEN);

                U.roomCount = 0;
                JsonArrayConst rs = o["rooms"];
                for (JsonObjectConst ro : rs) {
                    if (U.roomCount >= UNIT_MAX_ROOMS) {
                        if (error) *error = "too many rooms in a unit";
                        return false;
                    }
                    // A room with no lamps at all is kept here and dropped
                    // by clamp(), which is also what removes a lamp the
                    // unit already lists somewhere else.
                    if (!roomFromJson(ro, U.rooms[U.roomCount], error)) {
                        return false;
                    }
                    U.roomCount++;
                }
                next.unitCount++;
            }
        }
    } else if (!doc["flats"].isNull() || !doc["groups"].isNull()) {
        // The old shape. Flats first, so their unit indices, and with them
        // their room keys and their household draws, are the ones they had.
        next.modelCount = 0;
        next.unitCount = 0;

        // The model each template has been given, so two flats of one type
        // share it and a template model exists only once it is used.
        int8_t fromTemplate[(uint8_t)Template::COUNT];
        for (uint8_t i = 0; i < (uint8_t)Template::COUNT; i++) {
            fromTemplate[i] = -1;
        }

        JsonArrayConst fs = doc["flats"];
        for (JsonObjectConst o : fs) {
            if (next.unitCount >= SCENE_MAX_UNITS) {
                break;              // no room left on the layout
            }
            // Static: these are too big for the core-0 stack, and nothing
            // here nests with itself.
            static ModelConfig flatModel;
            static ModelConfig tmplModel;
            UnitConfig &U = next.units[next.unitCount];

            Template t = Template::Family;
            bool custom = false;
            if (!migrateFlat(o, flatModel, U, t, custom, error)) {
                return false;
            }
            tmplModel.setTemplate(t);

            // On the template's values, and not a custom flat: the template
            // model, made the first time a flat asks for it. Otherwise a
            // model of its own, named after the flat.
            bool onTemplate = !custom && sameRhythm(flatModel, tmplModel);
            int mi = onTemplate ? fromTemplate[(uint8_t)t] : -1;
            if (mi < 0) {
                if (next.modelCount >= SCENE_MAX_MODELS) {
                    continue;       // no model to point at: the unit goes
                }
                ModelConfig &M = next.models[next.modelCount];
                M = onTemplate ? tmplModel : flatModel;
                strlcpy(M.name, onTemplate ? templateTitle(t) : U.name, SCENE_NAME_LEN);
                uniqueName(next.models, next.modelCount, next.modelCount, M.name);
                if (onTemplate) {
                    fromTemplate[(uint8_t)t] = (int8_t)next.modelCount;
                }
                mi = next.modelCount++;
            }
            U.model = (uint8_t)mi;
            next.unitCount++;
        }

        // Then the groups. Each one is a model, and a unit as well when it
        // has lamps.
        JsonArrayConst gs = doc["groups"];
        for (JsonObjectConst o : gs) {
            if (next.modelCount >= SCENE_MAX_MODELS) {
                break;              // no room left in the model table
            }
            ModelConfig &M = next.models[next.modelCount];
            UnitConfig *U = next.unitCount < SCENE_MAX_UNITS
                                ? &next.units[next.unitCount] : nullptr;
            if (!migrateGroup(o, M, U, error)) {
                return false;
            }
            uniqueName(next.models, next.modelCount, next.modelCount, M.name);
            uint8_t mi = next.modelCount++;
            if (U && U->roomCount > 0) {
                U->model = mi;
                next.unitCount++;
            }
        }
    }
    // A document with none of the three keys leaves the models and the
    // units as they were, which is what a clock-only save looks like.

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
    // about 16 kB, too much for the stack, and fromJson() only commits on
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
