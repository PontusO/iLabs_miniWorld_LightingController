/*
    LampDriver - the seam between the Lamps API and whatever hardware is
                 actually behind it.

    Levels are 16 bit throughout this interface. That is not because any
    fitted device has 16 bits of resolution, but so that the interface is not
    the thing that has to change when a device with more resolution than the
    last one turns up. Each driver scales down to whatever it can render:

        SX1503      1 bit   (threshold)
        AL5887      12 bit  (level >> 4)

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include <api/HardwareI2C.h>

class LampDriver {
public:
    virtual ~LampDriver() { }

    // Bring up the device. Returns false if it did not answer.
    virtual bool begin() = 0;

    // How many lamps this driver owns.
    virtual uint16_t channels() const = 0;

    // True if the hardware renders intermediate levels rather than
    // quantising them to on or off.
    virtual bool hasIntensity() const = 0;

    // Effective resolution in bits, for callers that want to know how much
    // of the 16 bit level actually survives. 1 means on/off.
    virtual uint8_t resolutionBits() const = 0;

    // Stage a level. Must not touch the bus.
    virtual void setChannel(uint16_t channel, uint16_t level) = 0;

    // Push staged changes to the device. Returns false on a bus error.
    // Must be a no-op when nothing changed.
    virtual bool flush() = 0;

    // True if the device stopped answering on the last flush.
    virtual bool faulted() const = 0;
};

// ---------------------------------------------------------------------------
// Semtech SX1503, 16-channel I2C GPIO expander, fixed address 0x20.
//
// Register pairs are ordered bank B then bank A, so a 16-bit write starting
// at the B register with auto-increment lands IO15..8 first and IO7..0
// second. The driver keeps bit 0 = IO0 and swaps on the wire.
// ---------------------------------------------------------------------------

#define SX1503_I2C_ADDRESS      0x20
#define SX1503_REG_DATA_B       0x00    // RegDataB, then RegDataA at 0x01
#define SX1503_REG_DIR_B        0x02    // RegDirB, then RegDirA at 0x03
#define SX1503_REG_PULLUP_B     0x04
#define SX1503_REG_PULLDOWN_B   0x06
#define SX1503_REG_ADVANCED     0xAD    // bit 7: auto-increment, 0 = enabled

class SX1503LampDriver : public LampDriver {
public:
    SX1503LampDriver(arduino::HardwareI2C &bus, uint8_t address = SX1503_I2C_ADDRESS)
        : _bus(bus), _address(address) { }

    bool begin() override;
    uint16_t channels() const override { return 16; }
    bool hasIntensity() const override { return false; }
    uint8_t resolutionBits() const override { return 1; }
    void setChannel(uint16_t channel, uint16_t level) override;
    bool flush() override;
    bool faulted() const override { return _faulted; }

    // Level at or above this counts as on. Default is mid scale.
    void setThreshold(uint16_t t) { _threshold = t; }

    // Set if the IO sinks the LED current, so a 1 bit means the lamp is off.
    void setActiveLow(bool b) { _activeLow = b; }

private:
    arduino::HardwareI2C &_bus;
    uint8_t _address;
    uint16_t _state = 0;            // staged bit pattern, bit 0 = IO0
    bool _dirty = true;
    bool _faulted = false;
    uint16_t _threshold = 0x8000;
    bool _activeLow = false;
};
