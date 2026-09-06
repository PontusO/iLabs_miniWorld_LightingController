# Web GUI and Captive Portal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the miniWorld lighting controller a captive-portal WiFi onboarding flow and a single-page product GUI, served by the RP2040 over its ESP8285 AT co-processor.

**Architecture:** A `NetManager` state machine (Connecting, Online, Portal) owns the ESP8285 through WiFiEspAT, with a UDP `DnsResponder` for the portal. An `HttpServer` routes captive probes, basic auth, four JSON web APIs and one gzipped single-page UI embedded in the firmware as a generated header. The UI is vanilla HTML/CSS/JS in `web/`, built by a Python script and developed against a Python mock of the API.

**Tech Stack:** arduino-pico core 5.5.1, WiFiEspAT 2.0.0 (AT 2.x dialect, the library default), ArduinoJson 7, LittleFS, Python 3 standard library for tooling, GNU make.

**Spec:** `docs/superpowers/specs/2026-09-06-web-gui-captive-portal-design.md`. Read it first; the plan argues from it.

## Global Constraints

- No em dashes anywhere: code, comments, HTML, docs, commit messages. Use commas, colons, parentheses or a full stop.
- Every new source file's header comment ends with `Invector Embedded Systems AB`.
- Web API handlers are `static int handle(const String &method, const String &path, const String &requestBody, String &body)` returning the HTTP code, plus `static bool owns(const String &path)`. They never touch a server class.
- Nothing served from the device fetches anything external: no fonts, no CDN, no framework.
- Never call `Serial1.begin()`. Debug is USB CDC `Serial`. Serial2 is the ESP link.
- Build and lint step is `make` at the repo root; it must stay warning-free with `--warnings default`.
- This directory is not a git repository. Skip every "commit" step; there is nothing to commit to. Do not run `git init`.
- Levels through `LampDriver` stay `uint16_t`.
- Hardware facts not read from a datasheet are labelled as such in a comment.
- Python tooling is standard library only.

## File structure

Firmware, in `miniWorld_LightingController/`:

| File | Responsibility |
|---|---|
| `LampConfig.h/.cpp` (modify) | per-bus hardware model, JSON, LittleFS |
| `Lamps.h/.cpp` (modify) | build devices per bus, probe per bus, constants |
| `LampWebApi.h/.cpp` (modify) | status JSON with per-bus summary |
| `Version.h` (create) | `MINIWORLD_VERSION`, build stamp |
| `NetConfig.h/.cpp` (create) | `/net.json` model and store |
| `DnsResponder.h/.cpp` (create) | catch-all DNS over UDP for the portal |
| `NetManager.h/.cpp` (create) | ESP bring-up, state machine, SNTP, mDNS, AP |
| `NetWebApi.h/.cpp` (create) | `/api/net/*` |
| `SystemWebApi.h/.cpp` (create) | `/api/system/*` |
| `HttpServer.h/.cpp` (create) | request parsing, auth, routing, static UI |
| `WebUI.gen.h` (generated) | gzipped UI bytes, length, ETag |
| `miniWorld_LightingController.ino` (modify) | wiring only |
| `lamps.html` (delete) | superseded by the SPA |

Web and tooling, at the repo root:

| File | Responsibility |
|---|---|
| `web/index.html` | page shell, header, view containers |
| `web/app.css` | tokens, layout, shared components |
| `web/app.js` | api helper, router, toast, shared widgets |
| `web/view-home.css/.js` | Home view |
| `web/view-wifi.css/.js` | WiFi view and portal landing |
| `web/view-lamps.css/.js` | Lamps view |
| `web/view-scene.css/.js` | Scene view with scrubber and group editor |
| `tools/buildweb.py` | inline, minify lightly, gzip, emit `WebUI.gen.h` |
| `tools/mockserver.py` | fake API plus static `web/` for off-target work |
| `tools/apicheck.sh` | curl smoke test of every route |
| `Makefile` (modify) | `web`, `mock`, `check` targets, `compile` depends on `web` |

`tools/buildweb.py` inlines CSS in this order: `app.css`, then `view-*.css` sorted by name. JS: `app.js`, then `view-*.js` sorted by name. Each view file registers itself with `App.register(name, view)` from `app.js`, so order among views does not matter.

## Task dependency graph

```
T1 per-bus lamps ─────────────────────────────┐
T2 NetConfig ──┐                              │
T3 Version + SystemWebApi ─┐                  │
T4 DnsResponder ───────────┼─> T5 NetManager + NetWebApi ─> T6 HttpServer + sketch ─┐
T7 buildweb + mock + apicheck + web shell ────────────────────────────────────────────┼─> T12 docs, review, size
       └─> T8 Home view ─┐                                                            │
       └─> T9 WiFi view ─┼────────────────────────────────────────────────────────────┘
       └─> T10 Lamps view┤
       └─> T11 Scene view┘                                                     T13 flash and bench
```

T1, T2, T3, T4 and T7 are independent and may run in parallel. T8 to T11 may run in parallel after T7. T6 needs T3 and T5. T12 needs everything. T13 last.

---

### Task 1: Per-bus lamp hardware model

**Files:**
- Modify: `miniWorld_LightingController/LampConfig.h`, `LampConfig.cpp`
- Modify: `miniWorld_LightingController/Lamps.h`, `Lamps.cpp`
- Modify: `miniWorld_LightingController/LampWebApi.h`, `LampWebApi.cpp`
- Modify: `miniWorld_LightingController/miniWorld_LightingController.ino` (only the boot print that calls `LampConfig::hardwareName`)
- Delete: `miniWorld_LightingController/lamps.html`

**Interfaces:**
- Produces:

```cpp
// LampConfig.h
#define LAMPS_NUM_BUSES 12
#define LAMPS_MAX_AL5887_PER_BUS 4
struct BusConfig { bool sx1503 = false; uint8_t al5887 = 0; };
struct LampConfig {
    uint32_t busSpeed = 400000;
    bool activeLow = false;
    bool rgb = false;
    BusConfig buses[LAMPS_NUM_BUSES];
    uint16_t lampCount() const;              // sum over buses: 16*sx1503 + 36*al5887
    uint8_t deviceCount() const;             // sum over buses: sx1503 + al5887
    bool anyHardware() const;
    bool clamp();                            // al5887 <= 4, busSpeed in {100000, 400000, 1000000}
    bool operator==(const LampConfig &o) const;
    bool operator!=(const LampConfig &o) const;
    void toJson(String &out) const;
    bool fromJson(const String &in, String *error = nullptr);
};
// LampConfigStore unchanged.
// Lamps.h
#define LAMPS_MAX_DEVICES 60
#define LAMPS_MAX_LAMPS   2048
// LampController gains:
//   uint8_t busDeviceCount(uint8_t bus) const;   devices built on that bus
//   bool busSx1503Faulted(uint8_t bus) const;    false when none fitted
//   bool busAl5887Faulted(uint8_t bus, uint8_t n) const;
```

