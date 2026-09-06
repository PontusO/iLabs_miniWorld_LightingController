# Web GUI and captive portal: design

Date: 2026-09-06. Owner: Pontus, Invector Embedded Systems AB.
Status: approved in conversation, implementation follows this document.

## 1. Goal

Give the miniWorld lighting controller a product-quality web interface
and WiFi onboarding, served by the Challenger NB RP2040 WiFi itself over
its ESP8285 AT co-processor (AT firmware 2.x, WiFiEspAT library).

In scope, first release:

- Captive portal onboarding: the device raises its own access point when
  it has no usable WiFi, phones get the sign-in sheet, the user picks a
  network and enters a password, the device joins it and reports its
  address.
- AP fallback with the full GUI, for layouts with no router.
- A single-page GUI with four views: Home, WiFi, Lamps, Scene.
- HTTP basic auth with one device password.
- SNTP and mDNS through the AT firmware.
- Off-target development of the UI against a Python mock of the API.
- The per-bus lamp hardware model from HANDOFF.md section 5.1, because the
  Lamps view is built against it.

Out of scope: HTTPS, OTA firmware update, named scenes, drag-to-assign
lamp editing, user accounts, any framework or external asset.

## 2. Constraints that shape everything

- WiFi is only reachable through WiFiEspAT over Serial2. The link is a
  UART; every byte of every page crosses it. The ESP link is raised from
  115200 to 921600 at boot through the variant helper
  `Challenger2040WiFi.changeBaudRate()`.
- WiFiEspAT serves a small number of TCP clients in turn. Browsers open
  several connections for separate assets, so the UI is one file.
- House rules (HANDOFF.md section 7): no em dashes, no web fonts, no
  CDN, no framework, everything hardware is runtime config, web API
  handlers are server-agnostic `(method, path, body) -> (code, json)`.
- Debug output is USB CDC `Serial`. GPIO16/17 are an I2C bus, so
  `Serial1` is never started. Serial2 is the ESP link.
- RAM: RP2040 has 264 kB. The per-bus rework raises `LAMPS_MAX_LAMPS` to
  2048 (about 12 kB more). The HTTP server keeps one request body of at
  most 8 kB.

## 3. Device side

### 3.1 Per-bus lamp hardware (HANDOFF section 5.1, prerequisite)

`LampConfig` becomes:

```json
{
  "busSpeed": 400000,
  "activeLow": false,
  "rgb": false,
  "buses": [ { "sx1503": true, "al5887": 0 }, ... 12 entries ... ]
}
```

- `LampConfig`: `struct BusConfig { bool sx1503; uint8_t al5887; }`,
  `BusConfig buses[LAMPS_NUM_BUSES]` with `LAMPS_NUM_BUSES` 12. Remove
  `hardware` and `devices`. `lampCount()` sums `16 * sx1503 + 36 * al5887`
  over buses. `clamp()` limits `al5887` to 0..4. JSON accepts fewer than
  12 entries (missing ones are empty) and rejects more.
- `Lamps::build()`: per bus, start the bus if anything is on it, create
  the SX1503 driver first, then AL5887 drivers at 0x30 + n in order. Lamp
  numbering is bus order, then that device order, then channel.
- `Lamps::probe()`: per bus, ping 0x20, then 0x30..0x33 stopping at the
  first non-responder. Returns a `LampConfig` with the global flags copied
  from the running config.
- `LAMPS_MAX_LAMPS` 2048, `LAMPS_MAX_DEVICES` 60. `GroupConfig::lampBits`
  and the engine's per-lamp arrays follow the constant automatically.
- `LampWebApi` status object:

```json
{
  "lamps": 144, "devices": 9,
  "intensity": false, "resolutionBits": 1,
  "buses": [ { "sx1503": "ok" | "fault" | "none", "al5887": ["ok", "fault"] }, ... ],
  "faults": [false, false, true, ...]
}
```

`faults` is per device in lamp order, as before. `buses` is the per-bus
summary the GUI renders. The `hardware` field is gone.

- `lamps.html` is deleted; the SPA replaces it.

### 3.2 Network configuration: `/net.json`

