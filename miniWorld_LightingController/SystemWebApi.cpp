/*
    SystemWebApi - see SystemWebApi.h

    Invector Embedded Systems AB
*/

#include "SystemWebApi.h"
#include "Version.h"

#include <ArduinoJson.h>
#include <time.h>

static bool reboot_pending = false;

int SystemWebApi::error(int code, const char *msg, String &body) {
    JsonDocument doc;
    doc["error"] = msg;
    body = "";
    serializeJson(doc, body);
    return code;
}

int SystemWebApi::getStatus(String &body) {
    JsonDocument doc;
    time_t now = time(nullptr);
    // localtime() answers null for a time it cannot break down. Report no
    // time at all rather than formatting from a null tm.
    struct tm *timeinfo = localtime(&now);
    char time_str[20] = "";
    if (timeinfo) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%S", timeinfo);
    }

    doc["firmware"] = MINIWORLD_VERSION;
    doc["build"] = MINIWORLD_BUILD;
    doc["uptime"] = (uint32_t)(millis() / 1000);
    doc["heap"] = (uint32_t)rp2040.getFreeHeap();
    doc["time"] = time_str;
    doc["timeValid"] = (timeinfo != nullptr) && (now > 1700000000);

    body = "";
    serializeJson(doc, body);
    return 200;
}

int SystemWebApi::postReboot(String &body) {
    reboot_pending = true;
    JsonDocument doc;
    doc["ok"] = true;
    body = "";
    serializeJson(doc, body);
    return 200;
}

int SystemWebApi::handle(const String &method, const String &path,
                         const String &requestBody, String &body) {
    body = "";

    if (path == "/api/system/status") {
        if (method == "GET") return getStatus(body);
        return error(405, "method not allowed", body);
    }
    if (path == "/api/system/reboot") {
        if (method == "POST") return postReboot(body);
        return error(405, "method not allowed", body);
    }
    return error(404, "not found", body);
}

bool SystemWebApi::rebootPending() {
    return reboot_pending;
}