- [ ] **Step 1: Read the current model**

Read `LampConfig.h/.cpp`, `Lamps.h/.cpp`, `LampWebApi.cpp` and HANDOFF.md section 5.1 in full. Note that `Lamps.cpp` holds the static `Wire`, `Wire1`, `PIOWire[7]` and `BitBangWire[3]` instances and a `busFor(index)` style lookup; keep that pin map exactly.

- [ ] **Step 2: Rewrite `LampConfig`**

Replace `hardware`/`devices`/`maxDevices()`/`lampsPerDevice()`/`hardwareName()`/`parseHardware()` with the interface above. JSON form:

```json
{"busSpeed":400000,"activeLow":false,"rgb":false,
 "buses":[{"sx1503":true,"al5887":0}, ...]}
```

`toJson` always writes 12 entries. `fromJson`: `buses` may be absent (all empty) or have up to 12 entries; more than 12 is the error `"buses: at most 12 entries"`. Each entry: `sx1503` bool default false, `al5887` int 0..4 else error `"buses[n].al5887 must be 0..4"`. Keep the existing error-string style.

- [ ] **Step 3: Rewrite `Lamps::build()` and `Lamps::probe()`**

`build()`: for bus 0..11, if `cfg.buses[b].sx1503 || cfg.buses[b].al5887`, begin that bus at `cfg.busSpeed`, then `new SX1503LampDriver(bus)` if `sx1503` (apply `setActiveLow(cfg.activeLow)`), then for n in 0..al5887-1 `new AL5887LampDriver(bus, 0x30 + n)`. Push each into `_dev[]` and record `_devBus[i] = b`. A device whose `begin()` returns false still occupies its lamp range (as today) and is marked faulted. Return false if any device failed.

`probe()`: stop the running configuration, for each bus: begin bus, `beginTransmission(0x20); endTransmission() == 0` sets `sx1503`; then for a in 0x30..0x33 in order, break at the first address that does not ACK, counting the ones that do. End the bus afterwards. Restore the running configuration. Copy `busSpeed`, `activeLow`, `rgb` from the running config into the result.

Add `busDeviceCount`, `busSx1503Faulted`, `busAl5887Faulted` using `_devBus[]` and the device order (SX1503 first on a bus).

- [ ] **Step 4: Update `LampWebApi::statusJson`**

Emit exactly:

```json
{"lamps":144,"devices":9,"intensity":false,"resolutionBits":1,
 "buses":[{"sx1503":"ok","al5887":["ok","fault"]}, ... 12 ...],
 "faults":[false,false,true]}
```

`sx1503` is `"none"` when not configured, `"ok"` or `"fault"` otherwise. `al5887` is an array with one string per configured device. Update the header comment's example to match. Remove `hardware`.

- [ ] **Step 5: Fix the boot print in the sketch**

Replace the `LampConfig::hardwareName(...)` print with:

```cpp
Serial.printf("lamps: %u devices, %u lamps\n", Lamps.deviceCount(), Lamps.count());
```

- [ ] **Step 6: Delete `lamps.html`**

`rm miniWorld_LightingController/lamps.html`. The sketch's `/lamps.html` route goes away in Task 6; until then it simply 404s.

- [ ] **Step 7: Build**

Run: `make`
Expected: compiles with no warnings. Check `Global variables use` in the output stays under 40 kB; the per-lamp arrays grew by about 12 kB.

- [ ] **Step 8: Verify the JSON round trip mentally against the spec**

Read section 3.1 of the spec once more and confirm field names match character for character.

---

### Task 2: NetConfig

**Files:**
- Create: `miniWorld_LightingController/NetConfig.h`, `NetConfig.cpp`

**Interfaces:**
- Produces:

```cpp
#pragma once
#include <Arduino.h>

#define NET_SSID_LEN  33
#define NET_PASS_LEN  65
#define NET_HOST_LEN  25
#define NET_NTP_LEN   48
#define NET_TZ_LEN    48

struct NetConfig {
    char ssid[NET_SSID_LEN]     = "";
    char pass[NET_PASS_LEN]     = "";
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
```

- [ ] **Step 1: Copy the pattern from `LampConfig.cpp`**

Use the same LittleFS begin-once guard and `File` handling as `LampConfigStore::load/save/erase`.

- [ ] **Step 2: Write `NetConfig.cpp`**

`toJson` with `includeSecrets=false` writes `sta: {ssid}` only, plus `hasWifi` and `hasPassword` at top level. `fromJson` uses `JsonDocument` and `doc["sta"]["ssid"].is<const char*>()` style presence checks; `strlcpy` into the fixed buffers; reject strings that do not fit with error `"<field> too long"`.

- [ ] **Step 3: Build**

Run: `make`
Expected: no warnings. Nothing includes the file yet, but Arduino compiles every `.cpp` in the sketch folder.

---

### Task 3: Version and SystemWebApi

**Files:**
- Create: `miniWorld_LightingController/Version.h`
- Create: `miniWorld_LightingController/SystemWebApi.h`, `SystemWebApi.cpp`

**Interfaces:**
- Produces:

```cpp
// Version.h
#pragma once
#define MINIWORLD_VERSION "0.1.0"
#define MINIWORLD_BUILD   __DATE__ " " __TIME__

// SystemWebApi.h
class SystemWebApi {
public:
    static int handle(const String &method, const String &path,
                      const String &requestBody, String &body);
    static bool owns(const String &path) { return path.startsWith("/api/system"); }
    // Set by handle() when a reboot was requested; the server reboots
    // after the response has been sent.
    static bool rebootPending();
};
```

- [ ] **Step 1: Write `SystemWebApi.cpp`**

`GET /api/system/status`:

```json
{"firmware":"0.1.0","build":"Sep  6 2026 19:40:12","uptime":1234,
 "heap":180000,"time":"2026-09-06T19:40:12","timeValid":true}
```

