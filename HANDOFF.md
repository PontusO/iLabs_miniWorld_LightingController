# Lamp controller: handoff

Firmware and hardware brief for a distributed model-railway lighting
controller (H0, 1:87) built on the iLabs Challenger NB RP2040 WiFi. This
document is the entry point. Read it before touching any file.

Owner: Pontus, Invector Embedded Systems AB (iLabs). Written 2026-09-06.

---

## 1. What this is

A Challenger RP2040 drives up to twelve independent I2C buses, one per
building or cluster, over the iLabs BConnect Bi2C FFC interconnect. On
each bus sit LED driver boards: SX1503 16-channel expanders (on/off) or
AL5887 36-channel 12-bit PWM drivers (dimmable), or both. A scene engine
runs the town through a day: street lights at civil dusk, flats lighting
up one by one, shops closing, the pub going dark last, computed from the
real date and latitude or from an accelerated or manually scrubbed clock.

The product has a web GUI. All configuration is runtime, stored in
LittleFS as JSON, edited through a small HTTP API. Nothing about the
hardware is compiled in except the pin map.

Target: arduino-pico core (Earle Philhower), Arduino framework, built
with `make` at the repo root. Debug output is USB CDC (`Serial`); the
UART pins are used as an I2C bus.

## 2. Layering

```
  miniWorld_LightingController.ino   application: Net.tick, Http.tick, Scene.tick
  ───────────────────────────────────────────────────────────────
  NetManager              WiFi state machine, portal AP, SNTP, mDNS
  DnsResponder            portal DNS, one answer for every name asked
  HttpServer              parsing, basic auth, routing, SPA from flash (Home, WiFi, Lamps, Scene, Houses)
  NetWebApi               /api/net/*
  SystemWebApi            /api/system/*
  ───────────────────────────────────────────────────────────────
  SceneEngine             clock modes, habits, fades, flicker, events (day lights, dips, night wake-ups), flats (households with rooms)
  Scene / Sun             scene data model + JSON, solar calculation
  SceneWebApi             /api/scene/*
  ───────────────────────────────────────────────────────────────
  Lamps  (LampController) public lamp API: index + 16-bit intensity
  LampConfig              which hardware is fitted, JSON, LittleFS
  LampWebApi              /api/lamps/*
  RgbLamps                optional RGB-triplet view over Lamps
  ───────────────────────────────────────────────────────────────
  LampDriver              abstract: channels, setChannel(16-bit), flush
    SX1503LampDriver      1-bit, threshold
    AL5887LampDriver      12-bit, range-based burst flush
  ───────────────────────────────────────────────────────────────
  arduino::HardwareI2C    Wire, Wire1 (hardware)
    PIOWire               PIO state machine I2C master, 7 instances
    BitBangWire           software I2C, 3 instances, real repeated start
```

The rule that made this work: everything above `Lamps` knows nothing
about I2C, PIO, registers or buses. Everything below `LampDriver` knows
nothing about lamps, scenes or time. `Lamps.cpp` is the only file that
knows the pin map and the bus order. The network band names no lamp and
no group either: `HttpServer` hands a path to whichever web API `owns()`
it, and every web API stays a pure `(method, path, body)` function.

## 3. File inventory

