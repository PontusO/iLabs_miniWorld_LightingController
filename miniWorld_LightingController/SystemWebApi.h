/*
    SystemWebApi - HTTP contract for system status and control.

    Routes:

      GET  /api/system/status    firmware version, build time, uptime,
                                 heap, system time, time validity
      POST /api/system/reboot    trigger a system reboot (does not reboot
                                 immediately; the flag is read by the caller)

    Status object:

      {"firmware":"0.1.0","build":"Sep  6 2026 19:40:12","uptime":1234,
       "heap":180000,"time":"2026-09-06T19:40:12","timeValid":true}

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>

class SystemWebApi {
public:
    static int handle(const String &method, const String &path,
                      const String &requestBody, String &body);

    static bool owns(const String &path) { return path.startsWith("/api/system"); }

    // Set by handle() when a reboot was requested; the server reboots
    // after the response has been sent.
    static bool rebootPending();

private:
    static int getStatus(String &body);
    static int postReboot(String &body);
    static int error(int code, const char *msg, String &body);
};