Owned by `NetConfig` (`NetConfig.h/.cpp`), same pattern as `LampConfig`:
struct, `toJson`, `fromJson`, `NetConfigStore::load/save/erase`.

```json
{
  "sta":      { "ssid": "", "pass": "" },
  "hostname": "miniworld",
  "password": "",
  "ntp":      "pool.ntp.org",
  "tz":       "CET-1CEST,M3.5.0,M10.5.0/3"
}
```

- `password` is the GUI password. Empty means no authentication.
- `hostname` is used for mDNS (`<hostname>.local`) and the DHCP host name.
  Lower-case letters, digits and hyphens, 1..24 characters; `clamp()`
  fixes anything else to `miniworld`.
- Passwords are stored in clear text in LittleFS. This is the norm for
  this class of device and is stated here so nobody is surprised.
- `toJson(out, includeSecrets)`: the store writes secrets, the API never
  returns them. The API form adds `"hasWifi": bool` and
  `"hasPassword": bool`.

### 3.3 `NetManager`

`NetManager.h/.cpp`, a global `Net`. Owns the ESP8285, WiFiEspAT, the
state machine, SNTP, mDNS and the DNS responder. The sketch calls
`Net.begin()` in `setup()` and `Net.tick()` in `loop()`.

States and transitions:

```
Boot ──────────────> Connecting        stored credentials exist
     └─────────────> Portal            none stored

Connecting ────────> Online            WL_CONNECTED within 20 s
           └───────> Portal            timeout or failure

Portal ────────────> Connecting        every 5 min if stored credentials
                                       exist; or immediately on
                                       /api/net/connect (keeps AP up
                                       during the attempt, see below)

Online ────────────> Portal            WL_CONNECTED lost for 2 min, or
                                       /api/net/forget
```

- Boot: `Serial2.begin(115200)`, `Challenger2040WiFi.reset()`,
  `Challenger2040WiFi.changeBaudRate(921600)`, `WiFi.init(Serial2)`. If
  the module does not answer (`WL_NO_MODULE`), retry the reset every 10 s
  and report `mode: "nomodule"` in status. Nothing else blocks on it.
- Boot with no stored credentials also tries `WiFi.begin()` with no
  arguments once, which asks the AT firmware to join whatever network it
  last persisted. If that works within 20 s the device is Online and
  `/net.json` is left as it is (the GUI shows the SSID from the module).
  This is what makes a previously used board come up on the bench.
- Connecting: `WiFi.begin(ssid, pass)`, poll `WiFi.status()`.
- Online: `WiFi.setHostname()`, `WiFi.startMDNS(hostname, "http", 80)`,
  `WiFi.sntp(ntp)`, `setenv("TZ", tz)` and `tzset()`. Time from
  `WiFi.getTime()` is pushed into the system clock with `settimeofday()`
  once it is non-zero, and refreshed hourly. The scene's real clock
  mode depends on this.
- Portal: `WiFi.beginAP(apSsid, nullptr, 1)` with no passphrase (open
  network), `WiFi.configureAP(192.168.4.1, 192.168.4.1, 255.255.255.0)`,
  start `DnsResponder`. AP SSID is `miniWorld-XXXX` where XXXX is the last
  two bytes of the AP MAC in upper-case hex, so two devices at one
  exhibition do not clash.
- Onboarding from the portal: `/api/net/connect` stores the credentials,
  then the manager tries them in AP+station mode so the phone stays
  associated and can poll the result. `connectResult` in status goes
  `connecting` then `ok` or `failed`. On `ok` the device is Online, the AP
  is kept for 60 s more so the portal page can show the new address, then
  `WiFi.endAP()`. On `failed` the device stays in Portal with the
  credentials kept for the 5 minute retry.
- Every state change is logged on `Serial` with the resulting IP, and
  the Online IP is printed as `net: online <ssid> <ip>` so it can be read
  from the USB console.

### 3.4 `DnsResponder`

`DnsResponder.h/.cpp`. A UDP listener on port 53, active only in Portal.
For every query it builds a response that copies the question section
and answers with a single A record pointing at the AP address, TTL 60,
regardless of the name. Queries other than type A or class IN get the
same answer; that is what makes the phones' probe resolution succeed.
Packets over 512 bytes or without a question are dropped. It reads one
packet per `tick()`.

