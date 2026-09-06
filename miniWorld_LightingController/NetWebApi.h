/*
    NetWebApi - HTTP contract for the network, same shape as LampWebApi.

    Routes:

      GET  /api/net/status    the status object below
      GET  /api/net/scan      { "networks": [ { "ssid", "rssi", "secure" } ] }
                              up to 12, strongest first. A blocking scan of
                              a few seconds.
      GET  /api/net/config    NetConfig without secrets, plus hasWifi and
                              hasPassword
      PUT  /api/net/config    any subset of hostname, password, ntp, tz.
                              Saves, applies hostname/ntp/tz live when
                              Online, returns the config. sta.ssid and
                              sta.pass are ignored here, use connect. PUT
                              only, POST is 405.
      POST /api/net/connect   { "ssid", "pass" } stores the credentials and
                              starts an attempt, 202 with the status object
      POST /api/net/forget    clears the credentials, goes to Portal,
                              returns the status object

    Status object:

      { "mode": "nomodule" | "connecting" | "online" | "portal",
        "ssid": "Home", "ip": "192.168.1.42", "rssi": -61,
        "hostname": "miniworld",
        "apSsid": "miniWorld-1A2B", "apIp": "192.168.4.1", "apActive": true,
        "connectResult": "idle" | "connecting" | "ok" | "failed",
        "auth": false, "timeValid": true }

    ip is the station address, and is "" in any mode other than online: the
    portal has no station address, and the views read the field as "no
    address yet" when it is empty.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>

class NetWebApi {
public:
    static int handle(const String &method, const String &path,
                      const String &requestBody, String &body);

    static bool owns(const String &path) { return path.startsWith("/api/net"); }

private:
    static int getStatus(String &body);
    static int getScan(String &body);
    static int getConfig(String &body);
    static int putConfig(const String &in, String &body);
    static int postConnect(const String &in, String &body);
    static int postForget(String &body);
    static void statusJson(String &body);
    static int error(int code, const char *msg, String &body);
};