`uptime` is `millis()/1000`. `heap` is `rp2040.getFreeHeap()`. `time` is `strftime("%Y-%m-%dT%H:%M:%S", localtime(&now))`; `timeValid` is `now > 1700000000`. `POST /api/system/reboot` sets a static flag and returns `200 {"ok":true}`. Other paths 404, other methods 405, same `error()` helper style as `SceneWebApi.cpp`.

- [ ] **Step 2: Build**

Run: `make`
Expected: no warnings.

---

### Task 4: DnsResponder

**Files:**
- Create: `miniWorld_LightingController/DnsResponder.h`, `DnsResponder.cpp`

**Interfaces:**
- Produces:

```cpp
#pragma once
#include <Arduino.h>
#include <WiFiEspAT.h>

class DnsResponder {
public:
    // Answer every query with this address. Returns false if the UDP
    // socket could not be opened.
    bool begin(IPAddress answer, uint16_t port = 53);
    void end();
    // Handles at most one packet per call.
    void tick();
    bool active() const { return _active; }
    uint32_t answered() const { return _answered; }
private:
    WiFiUDP _udp;
    IPAddress _ip;
    bool _active = false;
    uint32_t _answered = 0;
    uint8_t _buf[512];
};
```

- [ ] **Step 1: Write the packet handling**

In `tick()`: `int len = _udp.parsePacket(); if (len <= 0) return;` then `_udp.read(_buf, min(len, 512))`. Validate: `len >= 17`, QR bit clear (`_buf[2] & 0x80` is 0), QDCOUNT (`_buf[4..5]`) is 1. Walk the question name from offset 12: labels until a zero byte, each label length must keep the index below `len - 4`; after the name come 4 bytes (QTYPE, QCLASS). Let `qend` be the index after those 4 bytes.

Build the reply in place:

```cpp
_buf[2] = 0x81; _buf[3] = 0x80;         // response, RD, RA, no error
_buf[6] = 0; _buf[7] = 1;               // ANCOUNT 1
_buf[8] = _buf[9] = _buf[10] = _buf[11] = 0;  // NSCOUNT, ARCOUNT
uint8_t ans[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 60, 0, 4,
                  _ip[0], _ip[1], _ip[2], _ip[3] };
```

If `qend + 16 > 512` drop the packet. `memcpy(_buf + qend, ans, 16)`, then `_udp.beginPacket(_udp.remoteIP(), _udp.remotePort()); _udp.write(_buf, qend + 16); _udp.endPacket(); _answered++`.

`begin()`: `_udp.begin(port)` returns 1 on success. `end()`: `_udp.stop()`.

- [ ] **Step 2: Build**

Run: `make`
Expected: no warnings.

---

### Task 5: NetManager and NetWebApi

**Files:**
- Create: `miniWorld_LightingController/NetManager.h`, `NetManager.cpp`
- Create: `miniWorld_LightingController/NetWebApi.h`, `NetWebApi.cpp`

**Interfaces:**
- Consumes: `NetConfig`, `NetConfigStore` (Task 2), `DnsResponder` (Task 4), `Challenger2040WiFi` from `ChallengerWiFi.h` (the board variant), WiFiEspAT `WiFi`, `WiFiApData`.
- Produces:

```cpp
// NetManager.h
#pragma once
#include <Arduino.h>
#include <WiFiEspAT.h>
#include "NetConfig.h"
#include "DnsResponder.h"

enum class NetMode : uint8_t { NoModule, Connecting, Online, Portal };
enum class ConnectResult : uint8_t { Idle, Connecting, Ok, Failed };

class NetManager {
public:
    bool begin();                  // brings up the ESP, loads /net.json, starts the state machine
    void tick();

    NetMode mode() const { return _mode; }
    ConnectResult connectResult() const { return _result; }
    const NetConfig &config() const { return _cfg; }

    // Save and apply hostname, ntp, tz, password. ssid/pass in cfg are
    // ignored here; use connectTo() for those.
    bool applyConfig(const NetConfig &cfg);
    // Store credentials and start an attempt now, keeping the AP up if
    // it is up.
    void connectTo(const char *ssid, const char *pass);
    // Clear credentials, go to Portal.
    void forget();

    bool apActive() const { return _apUp; }
    IPAddress apIP() const { return IPAddress(192, 168, 4, 1); }
    const char *apSsid() const { return _apSsid; }
    IPAddress ip() const;          // station IP when Online, else 0.0.0.0
    int rssi() const;
    String ssid() const;
    bool timeValid() const { return _timeValid; }
    bool moduleOk() const { return _mode != NetMode::NoModule; }

    // Blocking scan, strongest first. Returns count written.
    int scan(WiFiApData *out, int max);

    static const char *modeName(NetMode m);
    static const char *resultName(ConnectResult r);

private:
    bool bringUpModule();
    void enterConnecting(bool keepAp);
    void enterOnline();
    void enterPortal();
    void startAp();
    void stopAp();
    void applyOnlineServices();    // hostname, mDNS, SNTP, TZ
    void pollTime();

    NetConfig _cfg;
    NetMode _mode = NetMode::NoModule;
    ConnectResult _result = ConnectResult::Idle;
    DnsResponder _dns;
    char _apSsid[20] = "miniWorld";
    bool _apUp = false;
    bool _moduleUp = false;
    bool _timeValid = false;
    bool _triedPersisted = false;
    uint32_t _stateSince = 0;      // millis() at last transition
    uint32_t _lostSince = 0;       // Online: when WL_CONNECTED was last seen
    uint32_t _apDropAt = 0;        // Online after onboarding: when to end the AP
    uint32_t _lastRetry = 0;
    uint32_t _lastTimePoll = 0;
    uint32_t _lastModuleTry = 0;
};

extern NetManager Net;

// NetWebApi.h
class NetWebApi {
public:
    static int handle(const String &method, const String &path,
                      const String &requestBody, String &body);
    static bool owns(const String &path) { return path.startsWith("/api/net"); }
};
```

- [ ] **Step 1: Read the two libraries you depend on**

`~/.arduino15/packages/rp2040/hardware/rp2040/5.5.1/variants/challenger_nb_2040_wifi/ChallengerWiFi.h` and `.cpp` (reset, `changeBaudRate`, `isAlive`), and `~/Data/Dropbox/Arduino/libraries/WiFiEspAT/src/WiFi.h` (`init`, `begin`, `status`, `beginAP`, `configureAP`, `endAP`, `apMacAddress`, `setHostname`, `startMDNS`, `sntp`, `getTime`, `scanNetworks(WiFiApData*, uint8_t)`, `disconnect`, `setPersistent`). Also `examples/Tools/SetupPersistentWiFiConnection` in the library for how persisted credentials work.

