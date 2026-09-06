/*
    NetConfig - WiFi station credentials, GUI access and network settings.

    Stored in LittleFS as /net.json. Owned by NetConfig, applied by
    NetManager at boot and on change through the web GUI.

    JSON shape, which is also the GUI contract:

        {
          "sta":      { "ssid": "", "pass": "" },
          "hostname": "miniworld",
          "password": "",
          "ntp":      "pool.ntp.org",
          "tz":       "CET-1CEST,M3.5.0,M10.5.0/3"
        }

    "password" is the GUI password: empty means no authentication. "hostname"
    is used for mDNS (<hostname>.local) and the DHCP host name, lower-case
    letters, digits and hyphens, 1..24 characters; clamp() fixes anything
    else to "miniworld".

    Passwords are stored in clear text in LittleFS. This is the norm for
    this class of device and is stated here so nobody is surprised.

    toJson(out, includeSecrets): the store writes sta.pass and password
    (includeSecrets=true), the API never returns them (includeSecrets=false)
    and instead adds "hasWifi" and "hasPassword" booleans at top level.

    Default network: with no /net.json stored, ssid and pass start as
    NET_DEFAULT_SSID and NET_DEFAULT_PASS from NetDefaults.h, so a fresh
    board joins a known network instead of raising the portal. Copy
    NetDefaults.example.h to NetDefaults.h if the real file is missing.

    Dependencies: ArduinoJson 7, LittleFS (arduino-pico core, set a
    filesystem size in the board menu or every save will fail).

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include "NetDefaults.h"

#define NET_SSID_LEN  33
#define NET_PASS_LEN  65
#define NET_HOST_LEN  25
#define NET_NTP_LEN   48
#define NET_TZ_LEN    48

struct NetConfig {
    char ssid[NET_SSID_LEN]     = NET_DEFAULT_SSID;
    char pass[NET_PASS_LEN]     = NET_DEFAULT_PASS;
    char hostname[NET_HOST_LEN] = "miniworld";
    char password[NET_PASS_LEN] = "";
    char ntp[NET_NTP_LEN]       = "pool.ntp.org";
    char tz[NET_TZ_LEN]         = "CET-1CEST,M3.5.0,M10.5.0/3";

    bool hasWifi() const { return ssid[0] != 0; }
    bool hasPassword() const { return password[0] != 0; }

    // Hostname to lower-case [a-z0-9-], 1..24 chars, else "miniworld".
    // Empty ntp becomes "pool.ntp.org", empty tz becomes "UTC0".
    // Returns true if nothing changed.
    bool clamp();

    // includeSecrets: pass and password are written. The store uses true,
    // the API uses false and adds "hasWifi" and "hasPassword".
    void toJson(String &out, bool includeSecrets) const;

    // Merge: fields present in the JSON overwrite, absent ones are kept.
    // "sta": {"ssid","pass"} nested as in the spec. Strings too long
    // are an error naming the field.
    bool fromJson(const String &in, String *error = nullptr);
};

class NetConfigStore {
public:
    static const char *path() { return "/net.json"; }
    static bool load(NetConfig &cfg);
    static bool save(const NetConfig &cfg);
    static bool erase();
};
