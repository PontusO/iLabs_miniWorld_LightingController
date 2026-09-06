/*
    NetWebApi - see NetWebApi.h

    Invector Embedded Systems AB
*/

#include "NetWebApi.h"
#include "NetManager.h"

#include <ArduinoJson.h>
#include <string.h>

// Up to twelve networks in a scan, as the GUI list shows.
static const int SCAN_MAX = 12;

int NetWebApi::error(int code, const char *msg, String &body) {
    JsonDocument doc;
    doc["error"] = msg;
    body = "";
    serializeJson(doc, body);
    return code;
}

void NetWebApi::statusJson(String &body) {
    JsonDocument doc;

    doc["mode"] = NetManager::modeName(Net.mode());
    doc["ssid"] = Net.ssid();
    // The station address only means anything when Online. Empty rather than
    // 0.0.0.0, so the views can treat the field as "no address yet".
    doc["ip"] = (Net.mode() == NetMode::Online) ? Net.ip().toString() : String();
    doc["rssi"] = Net.rssi();
    doc["hostname"] = Net.config().hostname;
    doc["apSsid"] = Net.apSsid();
    doc["apIp"] = Net.apIP().toString();
    doc["apActive"] = Net.apActive();
    doc["connectResult"] = NetManager::resultName(Net.connectResult());
    doc["auth"] = Net.config().hasPassword();
    doc["timeValid"] = Net.timeValid();

    body = "";
    serializeJson(doc, body);
}

int NetWebApi::getStatus(String &body) {
    statusJson(body);
    return 200;
}

int NetWebApi::getScan(String &body) {
    WiFiApData list[SCAN_MAX];
    int n = Net.scan(list, SCAN_MAX);

    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject o = networks.add<JsonObject>();
        o["ssid"] = list[i].ssid;
        o["rssi"] = list[i].rssi;
        // WiFiApData.enc is the raw AT <ecn> code, not a wl_enc_type: the
        // library only maps it in encryptionType(index). 0 is an open AP.
        o["secure"] = list[i].enc != 0;
    }

    body = "";
    serializeJson(doc, body);
    return 200;
}

int NetWebApi::getConfig(String &body) {
    Net.config().toJson(body, false);
    return 200;
}

int NetWebApi::putConfig(const String &in, String &body) {
    NetConfig cfg = Net.config();
    String why;
    if (!cfg.fromJson(in, &why)) {
        return error(400, why.c_str(), body);
    }
    // applyConfig takes hostname, password, ntp and tz only; credentials in
    // the body are ignored, /api/net/connect is the way in for those.
    if (!Net.applyConfig(cfg)) {
        return error(500, "applied but could not be stored", body);
    }
    Net.config().toJson(body, false);
    return 200;
}

int NetWebApi::postConnect(const String &in, String &body) {
    JsonDocument doc;
    if (deserializeJson(doc, in)) {
        return error(400, "invalid JSON", body);
    }
    if (!doc["ssid"].is<const char *>()) {
        return error(400, "ssid is required", body);
    }

    const char *ssid = doc["ssid"];
    const char *pass = doc["pass"].is<const char *>() ? (const char *)doc["pass"] : "";

    if (ssid[0] == 0 || strlen(ssid) >= NET_SSID_LEN) {
        return error(400, "ssid must be 1 to 32 characters", body);
    }
    if (strlen(pass) >= NET_PASS_LEN) {
        return error(400, "pass too long", body);
    }

    Net.connectTo(ssid, pass);
    statusJson(body);
    return 202;
}

int NetWebApi::postForget(String &body) {
    Net.forget();
    statusJson(body);
    return 200;
}

int NetWebApi::handle(const String &method, const String &path,
                      const String &requestBody, String &body) {
    body = "";

    if (path == "/api/net/status") {
        if (method == "GET") return getStatus(body);
        return error(405, "method not allowed", body);
    }
    if (path == "/api/net/scan") {
        if (method == "GET") return getScan(body);
        return error(405, "method not allowed", body);
    }
    if (path == "/api/net/config") {
        if (method == "GET") return getConfig(body);
        if (method == "PUT") return putConfig(requestBody, body);
        return error(405, "method not allowed", body);
    }
    if (path == "/api/net/connect") {
        if (method == "POST") return postConnect(requestBody, body);
        return error(405, "method not allowed", body);
    }
    if (path == "/api/net/forget") {
        if (method == "POST") return postForget(body);
        return error(405, "method not allowed", body);
    }
    return error(404, "not found", body);
}
