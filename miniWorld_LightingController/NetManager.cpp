/*
    NetManager - see NetManager.h

    Invector Embedded Systems AB
*/

#include "NetManager.h"

#include <ChallengerWiFi.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

NetManager Net;

// ---------------------------------------------------------------------------
// Timings. All of them are from the design document, section 3.3.
// ---------------------------------------------------------------------------

static const uint32_t CONNECT_TIMEOUT_MS = 20000;    // Connecting -> Portal
static const uint32_t LOSS_TIMEOUT_MS    = 120000;   // Online -> Portal
static const uint32_t RETRY_INTERVAL_MS  = 300000;   // Portal -> Connecting
static const uint32_t AP_LINGER_MS       = 60000;    // AP kept after onboarding
static const uint32_t MODULE_RETRY_MS    = 10000;    // NoModule reset retry
static const uint32_t AP_RETRY_MS        = 10000;    // Portal startAp() retry
static const uint32_t TIME_POLL_MS       = 3600000;  // hourly once time is valid
static const uint32_t TIME_POLL_FAST_MS  = 10000;    // until it is

// WiFi.status() is an AT round trip. Polling it on every pass through loop()
// would keep the ESP link busy and starve the HTTP server, so it is sampled
// once a second. The state timeouts are still measured against millis().
static const uint32_t STATUS_POLL_MS     = 1000;

static const uint32_t ESP_FAST_BAUDRATE  = 921600;
static const uint32_t READY_TIMEOUT_MS   = 2000;     // reset to the "ready" line
static const uint32_t AT_TIMEOUT_MS      = 500;      // one AT command to "OK"

// A UNIX time below this is not a real time, it is the AT firmware answering
// before SNTP has resolved anything.
static const unsigned long TIME_FLOOR = 1700000000UL;

// ---------------------------------------------------------------------------

const char *NetManager::modeName(NetMode m) {
    switch (m) {
        case NetMode::NoModule:   return "nomodule";
        case NetMode::Connecting: return "connecting";
        case NetMode::Online:     return "online";
        case NetMode::Portal:     return "portal";
    }
    return "nomodule";
}

const char *NetManager::resultName(ConnectResult r) {
    switch (r) {
        case ConnectResult::Idle:       return "idle";
        case ConnectResult::Connecting: return "connecting";
        case ConnectResult::Ok:         return "ok";
        case ConnectResult::Failed:     return "failed";
    }
    return "idle";
}

// ---------------------------------------------------------------------------
// Module bring-up
// ---------------------------------------------------------------------------

// Reads Serial2 until token has been seen or the deadline passes, throwing
// away everything else. The AT firmware frames its answers in CRLF, so a
// substring match is enough and cheaper than line buffering.
static bool waitForToken(const char *token, uint32_t timeoutMs) {
    size_t len = strlen(token);
    size_t matched = 0;
    uint32_t start = millis();

    while (millis() - start < timeoutMs) {
        int c = Serial2.read();
        if (c < 0) {
            continue;
        }
        if (c == token[matched]) {
            matched++;
            if (matched == len) {
                return true;
            }
        } else {
            matched = (c == token[0]) ? 1 : 0;
        }
    }
    return false;
}

// Throws away whatever the module says for the next few milliseconds, so a
// rate change does not leave half a garbled line in the buffer.
static void drainSerial2(uint32_t forMs) {
    uint32_t start = millis();
    while (millis() - start < forMs) {
        Serial2.read();
    }
}