The AT firmware's DHCP server is expected to offer the AP address as the
DNS server; ESP-IDF's DHCP server does so by default. This is the one
assumption in the portal that must be confirmed with a phone on the
bench (section 6).

### 3.5 `HttpServer`

`HttpServer.h/.cpp`. Replaces the adaptor in the sketch. A WiFiEspAT
`WiFiServer` on port 80, `begin(80, 3, timeout)` for three client slots.
`tick()` accepts at most one client per call and serves that request to
completion, so `Scene.tick()` between requests keeps the town moving.

Request parsing:

- Request line: method, path, query string split off and ignored except
  where a handler documents it.
- Headers read until the blank line: `Content-Length`, `Authorization`,
  `Host`, `If-None-Match`, `Accept-Encoding`. Others skipped. Header
  lines over 512 bytes cause 431.
- Body up to 8192 bytes read into a `String`; more causes 413 and the
  connection is closed.
- Read timeout 2 s per stage; a stalled client is dropped.

Routing, in order:

1. Captive-portal probes when in Portal mode and the `Host` header is not
   the AP address: `/generate_204`, `/gen_204`, `/hotspot-detect.html`,
   `/library/test/success.html`, `/connecttest.txt`, `/ncsi.txt`,
   `/canonical.html`, `/success.txt`, `/redirect`. Reply
   `302 Location: http://192.168.4.1/`. Any other path with a foreign
   `Host` in Portal mode gets the same redirect.
2. Auth check for everything below when a device password is set:
   `Authorization: Basic base64(user:password)`; the user part is
   ignored. Failure: `401`, `WWW-Authenticate: Basic realm="miniWorld"`,
   JSON error body.
3. `/api/lamps/*`, `/api/scene/*`, `/api/net/*`, `/api/system/*` to the
   handler whose `owns()` matches. Response `application/json`.
4. `/` and `/index.html`: the SPA from `WebUI.gen.h`. Headers:
   `Content-Type: text/html; charset=utf-8`, `Content-Encoding: gzip`,
   `ETag: "<hash>"`, `Cache-Control: no-cache`. If `If-None-Match`
   matches, `304` with no body. The bytes are written in 1 kB chunks
   from flash.
5. Anything else: `404` JSON error.

Every response carries `Connection: close` and `Content-Length`.

The server is written against `arduino::Client` and a byte-array static
asset so it does not depend on WiFiEspAT beyond the `WiFiServer` it is
given.

### 3.6 Web APIs

Same contract style as `LampWebApi`: static `handle(method, path,
requestBody, body)` returning the status code, static `owns(path)`.

`NetWebApi`, `/api/net`:

| Route | Method | Body | Result |
|---|---|---|---|
| `/api/net/status` | GET | | status object below |
| `/api/net/scan` | GET | | `{"networks":[{"ssid","rssi","secure"}]}`, up to 12, strongest first, blocking scan of a few seconds |
| `/api/net/config` | GET | | `NetConfig` without secrets plus `hasWifi`, `hasPassword` |
| `/api/net/config` | PUT | any subset of `hostname`, `password`, `ntp`, `tz` | saves, applies hostname/tz/ntp live when Online, returns config |
| `/api/net/connect` | POST | `{"ssid","pass"}` | stores credentials, starts the attempt, `202` with status |
| `/api/net/forget` | POST | | clears credentials, goes to Portal, returns status |

Status object:

```json
{
  "mode": "nomodule" | "connecting" | "online" | "portal",
  "ssid": "Home", "ip": "192.168.1.42", "rssi": -61,
  "hostname": "miniworld",
  "apSsid": "miniWorld-1A2B", "apIp": "192.168.4.1", "apActive": true,
  "connectResult": "idle" | "connecting" | "ok" | "failed",
  "auth": false,
  "timeValid": true
}
```

`SystemWebApi`, `/api/system`:

| Route | Method | Result |
|---|---|---|
| `/api/system/status` | GET | `{"firmware":"0.1.0","build":"2026-09-06 19:40","uptime":1234,"heap":180000,"time":"2026-09-06T19:40:12","timeValid":true}` |
| `/api/system/reboot` | POST | `200 {"ok":true}` then `rp2040.reboot()` after the response is flushed |

