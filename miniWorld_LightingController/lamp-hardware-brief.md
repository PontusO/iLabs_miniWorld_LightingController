# Lamp controller hardware: design brief

Two new boards plus the existing SX1503 board, all interconnected with
BConnect Bi2C. Decisions are proposals with reasons; items marked **open**
need a datasheet or a decision before schematic capture.

BConnect facts this brief is built on (from ilabs.se/bconnect):

- Bi2C-04 is a straight-through 0.5 mm FFC. Source (OUT) pin order is
  SCL, SDA, 3V3, GND; Sink (IN) is GND, 3V3, SDA, SCL. Boards carry
  clearly marked IN and, where chaining makes sense, OUT.
- The 3V3 line is 3.3 V only, and a string of Bi2C devices is limited to
  250 mA. Anything needing another voltage or more current brings its own
  supply connection.
- Cables are 10, 20 and 30 cm. 20 cm at 400 kHz is well inside what the
  concept is specified for; the site itself cautions against HS mode on
  the long cable.
- The Challenger NB RP2040 WiFi already carries one Bi2C-04 Source
  connector on its own 250 mA LDO.

---

## Board A: Controller carrier

Feather-footprint carrier for the Challenger NB RP2040 WiFi. Provides
eleven more Bi2C Source connectors, a field 3V3 supply that can actually
feed them, and protection on every bus.

### Bus connectors

Bus 0 is the Challenger's own Bi2C connector on GPIO0/1. The carrier adds
Bi2C-04 Source connectors for buses 1 to 11, in firmware order:

| Bus | GPIO SDA/SCL | Feather pins | Type |
|---|---|---|---|
| 0 | 0 / 1 | on the Challenger | hardware i2c0 |
| 1 | 26 / 27 | A0 / A1 | hardware i2c1 |
| 2 | 2 / 3 | D5 / D6 | PIO |
| 3 | 6 / 7 | D9 / D10 | PIO |
| 4 | 8 / 9 | D11 / D12 | PIO |
| 5 | 14 / 15 | D14 / D15 | PIO |
| 6 | 22 / 23 | SCK / SDO | PIO |
| 7 | 24 / 25 | SDI / A4 | PIO |
| 8 | 16 / 17 | TX / RX | PIO |
| 9 | 10 / 18 | D13 / D16 | bit-bang |
| 10 | 20 / 21 | D17 / A5 | bit-bang |
| 11 | 28 / 29 | A2 / A3 | bit-bang |

Silkscreen the bus number at every connector. The firmware counts
consecutively from bus 0, so a gap in population is a gap in lamps.

Whether bus 0 should also get a carrier connector, **open**: it would let
the whole system be cabled from one edge and put bus 0 on the carrier's
2 A rail instead of the Challenger's 250 mA LDO. The cost is that the
Challenger's own connector must then be left unused, which is a thing
someone will get wrong. My preference is to add it, silkscreened
"bus 0, use this one", and to leave the Challenger's LDO output out of
the carrier's 3V3 net so the two rails never fight.

Mind the FFC entry: the cable needs a clear path onto each connector
without bending over a component. Eleven connectors along two edges,
contact side up, all facing outward, and no tall parts in the approach.

### Per-bus protection and pull-ups

At each Source connector:

- Pull-ups 4.7 k to 3V3 on SDA and SCL. 20 cm of FFC is well under
  50 pF, so 400 kHz is comfortable. Bus 0 with AL5887 boards chained on
  it is the only bus with real capacitance; 2.2 k there if it lives on
  the carrier.
- 33 R series on SDA and SCL between GPIO and connector. Cheap insurance
  for the RP2040 pad clamps when a cable is inserted live.
- ESD on SDA, SCL and 3V3 at each connector, PRTR5V0U2X or similar.
- A 250 mA hold polyfuse on each connector's 3V3. This makes the
  carrier honour the BConnect string limit per bus, so one overloaded
  building trips its own fuse rather than dragging the rail down and
  producing the erratic failures the spec warns about.
- Check whether the Challenger fits pull-ups on GPIO0/1. If so, none on
  the carrier for bus 0.

SDA and SCL are adjacent in Bi2C-04 with no ground between them. At 20 cm
and 400 kHz that is fine. 1 MHz should be measured on the bench before
it goes in the config as a default.

### Power

- **Input** 5 V, 3 A rated: 2-pin screw terminal or barrel. Reverse
  polarity P-FET, TVS, 3 A polyfuse.