- [ ] **Step 2: Module bring-up**

```cpp
bool NetManager::bringUpModule() {
    Serial2.begin(DEFAULT_ESP_BAUDRATE);
    if (!Challenger2040WiFi.reset()) return false;      // hardware reset, waits for "ready"
    if (!Challenger2040WiFi.changeBaudRate(921600)) return false;   // AT+UART_CUR, re-begins Serial2
    WiFi.init(Serial2);
    if (WiFi.status() == WL_NO_MODULE) return false;
    WiFi.setPersistent(false);   // we manage credentials ourselves
    uint8_t mac[6];
    WiFi.apMacAddress(mac);
    snprintf(_apSsid, sizeof(_apSsid), "miniWorld-%02X%02X", mac[4], mac[5]);
    return true;
}
```

If `changeBaudRate` fails, fall back to `Serial2.begin(115200)` and continue at the slow rate rather than giving up; log `net: esp stays at 115200`.

- [ ] **Step 3: State machine**

`begin()`: `NetConfigStore::load(_cfg)` (ignore failure), `bringUpModule()`; on failure `_mode = NoModule`, `_lastModuleTry = millis()`, return false. Otherwise: if `_cfg.hasWifi()` `enterConnecting(false)` else `enterPortal()` after first trying the persisted network: call `WiFi.begin()` with no arguments, set `_triedPersisted = true`, and treat it as Connecting with a 20 s timeout. Log every transition as `net: <state> ...`.

`tick()` per state:
- NoModule: every 10 s retry `bringUpModule()`; on success proceed as in `begin()`.
- Connecting: `WiFi.status() == WL_CONNECTED` within 20 s of `_stateSince`: `enterOnline()`. Timeout: `_result = Failed` if an attempt was in progress from `connectTo`, then `enterPortal()`. Do not call `WiFi.begin()` again while waiting.
- Online: `pollTime()` every hour (and every 10 s until `_timeValid`). If `WiFi.status() != WL_CONNECTED`, start or continue `_lostSince`; after 2 minutes `enterPortal()`. If `_apUp` and `millis() > _apDropAt`, `stopAp()`. The `_dns` must be stopped when the AP stops.
- Portal: `_dns.tick()`. If `_cfg.hasWifi()` and 5 minutes since `_lastRetry`, `enterConnecting(true)` (keeps the AP up).

`enterConnecting(keepAp)`: `_mode = Connecting; _stateSince = millis(); _lastRetry = _stateSince;` if `!keepAp && _apUp` `stopAp()`. `WiFi.begin(_cfg.ssid, _cfg.pass)` (or no-arg for the persisted try). `WiFi.begin` in WiFiEspAT blocks until the AT firmware reports a result or times out; that is acceptable here.

`enterOnline()`: `_mode = Online; _result = (_result == Connecting) ? Ok : Idle; _lostSince = 0;` `applyOnlineServices()`; if `_apUp` `_apDropAt = millis() + 60000`. Print `net: online <ssid> <ip>`. If the credentials came from the persisted try, copy `WiFi.SSID()` into `_cfg.ssid` for display but leave `pass` empty and do not save.

`enterPortal()`: `_mode = Portal; _stateSince = millis();` `startAp()` if not up. Print `net: portal <apSsid> 192.168.4.1`.

`startAp()`: `WiFi.configureAP(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0))`, `WiFi.beginAP(_apSsid, nullptr, 1, ENC_TYPE_NONE)`, `_dns.begin(apIP())`, `_apUp = true`. `stopAp()`: `_dns.end(); WiFi.endAP(); _apUp = false`.

`applyOnlineServices()`: `WiFi.setHostname(_cfg.hostname)`, `WiFi.startMDNS(_cfg.hostname, "http", 80)`, `WiFi.sntp(_cfg.ntp)`, `setenv("TZ", _cfg.tz, 1); tzset();`.

`pollTime()`: `unsigned long t = WiFi.getTime(); if (t > 1700000000) { struct timeval tv = { (time_t)t, 0 }; settimeofday(&tv, nullptr); _timeValid = true; }`.

`connectTo(ssid, pass)`: `strlcpy` into `_cfg`, `NetConfigStore::save(_cfg)`, `_result = Connecting`, `enterConnecting(true)`.

`forget()`: clear `ssid`/`pass`, save, `_result = Idle`, `enterPortal()`.

`applyConfig(cfg)`: copy hostname, password, ntp, tz into `_cfg`, `clamp()`, save; if Online, `applyOnlineServices()`.

`scan()`: `int n = WiFi.scanNetworks(out, max);` sort by `rssi` descending with a simple insertion sort; return `n < 0 ? 0 : n`.

- [ ] **Step 4: Write `NetWebApi.cpp`**

Routes and bodies exactly as spec section 3.6. `status` object fields: `mode` (`NetManager::modeName`, lower-case: `nomodule`, `connecting`, `online`, `portal`), `ssid`, `ip` (as string, `"0.0.0.0"` when none), `rssi`, `hostname`, `apSsid`, `apIp`, `apActive`, `connectResult` (`idle`, `connecting`, `ok`, `failed`), `auth` (`config().hasPassword()`), `timeValid`. `scan`: `WiFiApData list[12]`, `secure` is `enc != ENC_TYPE_NONE`. `config` GET: `toJson(body, false)`. `config` PUT: parse into a copy of the current config with `fromJson`, `Net.applyConfig(copy)`, return the config. `connect` POST: require `ssid` string, `pass` may be empty; `Net.connectTo`; return 202 with the status object. `forget` POST: `Net.forget()`, return status.

- [ ] **Step 5: Build**

Run: `make`
Expected: no warnings. Still nothing calls `Net`; that comes in Task 6.

---

### Task 6: HttpServer, sketch wiring, web build target

**Files:**
- Create: `miniWorld_LightingController/HttpServer.h`, `HttpServer.cpp`
- Modify: `miniWorld_LightingController/miniWorld_LightingController.ino`
- Modify: `Makefile`
- Requires: `tools/buildweb.py` and `web/index.html` from Task 7 (a placeholder page is enough to compile; Task 7 runs in parallel and lands first)

