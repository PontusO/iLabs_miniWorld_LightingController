/*
    miniWorld lighting controller.

    setup() brings up the lamps, the scene and the network, then starts the
    HTTP server. loop() is three ticks: the WiFi state machine, one HTTP
    request, and the scene simulation.

    HttpServer serves the whole GUI: the single page from WebUI.gen.h, which
    the Makefile bakes into the image from web/, and the JSON APIs owned by
    LampWebApi, SceneWebApi, NetWebApi and SystemWebApi. NetManager owns the
    network: credentials, the captive portal, mDNS, NTP and the timezone.

    WiFi on the Challenger NB RP2040 WiFi is an ESP8285 co-processor on a
    UART driven by AT commands, so WiFiEspAT is required. The arduino-pico
    core's own WiFi.h (lwIP over the Pico W's CYW43) does not work here.

    Build with make at the repo root: it runs buildweb.py on web/ and pioasm
    on i2c.pio before arduino-cli, neither of which the core does for a
    sketch.

    Libraries: ArduinoJson 7, WiFiEspAT. The Makefile sets the LittleFS
    size; building from the IDE instead, set one in the board menu.

    Invector Embedded Systems AB
*/

#include <WiFiEspAT.h>
#include <LittleFS.h>
#include "Version.h"
#include "Lamps.h"
#include "SceneEngine.h"
#include "NetConfig.h"
#include "NetManager.h"
#include "HttpServer.h"

void setup() {
    Serial.begin(115200);
    delay(1500);                // give USB CDC a moment so the boot log is visible
    Serial.printf("miniWorld lighting controller %s (%s)\n", MINIWORLD_VERSION, MINIWORLD_BUILD);

    Lamps.begin();              // applies whatever the GUI last saved
    Serial.printf("lamps: %u devices, %u lamps\n", Lamps.deviceCount(), Lamps.count());

    Scene.begin();              // /scene.json, or an empty scene if there is none

    // Seed the nine models when there is nothing to start from: no stored
    // scene, or one whose models were never written. No units come with
    // them, since only the person building the layout knows what is on it,
    // and a model needs no lamps to exist.
    bool sceneEmpty = (Scene.config().modelCount == 0);
    if (sceneEmpty) {
        // Static: a SceneConfig is about 16 kB, too much for the stack.
        static SceneConfig s;
        s.seedTemplates();
        Scene.apply(s, true);
        Serial.println("scene: seeded the nine models");
    }

#ifdef MINIWORLD_ERASE_NET
    // Bench escape hatch: a build made with DEFINES=-DMINIWORLD_ERASE_NET
    // forgets the stored network so the compile-time defaults apply again.
    // Flash a normal build afterwards, or every boot erases it.
    NetConfigStore::erase();
    Serial.println("net: stored network erased by MINIWORLD_ERASE_NET");
#endif
    Net.begin();                // ESP8285, stored credentials or the portal
    Http.begin(80);
}

void loop() {
    Net.tick();                 // the WiFi state machine
    Http.tick();                // at most one request, so the town keeps moving
    Scene.tick();               // the town goes about its evening
}
