/*
    Lamps - see Lamps.h

    Everything in this file is private to the lamp subsystem. The pin map and
    the bus order live here and nowhere else. Which hardware is fitted, and
    how many devices, comes from LampConfig at runtime.

    Bus order for SX1503 (one device per bus, address 0x20):
        0  Wire   GPIO0/1     (SDA/SCL)
        1  Wire1  GPIO26/27   (A0/A1)
        2  PIO    GPIO2/3     (D5/D6)
        3  PIO    GPIO6/7     (D9/D10)
        4  PIO    GPIO8/9     (D11/D12)
        5  PIO    GPIO14/15   (D14/D15)
        6  PIO    GPIO22/23   (SCK/SDO)
        7  PIO    GPIO24/25   (SDI/A4)
        8  PIO    GPIO16/17   (TX/RX)
        9  soft   GPIO10/18   (D13/D16)
        10 soft   GPIO20/21   (D17/A5)
        11 soft   GPIO28/29   (A2/A3)

    A device count of N uses the first N buses. AL5887 uses only bus 0, with
    devices at 0x30, 0x31, 0x32, 0x33.

    Invector Embedded Systems AB
*/

#include "Lamps.h"
#include "LampDriver.h"
#include "AL5887LampDriver.h"
#include "PIOWire.h"
#include "BitBangWire.h"

#include <Wire.h>
#include <string.h>

#define AL5887_BASE_ADDRESS 0x30

// ---------------------------------------------------------------------------
// Buses, constructed once, started only when a configuration needs them.
// ---------------------------------------------------------------------------

static PIOWire pioBus[] = {
    PIOWire(2, 3),
    PIOWire(6, 7),
    PIOWire(8, 9),
    PIOWire(14, 15),
    PIOWire(22, 23),
    PIOWire(24, 25),
    PIOWire(16, 17),
};

static BitBangWire softBus[] = {
    BitBangWire(10, 18),
    BitBangWire(20, 21),
    BitBangWire(28, 29),
};

static const uint8_t NUM_PIO = sizeof(pioBus) / sizeof(pioBus[0]);
static const uint8_t NUM_SOFT = sizeof(softBus) / sizeof(softBus[0]);
static const uint8_t NUM_BUSES = 2 + NUM_PIO + NUM_SOFT;

static bool busStarted[NUM_BUSES] = { false };

static arduino::HardwareI2C *busAt(uint8_t i) {
    if (i == 0) {
        return &Wire;
    }
    if (i == 1) {
        return &Wire1;
    }
    i = (uint8_t)(i - 2);
    if (i < NUM_PIO) {
        return &pioBus[i];
    }
    i = (uint8_t)(i - NUM_PIO);
    if (i < NUM_SOFT) {
        return &softBus[i];
    }
    return nullptr;
}

static void startBus(uint8_t i, uint32_t speed) {
    if (i >= NUM_BUSES || busStarted[i]) {
        return;
    }
    if (i == 0) {
        Wire.setSDA(0);
        Wire.setSCL(1);
    } else if (i == 1) {
        Wire1.setSDA(26);
        Wire1.setSCL(27);
    }
    arduino::HardwareI2C *b = busAt(i);
    b->setClock(speed);
    b->begin();
    busStarted[i] = true;
}

static void stopBus(uint8_t i) {
    if (i >= NUM_BUSES || !busStarted[i]) {
        return;
    }
    busAt(i)->end();
    busStarted[i] = false;
}

static bool ping(arduino::HardwareI2C *b, uint8_t address) {
    b->beginTransmission(address);
    return b->endTransmission() == 0;
}

// ---------------------------------------------------------------------------

static uint16_t lampLevel[LAMPS_MAX_LAMPS];

LampController Lamps;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool LampController::begin() {
    LampConfig cfg;
    if (!LampConfigStore::load(cfg)) {
        cfg = LampConfig();             // nothing stored: no lamps, wait for GUI
    }
    return apply(cfg, false);
}

bool LampController::apply(const LampConfig &in, bool persist) {
    LampConfig cfg = in;
    cfg.clamp();

    teardown();
    bool ok = build(cfg);

    if (persist) {
        LampConfigStore::save(cfg);
    }
    return ok;
}

void LampController::end() {
    teardown();
}

bool LampController::build(const LampConfig &cfg) {
    _cfg = cfg;
    _numDev = 0;
    _count = 0;
    memset(lampLevel, 0, sizeof(lampLevel));

    bool ok = true;

    switch (cfg.hardware) {

        case LampHardware::AL5887: {
            startBus(0, cfg.busSpeed);
            for (uint8_t i = 0; i < cfg.devices; i++) {
                AL5887LampDriver *d = new AL5887LampDriver(
                    *busAt(0), (uint8_t)(AL5887_BASE_ADDRESS + i));
                if (!d->begin()) {
                    ok = false;
                }
                _dev[_numDev++] = d;
            }
            break;
        }

        case LampHardware::SX1503: {
            for (uint8_t i = 0; i < cfg.devices && i < NUM_BUSES; i++) {
                startBus(i, cfg.busSpeed);
                SX1503LampDriver *d = new SX1503LampDriver(*busAt(i));
                d->setActiveLow(cfg.activeLow);
                if (!d->begin()) {
                    ok = false;
                }
                _dev[_numDev++] = d;
            }
            break;
        }

        default:
            break;
    }

    for (uint8_t i = 0; i < _numDev; i++) {
        uint16_t ch = _dev[i]->channels();
        if (_count + ch > LAMPS_MAX_LAMPS) {
            break;
        }
        _count = (uint16_t)(_count + ch);
    }

    _built = true;
    return ok;
}