**Interfaces:**
- Consumes: `LampWebApi`, `SceneWebApi`, `NetWebApi`, `SystemWebApi`, `Net`, `WEBUI_GZ`, `WEBUI_GZ_LEN`, `WEBUI_ETAG` from `WebUI.gen.h`.
- Produces:

```cpp
#pragma once
#include <Arduino.h>
#include <WiFiEspAT.h>

class HttpServer {
public:
    void begin(uint16_t port = 80);
    // Serves at most one request per call.
    void tick();
private:
    struct Request {
        String method, path, host, auth, ifNoneMatch, body;
        size_t contentLength = 0;
        bool tooLarge = false, headerTooLong = false;
    };
    bool readRequest(WiFiClient &c, Request &r);
    bool readLine(WiFiClient &c, String &line, size_t maxLen);
    bool authorised(const Request &r);
    bool isCaptiveProbe(const String &path);
    void sendStatus(WiFiClient &c, int code, const char *type, const String &body, const char *extra = nullptr);
    void sendRedirect(WiFiClient &c, const char *location);
    void sendUi(WiFiClient &c, const Request &r);
    WiFiServer _server{80};
};

extern HttpServer Http;
```

- [ ] **Step 1: Write request reading**

`readLine`: read bytes until `\n` with a 2 s deadline (`millis()` based, `yield()` while waiting), strip `\r`, return false on timeout; if the line exceeds `maxLen` set the flag and keep discarding to end of line. Request line `maxLen` 1024, header lines 512. Split the request line on spaces; strip a `?query` from the path. Loop headers until an empty line, matching case-insensitively on `Content-Length:`, `Authorization:`, `Host:`, `If-None-Match:`. If `contentLength > 8192` set `tooLarge` and do not read the body. Otherwise read exactly `contentLength` bytes with the same deadline.

- [ ] **Step 2: Write auth**

Base64 decode of the token after `Basic `; compare the part after the first `:` with `Net.config().password` using `strcmp`. When no password is set, always authorised. Decode table: a 64-char string and a loop over 4-char groups; ignore `=` padding.

- [ ] **Step 3: Write routing in `tick()`**

```
WiFiClient c = _server.accept(); if (!c) return;
Request r; if (!readRequest(c, r)) { c.stop(); return; }
if (r.headerTooLong) 431; else if (r.tooLarge) 413;
else if (Net.mode() == NetMode::Portal && r.host != "192.168.4.1" && r.host.length())
     sendRedirect(c, "http://192.168.4.1/");
else if (Net.config().hasPassword() && !authorised(r))
     sendStatus(c, 401, "application/json", "{\"error\":\"unauthorised\"}",
                "WWW-Authenticate: Basic realm=\"miniWorld\"\r\n");
else if (LampWebApi::owns(r.path)) ...handle, send JSON...
else if (SceneWebApi::owns ...) / NetWebApi / SystemWebApi
else if (r.path == "/" || r.path == "/index.html") sendUi(c, r);
else if (isCaptiveProbe(r.path)) sendRedirect(c, "http://192.168.4.1/");
else 404 JSON.
c.stop();
if (SystemWebApi::rebootPending()) { delay(200); rp2040.reboot(); }
```

`sendStatus` writes `HTTP/1.1 <code> <reason>\r\nContent-Type: ..\r\nContent-Length: ..\r\nConnection: close\r\n<extra>\r\n` then the body. Reason strings: 200 OK, 202 Accepted, 302 Found, 304 Not Modified, 400 Bad Request, 401 Unauthorized, 404 Not Found, 405 Method Not Allowed, 413 Payload Too Large, 431 Request Header Fields Too Large, 500 Internal Server Error.

`sendUi`: if `r.ifNoneMatch == WEBUI_ETAG` send 304 with `ETag` and no body. Else headers `Content-Type: text/html; charset=utf-8`, `Content-Encoding: gzip`, `ETag: <WEBUI_ETAG>`, `Cache-Control: no-cache`, `Content-Length: WEBUI_GZ_LEN`, then write `WEBUI_GZ` in 1024-byte chunks with `c.write(ptr, n)` checking the return value and stopping on 0.

Captive probe paths: `/generate_204`, `/gen_204`, `/hotspot-detect.html`, `/library/test/success.html`, `/connecttest.txt`, `/ncsi.txt`, `/canonical.html`, `/success.txt`, `/redirect`.

- [ ] **Step 4: Rewrite the sketch**

```cpp
#include <WiFiEspAT.h>
#include <LittleFS.h>
#include "Version.h"
#include "Lamps.h"
#include "SceneEngine.h"
#include "NetManager.h"
#include "HttpServer.h"

void setup() {
    Serial.begin(115200);
    delay(1500);                    // give USB CDC a moment so the boot log is visible
    Serial.printf("miniWorld lighting controller %s (%s)\n", MINIWORLD_VERSION, MINIWORLD_BUILD);
    Lamps.begin();
    Serial.printf("lamps: %u devices, %u lamps\n", Lamps.deviceCount(), Lamps.count());
    if (!Scene.begin()) { ...seed the example town exactly as today... }
    Net.begin();
    Http.begin(80);
}

void loop() {
    Net.tick();
    Http.tick();
    Scene.tick();
}
```

Keep the header comment, updated: the adaptor is gone, the server is `HttpServer`. Remove `setenv("TZ")` from the sketch; `NetManager` owns it.

- [ ] **Step 5: Makefile**

Add:

```make
WEB_SRC := $(wildcard web/*.html web/*.css web/*.js)
WEB_HDR := $(SKETCH)/WebUI.gen.h

web: $(WEB_HDR)

$(WEB_HDR): $(WEB_SRC) tools/buildweb.py
	python3 tools/buildweb.py web $(WEB_HDR)

mock:
	python3 tools/mockserver.py

check:
	tools/apicheck.sh http://localhost:8080
```

Make `compile` and `upload` depend on `pio web`. Add `web mock check` to `.PHONY`. `clean` also removes `$(WEB_HDR)`. Add `WebUI.gen.h` to `.gitignore`.

- [ ] **Step 6: Build**

Run: `make`
Expected: `tools/buildweb.py` runs first and prints the gzipped size, then arduino-cli compiles with no warnings.

---

### Task 7: Web build tool, mock server, API check, page shell

**Files:**
- Create: `tools/buildweb.py`, `tools/mockserver.py`, `tools/apicheck.sh`
- Create: `web/index.html`, `web/app.css`, `web/app.js`

