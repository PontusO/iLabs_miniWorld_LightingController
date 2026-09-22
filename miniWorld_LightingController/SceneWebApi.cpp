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

    // Only when there is one, so a healthy device answers exactly what it
    // answered before this key existed.
    if (Scene.loadError()[0]) {
        doc["loadError"] = Scene.loadError();
    }

    // One entry per unit, in config order: what it is doing and which of
    // its rooms are lit right now.
    JsonArray us = doc["units"].to<JsonArray>();
    for (uint8_t u = 0; u < Scene.config().unitCount; u++) {
        char lit[(uint8_t)Room::COUNT + 1];
        Scene.unitLitRooms(u, lit, sizeof(lit));
        JsonObject o = us.add<JsonObject>();
        o["name"] = Scene.config().units[u].name;
        o["state"] = Scene.unitState(u);
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
    // Static: a SceneConfig is about 12 kB and the core-0 stack is far
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
    // Let the engine settle one tick, as putClock does, so the unit states
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
    if (count == 0) {
        return error(400, "no lamps are fitted", body);
    }

    // "lamps" is a whole room blinking together; "lamp" is one lamp, which
    // is what the GUI sent before rooms held more than one.
    uint16_t lamps[SceneEngine::IDENT_MAX_LAMPS];
    uint8_t n = 0;
    JsonArrayConst arr = doc["lamps"];
    if (!arr.isNull()) {
        if (arr.size() > SceneEngine::IDENT_MAX_LAMPS) {
            return error(400, "at most 8 lamps", body);
        }
        for (JsonVariantConst v : arr) {
            int lamp = v.is<int>() ? (int)v : -1;
            if (lamp < 0 || lamp >= (int)count) {
                char msg[32];
                snprintf(msg, sizeof(msg), "lamp must be 0..%d", (int)count - 1);
                return error(400, msg, body);
            }
            lamps[n++] = (uint16_t)lamp;
        }
    } else {
        int lamp = doc["lamp"].is<int>() ? (int)doc["lamp"] : -1;
        if (lamp < 0 || lamp >= (int)count) {
            char msg[32];
            snprintf(msg, sizeof(msg), "lamp must be 0..%d", (int)count - 1);
            return error(400, msg, body);
        }
        lamps[n++] = (uint16_t)lamp;
    }
    if (n == 0) {
        char msg[32];
        snprintf(msg, sizeof(msg), "lamp must be 0..%d", (int)count - 1);
        return error(400, msg, body);
    }

    // Returns at once: the blink is five phases of tick(), not a wait here.
    Scene.identify(lamps, n);

    JsonDocument out;
    out["ok"] = true;
    out["lamp"] = lamps[0];         // the single-lamp reply, still true
    JsonArray ls = out["lamps"].to<JsonArray>();
    for (uint8_t i = 0; i < n; i++) {
        ls.add(lamps[i]);
    }
    body = "";
    serializeJson(out, body);
    return 200;
}

int SceneWebApi::getPresets(String &body) {
    JsonDocument doc;

    // The nine templates, each a whole model object as the scene document
    // writes it, keyed by the template's key and named by its title. This
    // is what "New model, start from" offers.
    JsonObject ms = doc["models"].to<JsonObject>();
    for (uint8_t i = 0; i < (uint8_t)Template::COUNT; i++) {
        Template t = (Template)i;
        // Static: a config object does not go on the core-0 stack, and
        // setTemplate() fills in every field this loop reads.
        static ModelConfig M;
        M.setTemplate(t);
        JsonObject o = ms[SceneConfig::templateKey(t)].to<JsonObject>();
        o["name"] = SceneConfig::templateTitle(t);
        o["kind"] = SceneConfig::kindName(M.kind);
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
            o["onAnchor"] = SceneConfig::anchorName(M.onAnchor);
            JsonArray onW = o["on"].to<JsonArray>();
            onW.add(M.onFrom);
            onW.add(M.onTo);
            o["offAnchor"] = SceneConfig::anchorName(M.offAnchor);
            JsonArray offW = o["off"].to<JsonArray>();
            offW.add(M.offFrom);
            offW.add(M.offTo);
            o["litPercent"] = M.litPercent;
            o["flickerPercent"] = M.flickerPercent;
            o["morning"] = M.morning;
            o["individual"] = M.individual;
        }
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
