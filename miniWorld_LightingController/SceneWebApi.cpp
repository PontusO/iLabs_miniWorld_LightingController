/*
    SceneWebApi - see SceneWebApi.h

    Invector Embedded Systems AB
*/

#include "SceneWebApi.h"
#include "SceneEngine.h"
#include "Lamps.h"

#include <ArduinoJson.h>

int SceneWebApi::error(int code, const char *msg, String &body) {
    JsonDocument doc;
    doc["error"] = msg;
    body = "";
    serializeJson(doc, body);
    return code;
}

void SceneWebApi::statusJson(String &body) {
    JsonDocument doc;
    char t[8];

    SceneConfig::formatTime(Scene.simMinutes(), t, sizeof(t));
    doc["time"] = t;
    doc["minutes"] = Scene.simMinutes();
    doc["dayOfYear"] = Scene.dayOfYear();

    SceneConfig::formatTime(Scene.dusk(), t, sizeof(t));    doc["dusk"] = t;
    SceneConfig::formatTime(Scene.dawn(), t, sizeof(t));    doc["dawn"] = t;
    SceneConfig::formatTime(Scene.sunset(), t, sizeof(t));  doc["sunset"] = t;
    SceneConfig::formatTime(Scene.sunrise(), t, sizeof(t)); doc["sunrise"] = t;

    doc["mode"] = SceneConfig::modeName(Scene.config().mode);
    doc["dayMinutes"] = Scene.config().dayMinutes;
    doc["clockValid"] = Scene.clockValid();
    doc["enabled"] = Scene.enabled();
    doc["lit"] = Scene.litCount();
    doc["active"] = Scene.activeCount();
    doc["lamps"] = Lamps.count();
    doc["groups"] = Scene.config().groupCount;

    // One entry per flat, in config order: what the household is doing and
    // which of its rooms are lit right now.
    JsonArray fs = doc["flats"].to<JsonArray>();
    for (uint8_t f = 0; f < Scene.config().flatCount; f++) {
        char lit[FLAT_MAX_ROOMS + 1];
        Scene.flatLitRooms(f, lit, sizeof(lit));
        JsonObject o = fs.add<JsonObject>();
        o["name"] = Scene.config().flats[f].name;
        o["state"] = Scene.flatState(f);
        o["lit"] = lit;
    }

    body = "";
    serializeJson(doc, body);
}

int SceneWebApi::getConfig(String &body) {
    Scene.config().toJson(body);
    return 200;
}

int SceneWebApi::putConfig(const String &in, String &body) {
    // Static: a SceneConfig is about 4.8 kB and the core-0 stack is far
    // smaller. One request is served at a time, so this never nests.
    static SceneConfig cfg;
    cfg = Scene.config();
    String why;
    if (!cfg.fromJson(in, &why)) {
        return error(400, why.c_str(), body);
    }
    if (!Scene.apply(cfg, true)) {
        return error(500, "applied but could not be stored", body);
    }
    // Let the engine settle one tick, as putClock does, so the flat states
    // in the reply are the new scene's and not the old one's.
    Scene.tick();
    statusJson(body);
    return 200;
}

int SceneWebApi::getStatus(String &body) {
    statusJson(body);
    return 200;
}

int SceneWebApi::putClock(const String &in, String &body) {
    JsonDocument doc;
    if (deserializeJson(doc, in)) {
        return error(400, "invalid JSON", body);
    }

    if (doc["mode"].is<const char *>()) {
        ClockMode m;
        if (!SceneConfig::parseMode(doc["mode"], m)) {
            return error(400, "mode must be real, accelerated or manual", body);
        }
        Scene.setMode(m);
    }

    if (doc["time"].is<const char *>()) {
        uint16_t minutes;
        if (!SceneConfig::parseTime(doc["time"], minutes)) {
            return error(400, "time must be HH:MM", body);
        }
        Scene.setManualTime(minutes);
    } else if (doc["time"].is<int>()) {
        Scene.setManualTime((uint16_t)((int)doc["time"]));
    }

    if (doc["dayMinutes"].is<int>()) {
        int d = doc["dayMinutes"];
        if (d < 1 || d > 1440) {
            return error(400, "dayMinutes must be 1..1440", body);
        }
        Scene.setDayMinutes((uint16_t)d);
    }

    if (doc["dayOfYear"].is<int>()) {
        int d = doc["dayOfYear"];
        if (d < 1 || d > 366) {
            return error(400, "dayOfYear must be 1..366", body);
        }
        Scene.setDayOfYear((uint16_t)d);
    }

    if (doc["enabled"].is<bool>()) {
        Scene.setEnabled(doc["enabled"]);
    }

    if (doc["persist"] | false) {
        SceneStore::save(Scene.config());
    }

    // Let the engine settle one tick so the status reflects the change.
    Scene.tick();
    statusJson(body);
    return 200;
}

