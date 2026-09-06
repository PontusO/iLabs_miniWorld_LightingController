/*
    LampWebApi - see LampWebApi.h

    Invector Embedded Systems AB
*/

#include "LampWebApi.h"
#include "Lamps.h"

#include <ArduinoJson.h>

int LampWebApi::error(int code, const char *msg, String &body) {
    JsonDocument doc;
    doc["error"] = msg;
    body = "";
    serializeJson(doc, body);
    return code;
}

void LampWebApi::statusJson(String &body) {
    JsonDocument doc;
    const LampConfig &cfg = Lamps.config();

    doc["lamps"] = Lamps.count();
    doc["devices"] = Lamps.deviceCount();
    doc["intensity"] = Lamps.intensitySupported();
    doc["resolutionBits"] = Lamps.count() ? Lamps.resolutionBits(0) : 0;

    JsonArray buses = doc["buses"].to<JsonArray>();
    for (uint8_t b = 0; b < LAMPS_NUM_BUSES; b++) {
        const BusConfig &bc = cfg.buses[b];
        JsonObject bo = buses.add<JsonObject>();
        bo["sx1503"] = bc.sx1503 ? (Lamps.busSx1503Faulted(b) ? "fault" : "ok") : "none";

        JsonArray al = bo["al5887"].to<JsonArray>();
        for (uint8_t n = 0; n < bc.al5887; n++) {
            al.add(Lamps.busAl5887Faulted(b, n) ? "fault" : "ok");
        }
    }

    JsonArray faults = doc["faults"].to<JsonArray>();
    for (uint8_t i = 0; i < Lamps.deviceCount(); i++) {
        faults.add(Lamps.deviceFaulted(i));
    }

    body = "";
    serializeJson(doc, body);
}

int LampWebApi::getConfig(String &body) {
    Lamps.config().toJson(body);
    return 200;
}

int LampWebApi::putConfig(const String &in, String &body) {
    LampConfig cfg = Lamps.config();
    String why;
    if (!cfg.fromJson(in, &why)) {
        return error(400, why.c_str(), body);
    }

    bool ok = Lamps.apply(cfg, true);
    statusJson(body);
    // Configuration was applied and stored even if a device is missing;
    // the faults array says which. 200 either way, the GUI reads faults.
    (void)ok;
    return 200;
}

int LampWebApi::getStatus(String &body) {
    statusJson(body);
    return 200;
}

int LampWebApi::probe(String &body) {
    LampConfig found = Lamps.probe();
    found.toJson(body);
    return 200;
}

int LampWebApi::test(const String &in, String &body) {
    JsonDocument doc;
    if (deserializeJson(doc, in)) {
        return error(400, "invalid JSON", body);
    }
    if (!doc["level"].is<int>()) {
        return error(400, "level required", body);
    }
    int level = doc["level"];
    if (level < 0 || level > 255) {
        return error(400, "level must be 0..255", body);
    }

    if (doc["lamp"].is<int>()) {
        int lamp = doc["lamp"];
        if (lamp < 0 || lamp >= Lamps.count()) {
            return error(400, "lamp out of range", body);
        }
        Lamps.setIntensity((uint16_t)lamp, (uint8_t)level);
    } else {
        Lamps.setAll((uint8_t)level);
    }

    if (!Lamps.show()) {
        return error(503, "a device did not respond", body);
    }
    body = "{}";
    return 200;
}

int LampWebApi::handle(const String &method, const String &path,
                       const String &requestBody, String &body) {
    body = "";

    if (path == "/api/lamps/config") {
        if (method == "GET") {
            return getConfig(body);
        }
        if (method == "PUT" || method == "POST") {
            return putConfig(requestBody, body);
        }
        return error(405, "method not allowed", body);
    }

    if (path == "/api/lamps/status") {
        if (method == "GET") {
            return getStatus(body);
        }
        return error(405, "method not allowed", body);
    }

    if (path == "/api/lamps/probe") {
        if (method == "POST") {
            return probe(body);
        }
        return error(405, "method not allowed", body);
    }

    if (path == "/api/lamps/test") {
        if (method == "POST") {
            return test(requestBody, body);
        }
        return error(405, "method not allowed", body);
    }

    return error(404, "not found", body);
}