Firmware version is a `#define MINIWORLD_VERSION` in `Version.h`; the
build stamp is `__DATE__ " " __TIME__`.

### 3.7 The sketch

`miniWorld_LightingController.ino` becomes:

```
setup():  Serial, LittleFS, Lamps.begin(), Scene.begin() (seed as today),
          Net.begin(), Http.begin()
loop():   Net.tick(); Http.tick(); Scene.tick();
```

Version and build stamp printed at boot. The seeded example town stays,
because a device with lamps but no scene should show something.

## 4. Browser side

### 4.1 Packaging

- Sources: `web/index.html`, `web/app.css`, `web/app.js`. Plain files,
  editable and servable as they are.
- `tools/buildweb.py` inlines the CSS and JS into the HTML, strips
  comments and leading whitespace, gzips at level 9, and writes
  `miniWorld_LightingController/WebUI.gen.h`:

```c
// Generated by tools/buildweb.py, do not edit.
#define WEBUI_ETAG "\"3f9a1c2e\""
static const size_t WEBUI_GZ_LEN = 31274;
static const uint8_t WEBUI_GZ[] PROGMEM = { ... };
```

  The ETag is the first eight hex digits of the SHA-1 of the gzip bytes.
- Budget: 40 kB gzipped. The build script prints the size and fails
  above 48 kB.
- `make web` builds it; `make` depends on it so the header can never be
  stale. `WebUI.gen.h` is generated, listed in `.gitignore`.

### 4.2 Application structure

Vanilla JS, one file, no build-time transpiling. Hash routing:
`#home`, `#wifi`, `#lamps`, `#scene`. One `api()` helper does `fetch`
with JSON, turns non-2xx into an error toast, and reloads the page on
401 so the browser's auth prompt appears. Each view is an object with
`mount()`, `unmount()` and a `poll()` the router calls every 2 s while
the view is visible (status refresh) and stops when it is not.

Error and offline behaviour: a failed poll shows a quiet "no contact"
badge in the header instead of a toast, and clears on the next success.
After onboarding, when the phone loses the AP, the WiFi view keeps
showing the last known result and the home network address.

### 4.3 Visual system: night town

- Ground `#0b0f14`, cards `#141b23`, lines `#243040`. Text `#d7dfe6`,
  muted `#8a98a6`. Accent, the lamp, `#f4b642`; glow `#ffd47a`; sky
  gradient for the scrubber from `#7fb2e5` (day) through `#e08a4a` (dusk)
  to `#0b0f14` (night). OK `#63c27a`, fault `#ea6a4e`. All text pairs
  meet 4.5:1 on their background.
- Type: `system-ui, -apple-system, "Segoe UI", Roboto, sans-serif`,
  16 px base on phone, `font-variant-numeric: tabular-nums` on every
  time and count. One display size for the town clock on Home.
- Lamp discs: an 8 px circle, `--line` when off, accent with a soft glow
  box-shadow when lit, `transition: 400ms`. Used for lamps in the Home
  strip and in the group editor.
- Scrubber: full-width strip, 56 px tall, the sky gradient positioned by
  dusk and dawn from status, dawn and dusk tick marks with labels, a sun
  thumb, and a range input laid over it for touch. Dragging in manual
  mode sends `PUT /api/scene/clock {"mode":"manual","time":"HH:MM"}`
  throttled to one request per 150 ms.
- Touch targets 44 px minimum. Focus rings visible. Status conveyed by
  text or icon shape as well as colour. `prefers-reduced-motion` disables
  the glow transition.
- Phone first: single column, cards full width, sticky header with the
  four view links. From 720 px the Lamps and Scene views go to two
  columns.

### 4.4 Views

Home:
- Town clock (large), mode, dusk and dawn, lit count over lamp count.
- Buses strip: twelve small tiles, each showing what is fitted and a
  fault mark if any device on it faulted.
- Network line: mode, SSID or AP name, address.
- Scene enable switch (`PUT /api/scene/clock {"enabled":..}`).

WiFi (also the portal landing page):
- Current state at the top. In Portal: "This controller is not on a WiFi
  network" with the AP name.