| File | Purpose | Status |
|---|---|---|
| `PIOWire.h/.cpp` | HardwareI2C on a PIO SM. Non-adjacent SDA/SCL allowed. | Compiles, untested on hardware |
| `BitBangWire.h/.cpp` | HardwareI2C bit-banged. Clock stretch, repeated start. | Compiles, untested |
| `LampDriver.h/.cpp` | Driver interface + `SX1503LampDriver` | Register map from SX150x datasheet family, verify |
| `AL5887LampDriver.h/.cpp` | AL5887 backend | **Register map unconfirmed**, see §6 |
| `Lamps.h/.cpp` | Public API, runtime device construction, probe | Per-bus rework done, untested on hardware |
| `LampConfig.h/.cpp` | Hardware config model, JSON, LittleFS | Per-bus rework done, reviewed |
| `LampWebApi.h/.cpp` | `/api/lamps/config, status, probe, test` | Per-bus status object done, checked against the mock |
| `RgbLamps.h` | `setColor(module, r, g, b)` over Lamps | Done |
| `Sun.h/.cpp` | Sunrise/sunset/civil twilight | **Verified** against Lund almanac |
| `Scene.h/.cpp` | Groups, behaviours (incl. the four household presets), flats (households with rooms), clock, location, JSON | Compiles, reviewed, not yet run on hardware |
| `SceneEngine.h/.cpp` | The simulation, plus the event layer (day lights, dips, night wake-ups) and `evaluateFlats()` for the flats' rooms | Compiles, reviewed, not yet run on hardware |
| `SceneWebApi.h/.cpp` | `/api/scene/config, status, clock, presets` | Done |
| `NetConfig.h/.cpp` | `/net.json`: credentials, hostname, GUI password, NTP, TZ | Compiles, reviewed |
| `NetDefaults.h` | Compile-time default network for a board with no `/net.json`; gitignored, copy `NetDefaults.example.h` | Done |
| `NetManager.h/.cpp` | WiFi state machine, portal AP, SNTP, mDNS, global `Net` | Compiles, reviewed, not yet on a phone |
| `DnsResponder.h/.cpp` | Portal DNS on port 53, one A record for any name | Compiles, reviewed, not yet on a phone |
| `HttpServer.h/.cpp` | Parsing, basic auth, routing, the SPA from flash | Compiles, reviewed |
| `NetWebApi.h/.cpp` | `/api/net/status, scan, config, connect, forget` | Compiles, checked against the mock |
| `SystemWebApi.h/.cpp` | `/api/system/status, reboot` | Compiles, checked against the mock. The GUI reads `status` for the firmware line on Home; `reboot` has no button and is curl only. |
| `Version.h` | `MINIWORLD_VERSION`, printed at boot and in the status | Done |
| `web/` | SPA, five views, built into `WebUI.gen.h` | Done, reviewed at 320 px and 390 px |
| `web/view-houses.js/.css` | Houses view: households by building, room states, the rhythm editor | Compiles, reviewed, not yet run on hardware |
| `tools/` | `buildweb.py`, `mockserver.py`, `apicheck.sh` | Done |
| `miniWorld_LightingController.ino` | Application sketch: Net.tick, Http.tick, Scene.tick | Compiles; portal bring-up on a phone still to do, see §5.5 |
| `i2c.pio`, `pio_i2c.c/.h` | PIO I2C program and primitives, from pico-examples | Vendored, assert removed |
| `Makefile` (repo root) | arduino-cli wrapper: pioasm, buildweb.py, one explicit `./build` directory for compile and upload, `DEFINES=` passthrough | Done |
| `tools/flash.sh`, `checkimage.sh`, `findboard.sh`, `console.sh` | Guarded flashing: image freshness, identity and size check; board found by USB id; banner verified after flashing | Done, findboard and checkimage exercised; flash end to end pending the bench |
| `lamp-hardware-brief.md` | PCB design brief for carrier and AL5887 board | Needs the 4+6 connector patch, see §5 |

`i2c.pio`, `pio_i2c.c` and `pio_i2c.h` are copied from
`pico-examples/pio/i2c` (BSD-3, Raspberry Pi Ltd) with the
`assert(pin_scl == pin_sda + 1)` line removed from `i2c_program_init()`;
the body uses full GPIO masks and does not need adjacency. `i2c.pio.h` is
generated by `make` (see `Makefile` at the repo root) and is not a source
file. `WebUI.gen.h` is generated the same way, by `make web` from `web/`;
edit the sources under `web/`, never the header.