void LampController::teardown() {
    if (!_built) {
        return;
    }
    for (uint8_t i = 0; i < _numDev; i++) {
        delete _dev[i];
        _dev[i] = nullptr;
    }
    _numDev = 0;
    _count = 0;
    for (uint8_t i = 0; i < NUM_BUSES; i++) {
        stopBus(i);
    }
    _built = false;
}

LampConfig LampController::probe() {
    LampConfig running = _cfg;
    bool wasBuilt = _built;
    teardown();

    LampConfig found;
    found.busSpeed = 100000;        // be gentle while sniffing

    // AL5887 first: consecutive addresses from 0x30 on the primary bus.
    startBus(0, found.busSpeed);
    uint8_t n = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if (ping(busAt(0), (uint8_t)(AL5887_BASE_ADDRESS + i))) {
            n++;
        } else {
            break;
        }
    }
    if (n > 0) {
        found.hardware = LampHardware::AL5887;
        found.devices = n;
    } else {
        // SX1503: count consecutive buses that answer at 0x20, in bus order.
        n = 0;
        for (uint8_t i = 0; i < NUM_BUSES; i++) {
            startBus(i, found.busSpeed);
            if (ping(busAt(i), SX1503_I2C_ADDRESS)) {
                n++;
            } else {
                break;
            }
        }
        if (n > 0) {
            found.hardware = LampHardware::SX1503;
            found.devices = n;
        }
    }

    for (uint8_t i = 0; i < NUM_BUSES; i++) {
        stopBus(i);
    }

    found.busSpeed = running.busSpeed;
    found.activeLow = running.activeLow;
    found.rgb = running.rgb;
    found.clamp();

    if (wasBuilt) {
        build(running);
    }
    return found;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

bool LampController::deviceFaulted(uint8_t device) const {
    if (device >= _numDev) {
        return true;
    }
    return _dev[device]->faulted();
}

bool LampController::intensitySupported() const {
    if (_numDev == 0) {
        return false;
    }
    for (uint8_t i = 0; i < _numDev; i++) {
        if (!_dev[i]->hasIntensity()) {
            return false;
        }
    }
    return true;
}

uint8_t LampController::resolutionBits(uint16_t lamp) const {
    LampDriver *d;
    uint16_t ch;
    if (!resolve(lamp, &d, &ch)) {
        return 0;
    }
    return d->resolutionBits();
}

bool LampController::faulted(uint16_t lamp) const {
    LampDriver *d;
    uint16_t ch;
    if (!resolve(lamp, &d, &ch)) {
        return true;
    }
    return d->faulted();
}

// ---------------------------------------------------------------------------
// Lamps
// ---------------------------------------------------------------------------

bool LampController::resolve(uint16_t lamp, LampDriver **driver, uint16_t *channel) const {
    if (lamp >= _count) {
        return false;
    }
    uint16_t base = 0;
    for (uint8_t i = 0; i < _numDev; i++) {
        uint16_t ch = _dev[i]->channels();
        if (lamp < base + ch) {
            *driver = _dev[i];
            *channel = (uint16_t)(lamp - base);
            return true;
        }
        base = (uint16_t)(base + ch);
    }
    return false;
}

void LampController::setIntensity16(uint16_t lamp, uint16_t level) {
    LampDriver *d;
    uint16_t ch;
    if (!resolve(lamp, &d, &ch)) {
        return;
    }
    lampLevel[lamp] = level;
    d->setChannel(ch, level);
    if (_autoShow) {
        d->flush();
    }
}

void LampController::setIntensity(uint16_t lamp, uint8_t level) {
    setIntensity16(lamp, (uint16_t)(level * 257));
}

uint16_t LampController::intensity16(uint16_t lamp) const {
    return (lamp < _count) ? lampLevel[lamp] : 0;
}

uint8_t LampController::intensity(uint16_t lamp) const {
    return (uint8_t)(intensity16(lamp) >> 8);
}

void LampController::setAll16(uint16_t level) {
    for (uint16_t i = 0; i < _count; i++) {
        LampDriver *d;
        uint16_t ch;
        if (resolve(i, &d, &ch)) {
            lampLevel[i] = level;
            d->setChannel(ch, level);
        }
    }
    if (_autoShow) {
        show();
    }
}

bool LampController::show() {
    bool ok = true;
    for (uint8_t i = 0; i < _numDev; i++) {
        if (!_dev[i]->flush()) {
            ok = false;
        }
    }
    return ok;
}
