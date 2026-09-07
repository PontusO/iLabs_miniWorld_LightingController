/*
    SceneWebApi - HTTP contract for scenes, same shape as LampWebApi.

    Routes:

      GET  /api/scene/config    the running scene as JSON (see Scene.h)
      PUT  /api/scene/config    replace it and store it
      GET  /api/scene/status    where the simulation is right now
      PUT  /api/scene/clock     drive the clock without touching flash:
                                { "mode": "manual", "time": "19:40" }
                                { "mode": "accelerated", "dayMinutes": 20 }
                                { "dayOfYear": 355 }
                                { "enabled": false }
                                Any subset. Add "persist": true to keep it.
      GET  /api/scene/presets   default parameters per behaviour, for the
                                group editor in the GUI, plus "households"
                                (the rhythm per household type, no entry for
                                custom) and "rooms" (the room role names) for
                                the flat editor

    Status object:

      { "time": "19:42", "minutes": 1182, "dayOfYear": 249,
        "dusk": "19:58", "dawn": "05:47", "sunset": "19:21", "sunrise": "06:24",
        "mode": "real", "clockValid": true, "enabled": true,
        "lit": 61, "active": 2, "lamps": 144, "groups": 5,
        "flats": [ { "name": "Andersson", "state": "awake", "lit": "kl" } ] }

    "active" is the number of lamps currently inside a short activity event:
    a daytime light, an evening dip or a night wake-up.

    A flat's "state" is "asleep", "out", "awake", or "away" for the away
    household. "lit" is one letter per role that has a lit room, in role
    order and never repeated, whatever order the flat lists its rooms in:
    l living, k kitchen, b bedroom, t bathroom, h hall, o other, and an
    empty string when the flat is dark. Events count as lit.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>

class SceneWebApi {
public:
    static int handle(const String &method, const String &path,
                      const String &requestBody, String &body);

    static bool owns(const String &path) { return path.startsWith("/api/scene"); }

private:
    static int getConfig(String &body);
    static int putConfig(const String &in, String &body);
    static int getStatus(String &body);
    static int putClock(const String &in, String &body);
    static int getPresets(String &body);
    static void statusJson(String &body);
    static int error(int code, const char *msg, String &body);
};