**Interfaces:**
- Produces, for the view tasks:

```js
// app.js globals
const App = {
  register(name, view),        // view: { title, mount(root), unmount(), poll() }
  go(name),                    // navigate, updates location.hash
  toast(message, kind = 'info'),   // kind: 'info' | 'ok' | 'error'
  api(method, path, body),     // Promise<json>; throws Error(message) on non-2xx; reloads on 401
  el(tag, attrs, ...children), // tiny DOM builder: attrs object, children strings or nodes
  fmtTime(minutes),            // 1182 -> "19:42"
  parseRanges(text, max),      // "0-15, 20" -> [0..15, 20], throws on nonsense
  rangesText(list),            // [0,1,2,20] -> "0-2, 20"
  lampDisc(lit),               // <span class="lamp on|off">
};
```

`index.html` has `<header>` with the four nav links (`#home`, `#wifi`, `#lamps`, `#scene`) and a `no contact` badge, `<main id="view">` and a `<div id="toasts">`. Each view renders into `#view` on `mount(root)`.

- [ ] **Step 1: `tools/buildweb.py`**

```
usage: buildweb.py <webdir> <out.h>
```

Read `index.html`. Replace the literal line `<!-- CSS -->` with `<style>` + `app.css` + sorted `view-*.css` + `</style>`, and `<!-- JS -->` with `<script>` + `app.js` + sorted `view-*.js` + `</script>`. Strip `/* ... */` comments from CSS, `// ...` full-line comments from JS (only lines whose first non-space chars are `//`), `<!-- -->` from HTML, and leading whitespace on every line. Gzip level 9 (`gzip.compress(data, 9, mtime=0)` so the output is reproducible). ETag: first 8 hex chars of `hashlib.sha1(gz).hexdigest()`. Write the header exactly as spec 4.1 with 16 bytes per line. Print `webui: <raw> bytes raw, <gz> bytes gzipped`. Exit 1 with a message if gz > 49152.

- [ ] **Step 2: `tools/mockserver.py`**

`ThreadingHTTPServer` on `0.0.0.0:8080`, `--portal` flag, `--password <p>` flag. Static: `/` serves `web/index.html` with the two markers expanded inline the same way as buildweb (import the function from `buildweb.py` via `sys.path`), so the browser sees one page just like on the device; no gzip. State: `lamps` config (12 buses, bus 0 sx1503 true, bus 1 al5887 2 by default), `scene` config (the seeded town from the sketch: Street 0-15, Home 16-95, Shop 96-119, Late 120-127, AllNight 128-143), `net` config, a simulated clock. Routes: every route in spec 3.6 plus `/api/lamps/*` and `/api/scene/*` with the shapes in `LampWebApi.h` (Task 1 form) and `SceneWebApi.h`. Scene status: simulated minutes advance in real time in `real` mode, `1440 / dayMinutes` per real minute in `accelerated`, fixed in `manual`; dusk `19:58` and dawn `05:47` shifted by `sin` of day of year by up to 2 hours; `lit` is a plausible function of time (0 in the day, rising after dusk). Presets: the five behaviours with the field names in `SceneWebApi::getPresets`. Net: `scan` returns four fixed networks; `connect` sets `connectResult` to `connecting`, then `ok` after 4 s (or `failed` when the password is `wrong`), mode to `online` with ip `192.168.1.42`. Basic auth enforced on everything when `--password` is set. Unknown paths 404 JSON. Log one line per request.

- [ ] **Step 3: `tools/apicheck.sh`**

`#!/bin/sh`, `BASE=${1:-http://localhost:8080}`, optional `AUTH` env var as `user:pass`. One `curl -s -o /dev/null -w "%{http_code} $m $p\n"` per route, expecting the listed code, and exit 1 at the end if any differed: GET `/` 200, GET `/api/lamps/config` 200, GET `/api/lamps/status` 200, POST `/api/lamps/probe` 200, POST `/api/lamps/test` `{"level":128}` 200, GET `/api/scene/config` 200, GET `/api/scene/status` 200, PUT `/api/scene/clock` `{"mode":"manual","time":"19:40"}` 200, GET `/api/scene/presets` 200, GET `/api/net/status` 200, GET `/api/net/scan` 200, GET `/api/net/config` 200, PUT `/api/net/config` `{"ntp":"pool.ntp.org"}` 200, GET `/api/system/status` 200, GET `/nope` 404, GET `/api/net/connect` 405. Make it executable.

- [ ] **Step 4: `web/index.html`, `web/app.css`, `web/app.js`**

`index.html` with `<!DOCTYPE html>`, `<meta name="viewport" content="width=device-width, initial-scale=1">`, `<meta name="color-scheme" content="dark">`, `<title>miniWorld</title>`, the two markers `<!-- CSS -->` in `<head>` and `<!-- JS -->` at the end of `<body>`. Header: a small amber lamp glyph drawn with CSS, the word `miniWorld`, the nav, the badge `<span id="offline" hidden>no contact</span>`.

`app.css`: the tokens from spec 4.3 as CSS variables on `:root`, `body` background `var(--bg)`, reset, header sticky, nav links as 44 px targets with the active one underlined in accent, `.card`, `.row`, `.btn` (primary amber on dark text, secondary outlined), `.field` with label above input, `.switch` (checkbox styled as a toggle with a visible focus ring), `.lamp` disc on/off with glow, `.toast` stack bottom-centre, `.badge`, `@media (min-width: 720px)` two-column `.cols`, `@media (prefers-reduced-motion: reduce)` disables transitions. Text contrast: check `#d7dfe6` on `#0b0f14` and `#8a98a6` on `#141b23` both exceed 4.5:1 (they do: about 14:1 and 5.3:1).

`app.js`: the `App` object above. Router: on `hashchange` and load, `unmount()` the current view, clear `#view`, `mount()` the new one, start a 2 s `setInterval` calling `poll()` (the first call immediately), mark the active nav link. `api()`: `fetch(path, {method, headers: {'Content-Type': 'application/json'}, body: body && JSON.stringify(body)})`; on 401 `location.reload()`; on other non-ok parse `{error}` and throw; on network failure show the offline badge and rethrow; on success hide the badge. `toast()`: appends a `div.toast.<kind>` and removes it after 3 s.

- [ ] **Step 5: Run the mock and the check**

