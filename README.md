# miniWorld lighting controller

Firmware and web GUI for lighting a model railway town (H0, 1:87) from an
iLabs Challenger NB RP2040 WiFi. Up to twelve I2C buses, one per building
or cluster, carry small LED driver boards over the BConnect Bi2C FFC
interconnect. A scene engine runs the town through a day: street lights
at civil dusk, flats lighting up one by one, shops closing, the pub going
dark last, computed from the real date and latitude or from an
accelerated or manually scrubbed clock.

Everything about the fitted hardware and the scene is runtime
configuration, stored on the board and edited from a phone through a
built-in web interface. Nothing hardware related is compiled in except
the pin map.

## Hardware

- Controller: Challenger NB RP2040 WiFi. WiFi is an ESP8285 co-processor
  driven over a UART with AT commands (WiFiEspAT library); the RP2040 runs
  the Arduino framework through the arduino-pico core.
- Twelve I2C buses: two hardware controllers, seven PIO state machines and
  three bit-banged, all presenting the same `HardwareI2C` interface.
- Per bus: one SX1503 16-channel expander (on/off lamps) and up to four
  AL5887 36-channel 12-bit PWM drivers (dimmable), in any mix, up to 1920
  lamps in total.

The pin map and the bus order are in `HANDOFF.md`, which is the design
document for the whole project and the place to start reading.

## What it does

- **Captive portal onboarding.** A board with no usable WiFi raises its
  own open access point, `miniWorld-XXXX`. Joining it from a phone opens
  the sign-in sheet on the WiFi page, where you pick a network and enter
  its password. The same access point serves the full GUI at exhibitions
  with no router.
- **Web GUI**, one page with six views: Home (town time, dusk and dawn,
  lit lamps, bus health, network), WiFi (portal page and device
  settings), Lamps (what is fitted on each bus, probe, test), Scene
  (clock mode, a horizon scrubber for the time of day, location and
  seed), Models (the ways a building can behave, one editor per kind)
  and Houses (units, their rooms, and the model each one runs). Dark,
  phone first, nothing fetched from outside the board.
- **Models.** A model is a named way of behaving and owns no lamps: a
  rhythm model is a household's day (wake, leave, home, bed); an hours
  model is an opening day, on and off anchored to dusk, dawn or the
  clock. Nine built-in templates (Family, Elderly couple, Night owl,
  Away, Home, Shop, Pub, Street light, All night) seed a fresh device.
  Every lamp in a unit on a model gets a stable personal habit derived
  from its index and a seed, so the town looks like the same people
  living there evening after evening, with a little daily jitter.
- **Units.** A unit is a thing on the layout: a name, a building label,
  the model it runs, and rooms of lamp ranges. The Anderssons at
  Storgatan 3 are a unit on the Family model; the shop on the corner and
  a whole street of lamps are units too, each on an hours
  model. A rhythm unit's rooms light in order: kitchen for the coffee,
  hall on the way out, living room in the evening, bedroom last. Seen in
  the Houses tab and as chips on the Home strip.
- **JSON API** under `/api/lamps`, `/api/scene`, `/api/net` and
  `/api/system`, with optional HTTP basic auth behind one device password.
  SNTP and mDNS (`miniworld.local`) through the AT firmware.

## Repository layout

| Path | Contents |
|---|---|
| `miniWorld_LightingController/` | The Arduino sketch: I2C transports, lamp drivers, lamp and scene models, network manager, HTTP server, web APIs |
| `web/` | The GUI sources: `index.html`, `app.css`, `app.js` and one `view-*.js` and `view-*.css` per view |
| `tools/` | `buildweb.py` (packs `web/` into a gzipped header), `mockserver.py` (fakes the API for off-target work), `apicheck.sh`, and the guarded flashing scripts |
| `docs/superpowers/` | The design spec and the implementation plan for the GUI and portal |
| `HANDOFF.md` | Design document: layering, pin map, work queue, unverified facts, conventions |
| `CLAUDE.md` | Working notes for the coding agent: build rules, architecture, traps |
| `Makefile` | The build |

## Building and flashing

Requirements: arduino-cli with the arduino-pico core (board
`rp2040:rp2040:challenger_nb_2040_wifi`), ArduinoJson 7, WiFiEspAT, and
Python 3 for the tools. GNU make.

```
cp miniWorld_LightingController/NetDefaults.example.h \
   miniWorld_LightingController/NetDefaults.h      # optional default network
make              # assemble PIO, pack the GUI, compile into ./build
make upload       # check the image, find the board by USB id, flash, verify
make console      # read the board's USB console
make mock         # run the mock API server on http://localhost:8080
make check        # exercise every API route against the mock
```

Flashing is deliberately guarded: the image must be the one just built
from this tree, the board is found by its USB identity rather than a
guessed port, and after flashing the boot banner is read back and must
carry the image's build stamp. `make erase-net` flashes a one-shot build
that forgets the stored WiFi network. See `CLAUDE.md` for the reasoning.

## Working on the GUI without hardware

```
make mock
```

then open `http://localhost:8080/` in a browser, or on a phone on the
same network. The mock serves `web/` unbuilt and implements every route
with in-memory state, a simulated clock and a fake network scan;
`--portal` starts it in portal mode and `--password` turns on auth.

## Status

Compiles warning-free and every module has been reviewed; the network
stack and GUI have run on a board and served the page over WiFi. The I2C
transports and the AL5887 driver have not yet been exercised on real
hardware, and a few facts about the ESP8285 AT firmware and the AL5887
register map remain to be confirmed on the bench. `HANDOFF.md` sections 5
and 6 list exactly what is open.

Invector Embedded Systems AB
