/*
    NetConfig - see NetConfig.h

    Invector Embedded Systems AB
*/

#include "NetConfig.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>

// ---------------------------------------------------------------------------

static bool isValidHostname(const char *s) {
    size_t len = strlen(s);
    if (len < 1 || len > 24) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool NetConfig::clamp() {
    char beforeHostname[NET_HOST_LEN];
    char beforeNtp[NET_NTP_LEN];
    char beforeTz[NET_TZ_LEN];
    strlcpy(beforeHostname, hostname, sizeof(beforeHostname));
    strlcpy(beforeNtp, ntp, sizeof(beforeNtp));
    strlcpy(beforeTz, tz, sizeof(beforeTz));

    // Host names are case insensitive, so an entered "MyHost" is the same
    // name as "myhost". Fold before validating rather than discarding the
    // whole name to the default over a capital letter.
    for (size_t i = 0; hostname[i] != 0; i++) {
        if (hostname[i] >= 'A' && hostname[i] <= 'Z') {
            hostname[i] = (char)(hostname[i] - 'A' + 'a');
        }
    }

    if (!isValidHostname(hostname)) {
        strlcpy(hostname, "miniworld", sizeof(hostname));
    }
    if (ntp[0] == 0) {
        strlcpy(ntp, "pool.ntp.org", sizeof(ntp));
    }
    if (tz[0] == 0) {
        strlcpy(tz, "UTC0", sizeof(tz));
    }

    return strcmp(hostname, beforeHostname) == 0 &&
           strcmp(ntp, beforeNtp) == 0 &&
           strcmp(tz, beforeTz) == 0;
}

void NetConfig::toJson(String &out, bool includeSecrets) const {
    JsonDocument doc;

    JsonObject sta = doc["sta"].to<JsonObject>();
    sta["ssid"] = ssid;
    if (includeSecrets) {
        sta["pass"] = pass;
    }

    doc["hostname"] = hostname;
    if (includeSecrets) {
        doc["password"] = password;
    }
    doc["ntp"] = ntp;
    doc["tz"] = tz;

    if (!includeSecrets) {
        doc["hasWifi"] = hasWifi();
        doc["hasPassword"] = hasPassword();
    }

    out = "";
    serializeJson(doc, out);
}

// Copies src into dst (size dstSize) if it fits, including the terminator.
// Reports "<fieldName> too long" and returns false otherwise.
static bool setField(char *dst, size_t dstSize, const char *src, const char *fieldName, String *error) {
    if (strlen(src) >= dstSize) {
        if (error) {
            *error = String(fieldName) + " too long";
        }
        return false;
    }
    strlcpy(dst, src, dstSize);
    return true;
}

bool NetConfig::fromJson(const String &in, String *error) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, in);
    if (err) {
        if (error) {
            *error = String("invalid JSON: ") + err.c_str();
        }
        return false;
    }

    NetConfig next = *this;

    if (doc["sta"]["ssid"].is<const char *>()) {
        if (!setField(next.ssid, sizeof(next.ssid), doc["sta"]["ssid"], "ssid", error)) {
            return false;
        }
    }
    if (doc["sta"]["pass"].is<const char *>()) {
        if (!setField(next.pass, sizeof(next.pass), doc["sta"]["pass"], "pass", error)) {
            return false;
        }
    }
    if (doc["hostname"].is<const char *>()) {
        if (!setField(next.hostname, sizeof(next.hostname), doc["hostname"], "hostname", error)) {
            return false;
        }
    }
    if (doc["password"].is<const char *>()) {
        if (!setField(next.password, sizeof(next.password), doc["password"], "password", error)) {
            return false;
        }
    }
    if (doc["ntp"].is<const char *>()) {
        if (!setField(next.ntp, sizeof(next.ntp), doc["ntp"], "ntp", error)) {
            return false;
        }
    }
    if (doc["tz"].is<const char *>()) {
        if (!setField(next.tz, sizeof(next.tz), doc["tz"], "tz", error)) {
            return false;
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

bool NetConfigStore::load(NetConfig &cfg) {
    if (!fsReady()) {
        return false;
    }
    File f = LittleFS.open(path(), "r");
    if (!f) {
        return false;
    }
    String body = f.readString();
    f.close();

    NetConfig parsed;
    if (!parsed.fromJson(body)) {
        return false;
    }
    cfg = parsed;
    return true;
}

bool NetConfigStore::save(const NetConfig &cfg) {
    if (!fsReady()) {
        return false;
    }
    String body;
    cfg.toJson(body, true);

    File f = LittleFS.open(path(), "w");
    if (!f) {
        return false;
    }
    size_t n = f.print(body);
    f.close();
    return n == body.length();
}

bool NetConfigStore::erase() {
    if (!fsReady()) {
        return false;
    }
    return LittleFS.remove(path());
}