Run: `python3 tools/mockserver.py &` then `tools/apicheck.sh http://localhost:8080`
Expected: every line shows the expected code, exit 0. Open `http://localhost:8080/#home` in a browser: header renders, the view area shows "no view registered for home" from the router, no console errors.

Run: `python3 tools/buildweb.py web /tmp/claude-1000/WebUI.test.h` (scratchpad path in practice)
Expected: prints sizes and writes a header with `WEBUI_ETAG`, `WEBUI_GZ_LEN`, `WEBUI_GZ`.

---

### Task 8: Home view

**Files:**
- Create: `web/view-home.js`, `web/view-home.css`

**Interfaces:**
- Consumes: `App.*` from Task 7; `GET /api/scene/status`, `GET /api/lamps/status`, `GET /api/net/status`, `PUT /api/scene/clock {"enabled":bool}`.

- [ ] **Step 1: Write the view**

`App.register('home', {...})`. `mount(root)` builds: a `.card.clock` with the town time in a display size (`.town-time`, tabular nums, 3.5 rem on phone), a line `mode · dusk 19:58 · dawn 05:47`, a line `61 of 144 lamps lit` with a row of lamp discs summarising groups is not available here, so show a single large disc that is lit when `lit > 0`. A `.card.buses` with twelve tiles (`bus 0`..`bus 11`), each showing `SX` and `AL×n` marks and a fault glyph `!` with the text `fault` when any device on it is `fault`; empty buses are dimmed. A `.card.net` with mode, SSID or AP name, IP, and `time not set` when `timeValid` is false. A `.switch` labelled `Scene running` bound to `enabled`; change sends the clock PUT and toasts on error.

`poll()` fetches the three status objects with `Promise.all` and updates the DOM in place (no re-render, so the switch does not lose focus).

- [ ] **Step 2: Style**

`view-home.css`: `.town-time`, `.buses` grid `repeat(4, 1fr)` on phone and `repeat(6, 1fr)` from 720 px, tile states.

- [ ] **Step 3: Check in the browser**

Run the mock, open `#home`. Expected: numbers update every 2 s, the switch toggles and the mock's status reflects it, tiles for buses 0 and 1 are lit, others dim. Resize to 375 px wide: no horizontal scroll.

---

### Task 9: WiFi view and portal landing

**Files:**
- Create: `web/view-wifi.js`, `web/view-wifi.css`

**Interfaces:**
- Consumes: `App.*`; `/api/net/status`, `/api/net/scan`, `/api/net/config` GET and PUT, `/api/net/connect`, `/api/net/forget`.

- [ ] **Step 1: State card**

At the top: in `portal` mode a card reading `Not on a WiFi network` with the AP name and `Choose your network below`; in `online` mode `Connected to <ssid>` with the address and `http://<hostname>.local/`; in `connecting` a spinner line. `connectResult` drives a result line: `connecting` shows progress dots, `ok` shows `Connected. Address 192.168.1.42. Open http://miniworld.local/ from your home network. This access point switches off shortly.`, `failed` shows `Could not join <ssid>. Check the password and try again.`

- [ ] **Step 2: Network list and connect form**

A `Scan` button (disabled while scanning) fetches `/api/net/scan` and renders rows: SSID, four-bar signal glyph from RSSI (`> -55` four, `> -65` three, `> -75` two, else one), a lock glyph when `secure`. Tapping a row selects it (aria-pressed) and fills a read-only SSID field; a free-text SSID field is also available under `Other network`. Password field with a `show` toggle. `Connect` posts `/api/net/connect`, then `poll()` watches `connectResult`. Scan automatically on mount when in portal mode.

- [ ] **Step 3: Settings card and forget**

Fields: hostname, device password (with `clear` option that sends `""`), NTP server, timezone `<select>` with `CET-1CEST,M3.5.0,M10.5.0/3` (Sweden, central Europe) first, `UTC0`, `WET0WEST,M3.5.0/1,M10.5.0`, `EET-2EEST,M3.5.0/3,M10.5.0/4`, `GMT0BST,M3.5.0/1,M10.5.0`, and `Custom` which reveals a text field. `Save` sends only the fields that changed. `Forget WiFi` button asks `Forget the stored network and start the access point?` with a second `Yes, forget` button before posting.

- [ ] **Step 4: Style and check**

`view-wifi.css`: `.net-row` 48 px tall, selected state with accent border, signal bars as four spans of increasing height. Run the mock with `--portal`, open `#wifi`: scan fills, select, enter any password, Connect, the result goes to `ok` after 4 s with the address text. Enter password `wrong`: `failed` text. Without `--portal`: the connected card shows. Save settings: mock echoes them.

---

### Task 10: Lamps view

**Files:**
- Create: `web/view-lamps.js`, `web/view-lamps.css`

**Interfaces:**
- Consumes: `App.*`; `/api/lamps/config` GET and PUT, `/api/lamps/status`, `/api/lamps/probe`, `/api/lamps/test`. Config shape from Task 1.

- [ ] **Step 1: Bus table**

Twelve rows, each: `Bus n` and its GPIO pair from a fixed table in the view (`0: GPIO 0/1`, `1: 26/27`, `2: 2/3`, `3: 6/7`, `4: 8/9`, `5: 14/15`, `6: 22/23`, `7: 24/25`, `8: 16/17`, `9: 10/18`, `10: 20/21`, `11: 28/29`), an SX1503 `.switch`, an AL5887 stepper (`−` count `+`, 0..4, 44 px buttons), and status glyphs from `/api/lamps/status`: `ok`, `fault` or nothing per device. Global card: bus speed select (100 kHz, 400 kHz), `activeLow` switch, `rgb` switch. A `Save` button posts the form as config; a `changed` marker appears when the form differs from the last loaded config.

- [ ] **Step 2: Probe**

`Probe hardware` posts `/api/lamps/probe` (toast `Probing, lamps will blink briefly`), then renders a diff card: per bus where found differs from the form, a line `Bus 3: found SX1503 + 2 × AL5887, configured 1 × AL5887`; if nothing differs, `Matches the configuration`. `Use what was found` copies the probe result into the form.

- [ ] **Step 3: Test card**

A range input `All lamps` 0..255 sending `POST /api/lamps/test {"level"}` throttled to one request per 150 ms, and a lamp number input with `Set` sending `{"lamp","level"}`.

- [ ] **Step 4: Style and check**

