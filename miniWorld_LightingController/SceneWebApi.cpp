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
    doc["lamps"] = Lamps.count();
    doc["groups"] = Scene.config().groupCount;

    body = "";
    serializeJson(doc, body);
}

int SceneWebApi::getConfig(String &body) {
    Scene.config().toJson(body);
    return 200;
}

int SceneWebApi::putConfig(const String &in, String &body) {
    SceneConfig cfg = Scene.config();
    String why;
    if (!cfg.fromJson(in, &why)) {
        return error(400, why.c_str(), body);
    }
    if (!Scene.apply(cfg, true)) {
        return error(500, "applied but could not be stored", body);
    }
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

int SceneWebApi::getPresets(String &body) {
    JsonDocument doc;
    for (uint8_t i = 1; i < (uint8_t)Behaviour::COUNT; i++) {
        GroupConfig G;
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
    if (path == "/api/scene/presets") {
        if (method == "GET") return getPresets(body);
        return error(405, "method not allowed", body);
    }
    return 404;
}