// The variant helper's waitForReady(), isAlive() and therefore reset() and
// changeBaudRate() all return true when they time out: their loops end with
// "while (... && timeout--)" followed by "if (timeout)", which tests -1 and
// is true. A dead module would take about 27 s to not be detected. The waits
// are done here instead, against real deadlines. Only runReset(), which just
// toggles two pins, is taken from the helper.
//
// Order matters. WiFi.init() sends its own AT+RST, and AT+UART_CUR is a
// current-setting that does not survive a reset, so the rate is raised
// after the library is initialised, not before. Nothing else in this file
// may call WiFi.init(): every init puts the module's UART back to 115200.
bool NetManager::bringUpModule() {
    _moduleUp = false;

    // A previous attempt may have left the port at the fast rate.
    Serial2.end();
    Serial2.begin(DEFAULT_ESP_BAUDRATE);

    // PIN_ESP_MODE high for a normal start, then a 1 ms low pulse on
    // PIN_ESP_RST. The module answers "ready" when the AT interpreter is up.
    // This is only to reach a known state; the library resets it again.
    Challenger2040WiFi.runReset();
    if (!waitForToken("ready", READY_TIMEOUT_MS)) {
        return false;
    }
    drainSerial2(20);

    WiFi.init(Serial2);
    if (WiFi.status() == WL_NO_MODULE) {
        return false;
    }

    // Raise the rate by hand. The driver holds a Stream pointer to Serial2,
    // so re-opening the port at the new rate is all it takes on this side.
    uint32_t baud = DEFAULT_ESP_BAUDRATE;
    Serial2.print("AT+UART_CUR=");
    Serial2.print(ESP_FAST_BAUDRATE);
    Serial2.print(",8,1,0,0\r\n");

    if (waitForToken("OK", AT_TIMEOUT_MS)) {
        Serial2.end();
        Serial2.begin(ESP_FAST_BAUDRATE);
        delay(50);
        drainSerial2(20);
        baud = ESP_FAST_BAUDRATE;

        if (WiFi.status() == WL_NO_MODULE) {    // a real AT command over the fast link
            Serial2.end();
            Serial2.begin(DEFAULT_ESP_BAUDRATE);
            delay(50);
            drainSerial2(20);
            WiFi.init(Serial2);                 // the module is back at its default
            baud = DEFAULT_ESP_BAUDRATE;
            Serial.println("net: esp stays at 115200");
            if (WiFi.status() == WL_NO_MODULE) {
                return false;
            }
        }
    } else {
        Serial.println("net: esp stays at 115200");
    }

    WiFi.setPersistent(false);                 // we manage credentials ourselves

    // apMacAddress() leaves the buffer partly untouched when the query fails
    // (its clearing loop runs over five of the six bytes), so start from a
    // known value.
    uint8_t mac[6] = { 0, 0, 0, 0, 0, 0 };
    WiFi.apMacAddress(mac);
    snprintf(_apSsid, sizeof(_apSsid), "miniWorld-%02X%02X", mac[4], mac[5]);

    Serial.printf("net: esp link %lu\n", (unsigned long)baud);

    _moduleUp = true;
    return true;
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

bool NetManager::begin() {
    NetConfigStore::load(_cfg);                // absent file leaves the defaults

    if (!bringUpModule()) {
        enterNoModule();
        return false;
    }

    // With nothing stored, ask the AT firmware to join whatever network it
    // remembers. That is what brings a previously used board up on a bench.
    _triedPersisted = !_cfg.hasWifi();
    enterConnecting(false);
    return true;
}

void NetManager::tick() {
    uint32_t now = millis();

    switch (_mode) {

    case NetMode::NoModule:
        if (now - _lastModuleTry < MODULE_RETRY_MS) {
            return;
        }
        _lastModuleTry = now;
        if (!bringUpModule()) {
            return;
        }
        Serial.println("net: module back");
        _triedPersisted = !_cfg.hasWifi();
        enterConnecting(false);
        break;

    case NetMode::Connecting: {
        if (_apUp) {
            // connectTo() keeps the AP up on purpose, so the phone has to
            // keep resolving names for the whole 20 s attempt.
            _dns.tick();
        }
        bool failed = false;
        if (now - _lastStatus >= STATUS_POLL_MS) {
            _lastStatus = now;
            uint8_t st = WiFi.status();
            if (st == WL_NO_MODULE) {
                enterNoModule();
                break;
            }
            if (st == WL_CONNECTED) {
                enterOnline();
                break;
            }
            // WiFi.begin() blocks until the AT firmware answers, so a refused
            // join is already known here. The 20 s window is the backstop.
            failed = (st == WL_CONNECT_FAILED);
        }
        if (failed || now - _stateSince >= CONNECT_TIMEOUT_MS) {
            if (_result == ConnectResult::Connecting) {
                _result = ConnectResult::Failed;
            }
            // A remembered network that will not join is not worth retrying
            // every five minutes. One that has joined at least once is.
            if (_triedPersisted && _moduleSsid[0] == 0) {
                _triedPersisted = false;
            }
            enterPortal();
        }
        break;
    }

    case NetMode::Online: {
        uint32_t interval = _timeValid ? TIME_POLL_MS : TIME_POLL_FAST_MS;
        if (now - _lastTimePoll >= interval) {
            _lastTimePoll = now;
            pollTime();
        }

        if (now - _lastStatus >= STATUS_POLL_MS) {
            _lastStatus = now;
            uint8_t st = WiFi.status();
            if (st == WL_NO_MODULE) {
                enterNoModule();
                break;
            }
            if (st == WL_CONNECTED) {
                _lostSince = 0;
            } else if (_lostSince == 0) {
                _lostSince = now ? now : 1;    // 0 is the "not lost" marker
            }
        }
        if (_lostSince != 0 && now - _lostSince >= LOSS_TIMEOUT_MS) {
            Serial.println("net: connection lost");
            enterPortal();
            break;
        }

        // The AP outlives onboarding by a minute so the portal page can show
        // the new address before the phone is dropped. It keeps answering
        // DNS for as long as it is up.
        if (_apUp) {
            _dns.tick();
            if ((int32_t)(now - _apDropAt) >= 0) {
                stopAp();
            }
        }
        break;
    }

    case NetMode::Portal:
        if (!_apUp && now - _lastApTry >= AP_RETRY_MS) {
            startAp();              // the AP or its DNS did not come up
        }
        _dns.tick();
        if ((_cfg.hasWifi() || _triedPersisted) && now - _lastRetry >= RETRY_INTERVAL_MS) {
            enterConnecting(true);
        }
        break;
    }
}

void NetManager::enterNoModule() {
    _moduleUp = false;
    if (_apUp) {
        stopAp();               // an AP on a module that is gone is a lie
    }
    _mode = NetMode::NoModule;
    _stateSince = millis();
    _lastModuleTry = _stateSince;
    _lostSince = 0;
    Serial.println("net: nomodule");
}

void NetManager::enterConnecting(bool keepAp) {
    _mode = NetMode::Connecting;
    _stateSince = millis();
    _lastRetry = _stateSince;
    _lastStatus = _stateSince - STATUS_POLL_MS;    // resolve on the next tick

    if (!keepAp && _apUp) {
        stopAp();
    }

    if (_triedPersisted) {
        Serial.println("net: connecting to the remembered network");
        WiFi.begin();
    } else {
        Serial.printf("net: connecting %s\n", _cfg.ssid);
        WiFi.begin(_cfg.ssid, _cfg.pass);
    }
}

void NetManager::enterOnline() {
    _mode = NetMode::Online;
    _result = (_result == ConnectResult::Connecting) ? ConnectResult::Ok : ConnectResult::Idle;
    _stateSince = millis();
    _lostSince = 0;
    _lastTimePoll = _stateSince - TIME_POLL_FAST_MS;   // ask for the time at once

    if (_triedPersisted) {
        // Display only, and deliberately not in _cfg: the passphrase stays
        // in the AT firmware, and an ssid saved to /net.json without one
        // would be retried with an empty passphrase forever.
        _moduleSsid[0] = 0;
        WiFi.SSID(_moduleSsid);
    }

    applyOnlineServices();

    if (_apUp) {
        _apDropAt = millis() + AP_LINGER_MS;
    }

    Serial.printf("net: online %s %s\n", ssid().c_str(), ip().toString().c_str());
}

void NetManager::enterPortal() {
    _mode = NetMode::Portal;
    _stateSince = millis();
    if (_result != ConnectResult::Failed) {
        _result = ConnectResult::Idle;     // never report a stale "ok"
    }
    if (!_apUp) {
        startAp();
    }
    Serial.printf("net: portal %s 192.168.4.1\n", _apSsid);
}

void NetManager::startAp() {
    _lastApTry = millis();
    WiFi.configureAP(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                     IPAddress(255, 255, 255, 0));
    // beginAP() answers WL_AP_LISTENING once the AT firmware has the AP up.
    // Anything else means there is no AP, so _apUp stays false and the
    // Portal tick tries again.
    if (WiFi.beginAP(_apSsid, nullptr, 1, ENC_TYPE_NONE) != WL_AP_LISTENING) {
        Serial.println("net: ap failed");
        return;
    }
    // The catch-all DNS only redirects phones to the portal if the AT
    // firmware's DHCP server hands out the AP address as the name server.
    // ESP-IDF's DHCP server does that by default [stated], not verified on
    // this firmware.
    if (!_dns.begin(apIP())) {
        Serial.println("net: ap failed");
        WiFi.endAP();
        return;
    }
    _apUp = true;
}

void NetManager::stopAp() {
    _dns.end();
    WiFi.endAP();
    _apUp = false;
    Serial.println("net: ap down");
}

void NetManager::applyOnlineServices() {
    WiFi.setHostname(_cfg.hostname);
    WiFi.startMDNS(_cfg.hostname, "http", 80);
    WiFi.sntp(_cfg.ntp);
    setenv("TZ", _cfg.tz, 1);
    tzset();
}

void NetManager::pollTime() {
    unsigned long t = WiFi.getTime();
    if (t > TIME_FLOOR) {
        struct timeval tv = { (time_t)t, 0 };
        settimeofday(&tv, nullptr);
        _timeValid = true;
    }
}

// ---------------------------------------------------------------------------
// Commands from the web API
// ---------------------------------------------------------------------------

bool NetManager::applyConfig(const NetConfig &cfg) {
    strlcpy(_cfg.hostname, cfg.hostname, sizeof(_cfg.hostname));
    strlcpy(_cfg.password, cfg.password, sizeof(_cfg.password));
    strlcpy(_cfg.ntp, cfg.ntp, sizeof(_cfg.ntp));
    strlcpy(_cfg.tz, cfg.tz, sizeof(_cfg.tz));
    _cfg.clamp();

    bool stored = NetConfigStore::save(_cfg);
    if (_mode == NetMode::Online) {
        applyOnlineServices();
    }
    return stored;
}

void NetManager::connectTo(const char *ssid, const char *pass) {
    strlcpy(_cfg.ssid, ssid ? ssid : "", sizeof(_cfg.ssid));
    strlcpy(_cfg.pass, pass ? pass : "", sizeof(_cfg.pass));
    _triedPersisted = false;
    _moduleSsid[0] = 0;
    NetConfigStore::save(_cfg);

    _result = ConnectResult::Connecting;
    enterConnecting(true);          // the phone stays on the AP meanwhile
}

void NetManager::forget() {
    _cfg.ssid[0] = 0;
    _cfg.pass[0] = 0;
    _triedPersisted = false;
    _moduleSsid[0] = 0;
    NetConfigStore::save(_cfg);
    _result = ConnectResult::Idle;

    // Leave the network too, and persistently: on the ESP8285 the AP
    // follows the station channel, so a station left associated would move
    // the portal off channel 1 under the phone's feet, and a network still
    // remembered by the AT firmware would be rejoined at the next boot
    // through the no-argument WiFi.begin().
    WiFi.disconnect(true);
    _lostSince = 0;
    enterPortal();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

IPAddress NetManager::ip() const {
    if (_mode != NetMode::Online) {
        return IPAddress(0, 0, 0, 0);
    }
    return WiFi.localIP();
}

int NetManager::rssi() const {
    if (_mode != NetMode::Online) {
        return 0;
    }
    return (int)WiFi.RSSI();
}

String NetManager::ssid() const {
    if (_mode != NetMode::Online) {
        return String();
    }
    return String(_triedPersisted ? _moduleSsid : _cfg.ssid);
}

int NetManager::scan(WiFiApData *out, int max) {
    if (!out || max <= 0 || !_moduleUp) {
        return 0;
    }
    if (max > 255) {
        max = 255;
    }

    int n = WiFi.scanNetworks(out, (uint8_t)max);
    if (n <= 0) {
        return 0;
    }

    for (int i = 1; i < n; i++) {          // insertion sort, strongest first
        WiFiApData key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}