`view-lamps.css`: rows as a grid `auto 1fr auto auto`, stepper buttons. Against the mock: toggle bus 4 SX1503, count appears in `changed`, Save, reload, state persists in the mock. Probe shows the diff (mock's probe returns bus 0 sx1503 and bus 2 al5887 1). Test slider moves without flooding (watch the mock log).

---

### Task 11: Scene view

**Files:**
- Create: `web/view-scene.js`, `web/view-scene.css`

**Interfaces:**
- Consumes: `App.*`; `/api/scene/config` GET and PUT, `/api/scene/status`, `/api/scene/clock`, `/api/scene/presets`. Config shape in `Scene.h`; presets shape in `SceneWebApi::getPresets`.

- [ ] **Step 1: Clock card with scrubber**

Segmented control `Real · Accelerated · Manual` sending `PUT /api/scene/clock {"mode"}`. The scrubber: a `div.sky` 56 px tall with a `linear-gradient` whose stops are computed from status: day colour until `sunset - 60` min, dusk colour at `dusk`, night from `dusk + 60` to `dawn - 60`, dusk colour again at `dawn`, day from `sunrise + 60`; positions as percentages of 1440. Tick marks and labels for dawn and dusk under the strip. A sun thumb (`div.sun`) positioned at `minutes / 1440`. Over it a transparent `input[type=range]` 0..1439, height matching the strip, `aria-label="Time of day"`. In manual mode dragging sends `{"mode":"manual","time":"HH:MM"}` throttled to 150 ms and moves the thumb immediately; in other modes the input is disabled and the thumb follows status. Below: day of year number input with a readout of the date (`day 249 = 6 Sep`), `dayMinutes` input shown only in accelerated mode, dusk and dawn text. `persist: true` is added when a `Keep` checkbox is on.

- [ ] **Step 2: Groups editor**

Loads config and presets on mount. Group list rows: name, behaviour, `n lamps`; tap to expand. Expanded form: name (max 23 chars), behaviour select, lamps text field bound with `App.parseRanges`/`App.rangesText` and a red validation message on bad input, then the override fields `onAnchor`, `on[0..1]`, `offAnchor`, `off[0..1]`, `litPercent`, `flickerPercent`, `morning`, `level`, `fadeMs`, prefilled from the group and re-seeded from presets when the behaviour changes (with a toast `Preset applied`). `Delete group` with a confirm step. `Add group` appends `{name: "New group", behaviour: "home", lamps: []}` seeded from presets. `Save scene` sends the whole config with `PUT /api/scene/config`; a `changed` marker tracks edits. Lamp discs: a row of up to 48 discs per group showing membership (lit accent for member lamps, capped with `+n more`).

- [ ] **Step 3: Location card**

Latitude, longitude, seed. Part of the same config and the same Save.

- [ ] **Step 4: Style and check**

`view-scene.css`: `.sky`, `.sun` (16 px amber disc with glow), `.ticks`, segmented control, group rows, `.discs` wrapping. Against the mock: switch to manual, drag the scrubber, status time follows; switch back to real, the thumb moves on its own. Edit a group's lamps to `0-3, 9`, Save, reload, it persists. Bad input `abc` shows the validation message and disables Save.

---

### Task 12: Docs, review, size

**Files:**
- Modify: `HANDOFF.md` and `miniWorld_LightingController/HANDOFF.md` (keep identical)
- Modify: `CLAUDE.md`
- Modify: `.gitignore`

- [ ] **Step 1: Run the web-design-guidelines review**

Invoke the `web-design-guidelines` skill on `web/`. Fix what it finds that is real (focus states, labels, contrast, target sizes). Note anything skipped and why in the final report.

- [ ] **Step 2: Size and build**

Run: `make`
Expected: `webui:` line shows under 40960 bytes gzipped; compile warning-free. If over budget, remove comments or duplicate CSS first; do not add a minifier dependency.

- [ ] **Step 3: HANDOFF.md**

Section 2 layering: add `NetManager / DnsResponder / HttpServer / NetWebApi / SystemWebApi` as a new band between the application and the scene band; the application line becomes `miniWorld_LightingController.ino   application: Net.tick, Http.tick, Scene.tick`. Section 3 inventory: add rows for the new files, `web/`, `tools/`; mark 5.1 done; replace the `lamps.html` row with `web/` (`SPA, four views, built into WebUI.gen.h`). Section 5.1: mark done with the date. Add section 5.2 as done (the scrubber lives in the Scene view) and add a new open item `Portal bring-up on a phone` referencing spec section 6. Section 6 unverified: add `AT firmware DHCP offers the AP as DNS` and `921600 baud on the ESP link`. Section 7 conventions: add `Web UI: sources in web/, one view per file, built by make, never edit WebUI.gen.h`.

- [ ] **Step 4: CLAUDE.md**

Building section: add `make web`, `make mock`, `make check`, and the rule that `WebUI.gen.h` is generated. Architecture section: add a bullet for the network band and one for the UI packaging. Traps: `Serial2 is the ESP link`, `WiFi.begin() with no arguments joins the AT firmware's persisted network`.

- [ ] **Step 5: `.gitignore`**

Contains `*.pio.h` and `WebUI.gen.h`.

---

### Task 13: Flash and bench

**Files:** none.

- [ ] **Step 1: Find the board**

Run: `for d in /dev/ttyACM*; do echo -n "$d: "; udevadm info -q property $d | grep -E 'ID_MODEL=|ID_SERIAL='; done`
Expected: one line naming a Challenger NB 2040 WiFi (or an `RPI-RP2` mass-storage device if it sits in BOOTSEL). If it is not there, stop and report.

- [ ] **Step 2: Flash**

Run: `make upload PORT=/dev/ttyACMn`
Expected: arduino-cli resets the board through the 1200 baud touch and uploads with picotool. On failure, hold BOOT while pressing RESET and retry with the `RPI-RP2` path.

- [ ] **Step 3: Read the boot log**

Run: `timeout 40 cat /dev/ttyACMn` after `stty -F /dev/ttyACMn 115200 raw -echo`
Expected: the version line, `lamps:` line, `net:` transitions. Note the final one: `net: online <ssid> <ip>` or `net: portal miniWorld-XXXX 192.168.4.1`, or `net: no module`.

- [ ] **Step 4: Reach it**

If online: `tools/apicheck.sh http://<ip>` must pass, and `curl -s http://<ip>/api/system/status`. If portal: report the AP name and 192.168.4.1; the user onboards from a phone. Record the outcome in the final report with the address.