Dependencies: ArduinoJson 7, LittleFS (in the core; a filesystem size
must be set in the board menu or every save fails silently), and
WiFiEspAT. The Challenger NB RP2040 WiFi does its WiFi through an ESP8285
co-processor on a UART, driven by AT commands; the arduino-pico core's
own `WiFi.h` (lwIP over the Pico W's CYW43) does not work on this board.

## 4. Pin map and bus order

Challenger NB RP2040 WiFi header GPIOs: 0-3, 6-10, 14-18, 20-29.
GPIO 4, 5, 11, 12, 13, 19 are internal.

| Bus | GPIO SDA/SCL | Feather | Transport |
|---|---|---|---|
| 0 | 0/1 | SDA/SCL | Wire (i2c0), also the Challenger's own Bi2C connector |
| 1 | 26/27 | A0/A1 | Wire1 (i2c1) |
| 2 | 2/3 | D5/D6 | PIO |
| 3 | 6/7 | D9/D10 | PIO |
| 4 | 8/9 | D11/D12 | PIO |
| 5 | 14/15 | D14/D15 | PIO |
| 6 | 22/23 | SCK/SDO | PIO |
| 7 | 24/25 | SDI/A4 | PIO |
| 8 | 16/17 | TX/RX | PIO |
| 9 | 10/18 | D13/D16 | bit-bang |
| 10 | 20/21 | D17/A5 | bit-bang |
| 11 | 28/29 | A2/A3 | bit-bang |

Seven PIO SMs used of eight. Nothing else PIO-based can be added on
RP2040 without dropping a bus. Do not call `Serial1.begin()` anywhere;
it would reclaim GPIO16/17.

## 5. Work queue, in order

### 5.1 Per-bus hardware configuration (done 2026-09-06)

Firmware and GUI are done and compile. The one bullet still open below is
the last one, the `lamp-hardware-brief.md` rewrite, which is a PCB
document rather than firmware. The record of what was decided follows.

The carrier gets a Bi2C-04 and a Bi2C-06 connector on every bus, with
GPIO 1 of the 6-way repurposed as a 5 V output. AL5887 boards can
therefore sit on any bus, and an SX1503 (0x20) and up to four AL5887
(0x30-0x33) can share one bus. The current config model (one hardware
type for the whole system, AL5887 only on bus 0) must become per bus:

```json
{
  "busSpeed": 400000,
  "activeLow": false,
  "buses": [
    { "sx1503": true,  "al5887": 0 },
    { "sx1503": false, "al5887": 2 },
    ...12 entries...
  ]
}
```

Changes:

- `LampConfig`: replace `hardware`/`devices` with `buses[12]` of
  `{bool sx1503; uint8_t al5887;}`. Keep `busSpeed`, `activeLow`, `rgb`.
- `Lamps::build()`: per bus, start the bus if anything is on it, create
  the SX1503 driver first, then AL5887 drivers in address order. Lamp
  numbering is bus order then that order.
- `Lamps::probe()`: per bus, ping 0x20 and 0x30..0x33, stop AL5887 count
  at first non-responder.
- `LAMPS_MAX_LAMPS` 512 → 2048 (12 × (16 + 4 × 36) = 1920). Adjust
  `Scene` bitmaps (`lampBits` becomes 256 bytes per group) and the
  engine's per-lamp arrays. About 12 kB extra RAM total.
- `LampWebApi::statusJson`: faults becomes per device in lamp order, plus
  a per-bus summary.
- The Lamps view (`web/view-lamps.js`, which replaced `lamps.html`):
  twelve rows, each with an SX1503 switch and an AL5887 count 0-4, plus
  the global flags.
- `lamp-hardware-brief.md`: replace "Bus connectors" and Board B
  connector sections with 4-way + 6-way per channel; 5 V on 6-way pin
  GPIO 1; 500 mA hold polyfuse per 6-way 5 V; remove separate 5 V power
  outputs; remove the bus-0-on-carrier question (it is on the carrier
  now); AL5887 board takes 5 V from Bi2C-06 IN, no separate LED power
  connector.

### 5.2 Scene scrubber page (done 2026-09-06)

Built as the Scene view of the SPA (`web/view-scene.js`), not as a
separate `scene.html`: time slider 00:00-23:59 driving
`PUT /api/scene/clock` with `{"mode":"manual","time":...}`, mode
buttons, day-of-year and speed controls, a dusk/dawn readout from
`/api/scene/status`, and a group editor (name, behaviour, lamp ranges,
overrides seeded from `/api/scene/presets`). Served from flash, no
external resources.

### 5.3 Hardware bring-up, in this order

1. Bus 0 only, one SX1503, a throwaway sketch driving `Wire` directly
   (the old NineBuses test sketch was removed). Confirm
   register map and byte order (bit 0 must be IO0).
2. One PIOWire bus. Scope SDA/SCL: verify the non-adjacent init, the
   400 kHz clkdiv, and NAK detection (unplug the board, expect
   `endTransmission() != 0`).
3. One BitBangWire bus. Check the half-bit timing at 400 kHz; the
   `_halfCycles - 8` overhead estimate in `setClock()` is a guess.
4. All twelve. Watch the 3V3 rail.
5. AL5887 when the board exists, after §6.

### 5.4 Later, not blocking

- PIOWire true repeated start (needs the FIFO word encoding from
  `pio_i2c.c`). Only matters for a device that requires it; SX1503 and
  AL5887 do not.
- Named scenes: `/scenes/<name>.json` with an active pointer.
- Group editor drag-to-assign, driven by the manual clock so the user
  sees which lamp is which.

### 5.5 Portal bring-up on a phone

Open. Numbered last so the existing numbers stay put, but it comes
before 5.3 in practice: it needs only the board and a phone, no I2C
hardware. Run the bench sequence in section 6 of
`docs/superpowers/specs/2026-09-06-web-gui-captive-portal-design.md`, in
its order, with these three first:

1. Confirm the AT firmware's DHCP offers the AP as the name server:
   `dig @192.168.4.1 example.com` from a joined phone or laptop. Without
   it the portal never opens, whatever the rest of the code does.
2. Join the AP from a phone with a device password set, and see whether
   the sign-in sheet copes with basic auth. A captive-portal mini
   browser that cannot show the 401 prompt would need the portal page
   served unauthenticated.
3. Check that `http: listening on port 80` is followed by a real accept,
   and that three connections can be open at once (the CIPSERVER slot
   count), rather than the log line being the only evidence.

Then the rest: boot log at 921600 reaching Online or Portal, the phone
joining `miniWorld-XXXX` and getting the sign-in sheet, onboarding to a
real network, `http://miniworld.local/`, basic auth, and a reboot coming
straight back Online. The two entries added to section 6 below are what
that run confirms or refutes.

### 5.6 Scene activity on the board

Open. Needs a flashed board, no phone or extra hardware.

1. Set a group to Elderly, switch the clock to Manual and scrub through
   02:00..05:00 in one-minute steps: some lamps of that group must show
   one to four minute lights that are stable when scrubbing back.
2. Scrub through 07:00..09:00: short lights on the lamps that are off.
3. Switch to Accelerated at 20 minutes per day and watch Home's lit
   count move outside dusk and dawn.

### 5.7 Flats on the board (done 2026-09-07: five example flats on the board, morning and evening scrubbed minute by minute, order kitchen, living, bedroom, asleep; bathroom at 03:07 for the elderly; re-scrub identical; no reboot)

Open. Needs a flashed board, no phone or extra hardware.

1. Create the five example flats (Andersson family, Karlsson elderly,
   Nilsson nightowl, Persson family, Svensson away) through the Houses
   view or the API, on lamps the board reports.
2. Switch to Manual and scrub 05:30..08:30 and 21:00..00:30 in one-minute
   steps, recording `flats[].state` and `lit`: kitchen before living in
   the evening, bedroom last, `asleep` after bed, bathroom letters
   appearing briefly at night.
3. Re-scrub the same range and confirm the same result, no reboot in
   between.
4. Watch the Houses view during the scrub: the state word under each
   flat must change along with the scrubber.

## 6. Things that were not verified and must be

- **AL5887 register map.** Everything part-specific is in the define
  block at the top of `AL5887LampDriver.h`: PWM base register, bytes
  per channel, byte order, justification, chip-enable bit, reset
  register. All are placeholders. The addresses 0x30-0x33 come from the
  Diodes EVB user guide's example code, not the datasheet pin table.
- **AL5887 supply arrangement.** Whether logic VCC (3.3 V from Bi2C)
  and the LED anode rail (5 V) can differ. If not, the board needs a
  level shifter on SDA/SCL.
- **SX1503 RegAdvanced bit 7** is assumed to be auto-increment enable
  with 0 = enabled. `begin()` writes 0x00 there.
- **Challenger pull-ups on GPIO0/1.** If present, omit the carrier's.
- **BitBangWire timing** at 400 kHz, see 5.3 step 3.
- **1 MHz on 20 cm FFC.** Plausible, not measured. Do not make it a
  default until it is.
- **AT firmware DHCP offers the AP as DNS.** The captive portal only
  works if the phone is told to ask 192.168.4.1 for names. ESP-IDF's
  DHCP server does that by default and the AT firmware is not known to
  change it, but nobody has watched a lease. If it turns out not to,
  `DnsResponder` never gets a query and the sign-in sheet never opens.
  Test: `dig @192.168.4.1 example.com` from a joined phone or laptop,
  see §5.5.
- **921600 baud on the ESP link.** `NetManager::bringUpModule()` raises
  Serial2 from 115200 with `AT+UART_CUR` after `WiFi.init()`, then
  re-opens the port and checks the module still answers, falling back to
  115200 and logging `net: esp stays at 115200` if it does not. The rate
  is what makes the 20 kB page arrive in well under a second, but the
  link has never actually run at it here. Watch the boot log for
  `net: esp link 921600` and for garbled AT traffic under load.
- **RAM with flats.** The four static `SceneConfig` copies are now about
  8.8 kB each, and RAM use is about 29 percent. Adding a fifth static
  copy anywhere must be avoided; reuse one of the four instead.
- **No civil dusk at 55.7 N around midsummer.** The engine falls back to
  23:00. A clock-anchored off earlier than that wraps, so the lamp reads
  lit for about 22 hours. Pre-existing, now visible because the activity
  layer inherits it. Needs a decision: clamp the fallback, or anchor
  summer behaviour to sunset.

## 7. Conventions

- iLabs house style. No em dashes in any text, code comments or
  documentation; use commas, colons or parentheses.
- Every source file header ends with `Invector Embedded Systems AB`.
- `[stated]`-style caution applies to hardware facts: if a register or
  pin was not read from a datasheet, say so in a comment rather than
  letting it look verified.
- Levels are `uint16_t` end to end through `LampDriver`; drivers scale
  down. Never narrow the interface to match a device.
- Web API handlers are server-agnostic: `(method, path, body) → (code,
  json)`. Do not couple them to a specific server class.
- Anything served from the device: no web fonts, no CDN, no framework.
- Web UI: sources in `web/`, one view per file, built by `make`, never
  edit `WebUI.gen.h`.
- Config that the GUI edits is applied at runtime and persisted; nothing
  hardware-related is a compile-time switch.
- Adding a household type or a room role touches the enum, the name
  table, `setPreset`, the mock's presets and the GUI's lists together.

## 8. Design decisions worth knowing the reason for

- **Why three I2C transports instead of a mux.** The SX1503 has one
  fixed address, so one device per bus. Twelve buses were possible on
  the pin map, and parallel buses let all expanders refresh at once. A
  TCA9548A would have been simpler; the AL5887's four addresses later
  made most of this unnecessary, but the buses now also give physical
  distribution, one building per bus, so they stayed.
- **Why 16-bit intensity.** The first driver had 1 bit, the second has
  12. Sizing the interface to the first device would have meant
  changing it for the second. 16 bits means it never changes again.
- **Why per-lamp hashed habits.** A random roll every evening makes a
  town look like noise. A stable per-lamp value with a small daily
  jitter makes it look like the same people living there.
- **Why the engine leaves ungrouped lamps alone.** So the product can
  drive some lamps by other means (a signal, a switch) without the scene
  fighting it.
- **Why stop-then-start instead of repeated start in PIOWire.** The
  repeated-start path needs the raw FIFO word encoding from
  `pio_i2c.c`, which was not available when the wrapper was written.
  Shipping a guessed encoding was worse than shipping a documented
  limitation.
- **Why events come from a hash per five-minute slot.** The town must
  look the same when scrubbed back and forth and the same in accelerated
  and real time, so an event's presence and length are derived from
  `(seed, lamp, day, slot)` rather than rolled from a running random
  generator. The result is evaluated once per simulated minute into two
  bitmaps, so the 40 Hz tick only tests bits.
- **Why the household is the unit.** A flat gets one daily rhythm and
  the rooms are derived from it, not the other way round, because a
  family does not run four independent lamp schedules, it runs one day.
  Groups stay for everything that is not a household: a shop, a street,
  a cluster of lamps with no rooms to speak of. Evening and morning
  intervals are clipped to daylight so a clock-anchored rhythm does not
  light a living room at 19:00 in June.