- Network list from `/api/net/scan` with signal bars and a lock glyph;
  tap to select; password field; Connect button. Progress from
  `connectResult` polling; on `ok`: the new address and the mDNS name,
  and a note that the AP switches off shortly. On `failed`: a plain
  message and the form again.
- Settings card: hostname, device password (set or clear), NTP server,
  timezone as a select with Sweden/CET first, then UTC, WET, EET, plus a
  free text field for a POSIX TZ string. Save sends `PUT /api/net/config`.
- Forget WiFi button with a confirm step.

Lamps:
- Twelve rows: bus number and GPIO pair, SX1503 switch, AL5887 stepper
  0..4, and the status glyphs from `/api/lamps/status`.
- Global: bus speed select (100k, 400k), active-low switch, RGB label
  switch.
- Probe button: calls `/api/lamps/probe`, shows the result as a diff
  against the form (found but not configured, configured but not found),
  with "use what was found" to copy it into the form.
- Save applies the form with `PUT /api/lamps/config`. Test card: a level
  slider for all lamps and a lamp number field for one, `POST /api/lamps/test`.

Scene:
- Clock card: mode segmented control, the scrubber, day-of-year (with a
  date readout), speed for accelerated mode, dusk and dawn.
- Groups card: list of groups, each a row with name, behaviour and lamp
  count; tap to expand. Expanded: name, behaviour select, lamp ranges
  text (`0-15, 20`), the override fields seeded from `/api/scene/presets`
  when the behaviour changes, delete. Add group. Save sends the whole
  scene with `PUT /api/scene/config`; unsaved edits are marked.
- Location card: latitude, longitude, seed.

## 5. Build and mock

- `Makefile` gains `web` (generated header), `mock` (runs the mock
  server on port 8080), and `compile` depends on `web` as it does on
  `pio`.
- `tools/mockserver.py`, standard library only: serves `web/` unbuilt
  at `/`, implements all routes in section 3 with in-memory state,
  simulates the scene clock (advancing minutes, fixed dusk 19:58 and
  dawn 05:47 that shift with day of year), fakes a scan list and a
  connect attempt that succeeds after 4 s, and honours basic auth when a
  password is set. `--portal` starts it in Portal mode. It exists so the
  UI can be designed and tested without hardware.

## 6. Verification

- `make` compiles warning-free after every step.
- UI: every view exercised against the mock in a desktop browser and on
  a phone (the mock is reachable on the LAN), then reviewed with the
  web-design-guidelines skill. The gzipped size is under budget.
- API: a `tools/apicheck.sh` with curl calls for every route, run
  against the mock and then against the device.
- Bench, in this order, on the connected Challenger NB RP2040 WiFi:
  1. Boot log shows the ESP answering at 921600 and the state machine
     reaching Online (previously persisted network) or Portal.
  2. Portal: phone joins `miniWorld-XXXX`, the sign-in sheet opens on the
     WiFi view. `dig @192.168.4.1 example.com` answers 192.168.4.1. This
     confirms the DHCP-DNS assumption in 3.4.
  3. Onboarding: pick a network, connect, see the address; the AP goes
     away; `http://miniworld.local/` opens from the home network.
  4. Auth: set a password, reload, the browser prompts; curl without
     credentials gets 401.
  5. Reboot: the device comes straight back Online.
- Lamps and scene behaviour on real I2C hardware is not part of this
  spec; HANDOFF.md section 5.3 covers that bring-up.

## 7. Decisions and their reasons

- **One file, embedded in the firmware, not LittleFS.** One request over
  the UART, no second upload tool, no drift between UI and firmware.
- **Open onboarding AP.** A password on the AP has to be printed
  somewhere and typed before the portal can even appear. The GUI
  password protects control; the AP is only up when the device is not
  on a network.
- **Basic auth over HTTP.** The only scheme a browser handles without any
  page-side login flow, and it works inside a captive sign-in sheet.
- **AP kept up during onboarding.** Without it the phone drops off the
  moment the device switches to station mode and never sees the result.
- **Baud 921600.** The 40 kB page takes about four seconds at 115200 and
  under half a second at 921600; the variant helper already supports the
  change.
- **Python for tooling.** Present on the machine, no npm, and the mock
  server has to run on a laptop at the bench.