- **Feather** fed through the USB pin so it runs without a cable.
- **3V3 field rail**: buck from 5 V, 2 A, TPS62A02 or similar. Feeds
  buses 1 to 11 through their polyfuses. Worst case is 11 x 250 mA, which
  the buck will not do, but real SX1503 boards draw around 100 mA and
  the per-bus fuses cap any single fault.
- **5 V field rail**: a separate 2-pin power output, or several, for
  AL5887 boards. Not on the FFC. Own 2 A polyfuse.

Rail LEDs for 5 V and 3V3. There is no spare GPIO for a status LED once
all buses are populated; use the Challenger's own.

### Other

- Feather in female headers so the Challenger can be swapped.
- Test points on 3V3, 5 V, GND.
- Four M3 holes, clearance at the Challenger's antenna end, no ground
  pour or connectors under the antenna.
- Two layers, solid ground, SDA/SCL pairs routed together.

---

## Board B: AL5887 RGB driver board

One AL5887 driving 12 RGB modules or 36 single-colour LEDs, sitting in
or under a building.

### Connectors

- **Bi2C-04 IN** (Sink): GND, 3V3, SDA, SCL. 3V3 powers the AL5887 logic.
- **Bi2C-04 OUT** (Source) for chaining up to four boards on one bus,
  addresses 0x30 to 0x33. Populated always; a board at the end of the
  chain simply has nothing plugged in.
- **LED power** 5 V in, 2-pin, JST PH or a screw terminal, from the
  carrier's 5 V field rail or a local supply. This keeps the LED current
  off the Bi2C string entirely, which at 36 x 10 mA is the only way to
  stay inside the 250 mA limit for a chain.

RSTn stays local: tact switch, internal pull-up. FAULT to a local LED.
Both could go on a Bi2C-06 GPIO1/GPIO2 in future; the carrier has no
spare GPIOs to receive them today, so Bi2C-04 keeps the connector
count down.

### AL5887 wiring

- W-QFN6060-52 with wettable flanks. AXI will see the joints.
- INT_SEL tied low for I2C.
- Address: 2-position DIP switch or solder jumpers, silkscreened with the
  resulting 0x30 to 0x33. **Confirm pin names and encoding**; the EVB code
  lists these four addresses but I have not read the pin table.
- **VCC and LED return, open.** The device runs from 2.7 to 5.5 V and the
  OUT pins are rated to 5.5 V. Confirm that logic VCC (3.3 V from Bi2C)
  and the LED anode rail (5 V) can differ. If they cannot, the I2C lines
  need a PCA9306 and VCC goes to 5 V from the LED input.
- RSET for IMAX around 12 mA; model LEDs want 2 to 10 mA and the
  per-channel current registers trim from there. Second RSET footprint
  in series with a 0 R for adjustment.
- 1 uF plus 100 nF on VCC, 10 uF on 5 V at the LED return.
- Exposed pad to a real copper area regardless of current.
- 10 k bleed on the 5 V rail so lamps go fully dark on power-down.

### LED connectors

Current sink, so every LED is anode to 5 V, cathode to an OUT pin. Both
footprints on the board, populate one:

- **Single-colour, 36 ch**: 2 x 20-pin 2.54 mm header rows, pins in
  pairs (5V, OUTn). Easy to wire 0402s on magnet wire to.
- **RGB, 12 modules**: 12 x 4-pin (5V, R, G, B), 1.25 mm or 1.0 mm pitch
  so RGB modules become small cabled assemblies.

The firmware does not care which; `rgb` in the lamp config is a GUI label.

ESD on SDA/SCL and TVS on 5 V at the connectors.

---

## Board C: SX1503 expander board

The existing board only needs a Bi2C-04 IN to fit. No OUT: with a fixed
address, two on one bus collide, and an OUT connector invites exactly
that. At 16 LEDs of 5 mA plus the SX1503 it sits at ~90 mA, inside the
string limit, so LED power comes over the FFC and no local supply is
needed. Anode to IO with a series resistor to ground matches
`activeLow = false`.

---

## Bill of decisions

1. Carrier connector for bus 0, or use the Challenger's own.
2. AL5887 VCC and LED supply independence, from the datasheet.
3. AL5887 address pin encoding, from the datasheet.
4. LED current target, which sets RSET.
5. Number of 5 V outputs on the carrier, one per expected AL5887 board.