int SceneWebApi::postIdentify(const String &in, String &body) {
    JsonDocument doc;
    if (deserializeJson(doc, in)) {
        return error(400, "invalid JSON", body);
    }

    uint16_t count = Lamps.count();
    int lamp = doc["lamp"].is<int>() ? (int)doc["lamp"] : -1;
    if (lamp < 0 || lamp >= (int)count) {
        if (count == 0) {
            return error(400, "no lamps are fitted", body);
        }
        char msg[32];
        snprintf(msg, sizeof(msg), "lamp must be 0..%d", (int)count - 1);
        return error(400, msg, body);
    }

    // Returns at once: the blink is five phases of tick(), not a wait here.
    Scene.identify((uint16_t)lamp);

    JsonDocument out;
    out["ok"] = true;
    out["lamp"] = lamp;
    body = "";
    serializeJson(out, body);
    return 200;
}

int SceneWebApi::getPresets(String &body) {
    JsonDocument doc;
    for (uint8_t i = 1; i < (uint8_t)Behaviour::COUNT; i++) {
        // Static: a GroupConfig carries the 256 byte lamp bitmap, and
        // setPreset() fills in every field this loop reads.
        static GroupConfig G;
        G.setPreset((Behaviour)i);
        JsonObject o = doc[SceneConfig::behaviourName((Behaviour)i)].to<JsonObject>();
        o["onAnchor"] = SceneConfig::anchorName(G.onAnchor);
        JsonArray onW = o["on"].to<JsonArray>();
        onW.add(G.onFrom);
        onW.add(G.onTo);
        o["offAnchor"] = SceneConfig::anchorName(G.offAnchor);
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

    // The household rhythms, for the flat editor. No entry for custom: it is
    // "keep what I typed" and re-seeds from nothing.
    JsonObject hh = doc["households"].to<JsonObject>();
    for (uint8_t i = 0; i < (uint8_t)Household::COUNT; i++) {
        if ((Household)i == Household::Custom) {
            continue;
        }
        // Static: a FlatConfig carries the room table, and setPreset() fills
        // in every field this loop reads.
        static FlatConfig F;
        F.setPreset((Household)i);
        JsonObject o = hh[SceneConfig::householdName((Household)i)].to<JsonObject>();
        o["weekend"] = F.weekend;
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

    JsonArray rooms = doc["rooms"].to<JsonArray>();
    for (uint8_t i = 0; i < (uint8_t)Room::COUNT; i++) {
        rooms.add(SceneConfig::roomName((Room)i));
    }

    body = "";
    serializeJson(doc, body);
    return 200;
}

int SceneWebApi::handle(const String &method, const String &path,
                        const String &requestBody, String &body) {
    body = "";

    if (path == "/api/scene/config") {
        if (method == "GET") return getConfig(body);
        if (method == "PUT" || method == "POST") return putConfig(requestBody, body);
        return error(405, "method not allowed", body);
    }
    if (path == "/api/scene/status") {
        if (method == "GET") return getStatus(body);
        return error(405, "method not allowed", body);
    }
    if (path == "/api/scene/clock") {
        if (method == "PUT" || method == "POST") return putClock(requestBody, body);
        return error(405, "method not allowed", body);
    }
    if (path == "/api/scene/identify") {
        if (method == "POST") return postIdentify(requestBody, body);
        return error(405, "method not allowed", body);
    }
    if (path == "/api/scene/presets") {
        if (method == "GET") return getPresets(body);
        return error(405, "method not allowed", body);
    }
    return error(404, "not found", body);
}
