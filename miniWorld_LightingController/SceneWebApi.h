/*
    SceneWebApi - HTTP contract for scenes, same shape as LampWebApi.

    Routes:

      GET  /api/scene/config    the running scene as JSON (see Scene.h)
      PUT  /api/scene/config    replace it and store it. The document may be
                                partial. One with only "units" is applied
                                against the models the scene already has.
                                One with only "models" re-resolves the
                                existing units by the model names they were
                                following and drops a unit whose model name
                                is gone, so renaming a model without sending
                                the units drops the units that followed the
                                old name. One with neither leaves models and
                                units as they were. A save that removes a
                                model some sent unit still names is refused
                                with "unknown model".
      GET  /api/scene/status    where the simulation is right now
      PUT  /api/scene/clock     drive the clock without touching flash:
                                { "mode": "manual", "time": "19:40" }
                                { "mode": "accelerated", "dayMinutes": 20 }
                                { "dayOfYear": 355 }
                                { "enabled": false }
                                Any subset. Add "persist": true to keep it.
      POST /api/scene/identify  { "lamp": n } blinks that lamp for a second
                                and a quarter so it can be found on the
                                layout, then gives it back to the scene.
                                { "lamps": [n, ...] } blinks up to eight of
                                them together, which is how a whole room is
                                found. Each must be a fitted lamp; more than
                                eight is "at most 8 lamps".
                                Returns { "ok": true, "lamp": n,
                                "lamps": [n, ...] }, where "lamp" is the
                                first of them.
      GET  /api/scene/presets   "models", the nine templates keyed by their
                                template key, each a whole model object as
                                the scene document writes it and named by
                                its title, which is what "New model, start
                                from" offers, plus "rooms", the room role
                                names, for the unit editor

    Status object:

      { "time": "19:42", "minutes": 1182, "dayOfYear": 249,
        "dusk": "19:58", "dawn": "05:47", "sunset": "19:21", "sunrise": "06:24",
        "mode": "real", "clockValid": true, "enabled": true,
        "lit": 61, "active": 2, "lamps": 144,
        "units": [ { "name": "Andersson", "state": "awake", "lit": "kl" } ] }

    "active" is the number of lamps currently inside a short activity event:
    a daytime light, an evening dip or a night wake-up.

    "loadError" is there only when a stored /scene.json existed at boot and
    would not load, and carries the loader's reason. The scene is then the
    empty one and the file has been left exactly as it is. A device whose
    scene loaded, or that had nothing stored, does not send the key at all.

    A unit's "state" is "asleep", "out", "awake", or "away" under a rhythm
    model, and "open" or "closed" under an hours model that is anchored to
    the clock at both ends, "lit" or "dark" otherwise. "lit" is one letter
    per role that has a lit room, in role order and never repeated, whatever
    order the unit lists its rooms in: l living, k kitchen, b bedroom,
    t bathroom, h hall, f front, r back, s sign, o other, and an empty
    string when the unit is dark. Events count as lit.

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
    static int postIdentify(const String &in, String &body);
    static int getPresets(String &body);
    static void statusJson(String &body);
    static int error(int code, const char *msg, String &body);
};
