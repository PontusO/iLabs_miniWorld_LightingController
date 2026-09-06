/*
    Application view: the Lamps API plus the web API hooked to an HTTP
    server. The request loop below is a minimal adaptor written against the
    WiFiClient interface of WiFiEspAT. Replace it with the product's own
    server; the only thing it has to do is call LampWebApi::handle() for
    paths it owns.

    WiFi on the Challenger NB RP2040 WiFi is an ESP8285 co-processor on a
    UART driven by AT commands, so WiFiEspAT is required. The arduino-pico
    core's own WiFi.h (lwIP over the Pico W's CYW43) does not work here.

    Build with make at the repo root: it runs pioasm on i2c.pio before
    arduino-cli, which the core does not do for sketch-local .pio files.

    Libraries: ArduinoJson 7, WiFiEspAT. The Makefile sets the LittleFS
    size; building from the IDE instead, set one in the board menu.

    Invector Embedded Systems AB
*/

#include <WiFiEspAT.h>
#include <LittleFS.h>
#include "Lamps.h"
#include "LampWebApi.h"
#include "SceneEngine.h"
#include "SceneWebApi.h"

WiFiServer server(80);

void setup() {
    Serial.begin(115200);

    // ...WiFi bring-up for the product goes here...
    server.begin();

    // Local time for the real-time clock mode. Central European with DST.
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    // ...NTP sync goes here, so time(nullptr) is right before Scene runs...

    Lamps.begin();              // applies whatever the GUI last saved
    Serial.printf("%s, %u lamps\n",
                  LampConfig::hardwareName(Lamps.config().hardware),
                  Lamps.count());

    if (!Scene.begin()) {
        // Nothing stored yet: seed a town so there is something to look at.
        SceneConfig s;
        auto group = [&](const char *name, Behaviour b, uint16_t from, uint16_t to) {
            GroupConfig &G = s.groups[s.groupCount++];
            G.setPreset(b);
            strlcpy(G.name, name, SCENE_NAME_LEN);
            G.clearLamps();
            for (uint16_t i = from; i <= to && i < Lamps.count(); i++) G.add(i);
        };
        group("Street lights", Behaviour::Street,   0,  15);
        group("Flats",         Behaviour::Home,     16, 95);
        group("Shops",         Behaviour::Shop,     96, 119);
        group("Pub and grill", Behaviour::Late,     120, 127);
        group("Kiosk, church", Behaviour::AllNight, 128, 143);
        Scene.apply(s, true);
    }
}

// ---------------------------------------------------------------------------
// Minimal HTTP adaptor. Not a web server, just enough to route the lamp API
// and serve the settings page from flash.
// ---------------------------------------------------------------------------

static void sendResponse(WiFiClient &c, int code, const char *type, const String &body) {
    c.printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
             code, code == 200 ? "OK" : "Error", type, (unsigned)body.length());
    c.print(body);
}

static void serveRequest(WiFiClient &c) {
    String line = c.readStringUntil('\n');
    int sp1 = line.indexOf(' ');
    int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) {
        return;
    }
    String method = line.substring(0, sp1);
    String path = line.substring(sp1 + 1, sp2);

    size_t contentLength = 0;
    while (c.connected()) {
        String h = c.readStringUntil('\n');
        h.trim();
        if (h.length() == 0) {
            break;
        }
        if (h.startsWith("Content-Length:")) {
            contentLength = h.substring(15).toInt();
        }
    }

    String body;
    while (body.length() < contentLength && c.connected()) {
        if (c.available()) {
            body += (char)c.read();
        }
    }

    if (LampWebApi::owns(path) || SceneWebApi::owns(path)) {
        String out;
        int code = LampWebApi::owns(path)
                 ? LampWebApi::handle(method, path, body, out)
                 : SceneWebApi::handle(method, path, body, out);
        sendResponse(c, code, "application/json", out);
        return;
    }

    if (path == "/" || path == "/lamps.html") {
        File f = LittleFS.open("/lamps.html", "r");
        if (f) {
            String page = f.readString();
            f.close();
            sendResponse(c, 200, "text/html", page);
            return;
        }
    }

    sendResponse(c, 404, "text/plain", "not found");
}

void loop() {
    WiFiClient client = server.accept();
    if (client) {
        serveRequest(client);
        client.stop();
    }

    Scene.tick();               // the town goes about its evening
}
