/*
    LampWebApi - the HTTP contract between the product's web GUI and the
                 lamp subsystem, independent of which HTTP server is used.

    Call handle() from whatever server you have (WiFiEspAT WiFiServer, the
    lwIP WebServer, anything that gives you method, path and body) and send
    back the status code and JSON it returns.

    Routes:

      GET  /api/lamps/config     current configuration
      PUT  /api/lamps/config     body: LampConfig JSON. Applies and stores it.
                                 Returns the status object.
      GET  /api/lamps/status     what is running, per-device health
      POST /api/lamps/probe      detect fitted hardware. Returns a LampConfig
                                 the GUI can offer to apply. Interrupts the
                                 lamps briefly.
      POST /api/lamps/test       body: {"level": 0..255} sets every lamp, or
                                 {"lamp": n, "level": 0..255} sets one.
                                 For a "Test" button in the GUI.

    Status object:

      {
        "hardware": "sx1503", "devices": 9, "lamps": 144,
        "intensity": false, "resolutionBits": 1,
        "faults": [false, false, true, ...]
      }

    Errors are {"error": "..."} with a 4xx code.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>

class LampWebApi {
public:
    // Returns the HTTP status code. Fills body with JSON. Content-Type is
    // always application/json. Returns 404 for paths it does not own, in
    // which case body is empty and you should carry on with your own routes.
    static int handle(const String &method, const String &path,
                      const String &requestBody, String &body);

    // Path prefix this handler owns, for routing.
    static bool owns(const String &path) { return path.startsWith("/api/lamps"); }

private:
    static int getConfig(String &body);
    static int putConfig(const String &in, String &body);
    static int getStatus(String &body);
    static int probe(String &body);
    static int test(const String &in, String &body);
    static void statusJson(String &body);
    static int error(int code, const char *msg, String &body);
};
