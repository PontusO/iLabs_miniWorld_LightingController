/*
    AL5887LampDriver - Diodes AL5887 / AL5887Q, 36 linear current channels,
                       12-bit PWM per channel at 30 kHz, I2C or SPI.

    Used here as 36 independent single-colour lamps. If you are driving 12 RGB
    modules instead, the mapping is unchanged: lamps 3n, 3n+1 and 3n+2 are the
    R, G and B of module n. Put that view on top of the Lamps API, not here.

    THE REGISTER MAP BELOW IS NOT CONFIRMED. Everything part-specific is in the
    block of defines at the top of this file, deliberately, so that correcting
    it against the datasheet is a single edit and touches nothing else. Check:

      - the address list (0x30..0x33 come from the Diodes evaluation board
        user guide, not from the datasheet register section)
      - AL5887_REG_PWM_BASE and whether the channel PWM registers really do
        auto-increment across the whole block
      - AL5887_PWM_BYTES_PER_CHANNEL and AL5887_PWM_LSB_FIRST, i.e. how the
        12 bits are packed into the register pair, and whether they are left
        or right justified (see AL5887_PWM_SHIFT)
      - the enable bit in AL5887_REG_DEVICE_CONFIG0

    Hardware notes that are not this driver's problem but will stop it working:
      - INT_SEL must be tied low to select I2C
      - RSTn has an internal pull-up. Held low for 1 to 20 ms it resets only
        the digital interface and keeps register contents. Held low for more
        than 20 ms it clears all registers. Wire it to a GPIO if you want
        recovery without a power cycle, then use setResetPin().
      - global output current for all 36 channels is set by the external RSET
        resistor, not in software
      - the device drops to a low power mode when every channel has been off
        for more than 30 ms, so the first write after an idle period may want
        a little slack in your timing

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include <api/HardwareI2C.h>
#include "LampDriver.h"

// --- part specific, CONFIRM ------------------------------------------------

#ifndef AL5887_REG_DEVICE_CONFIG0
#define AL5887_REG_DEVICE_CONFIG0   0x00
#endif
#ifndef AL5887_CHIP_EN_BIT
#define AL5887_CHIP_EN_BIT          0x40
#endif
#ifndef AL5887_REG_DEVICE_CONFIG1
#define AL5887_REG_DEVICE_CONFIG1   0x01
#endif
#ifndef AL5887_REG_RESET
#define AL5887_REG_RESET            0x38
#endif
#ifndef AL5887_RESET_VALUE
#define AL5887_RESET_VALUE          0xFF
#endif

// First of the per-channel PWM registers. Channel n starts at
// AL5887_REG_PWM_BASE + n * AL5887_PWM_BYTES_PER_CHANNEL.
#ifndef AL5887_REG_PWM_BASE
#define AL5887_REG_PWM_BASE         0x0B
#endif
#ifndef AL5887_PWM_BYTES_PER_CHANNEL
#define AL5887_PWM_BYTES_PER_CHANNEL 2
#endif
#ifndef AL5887_PWM_LSB_FIRST
#define AL5887_PWM_LSB_FIRST        1
#endif
// How far to shift a 12-bit value left inside the 16-bit register pair.
// 0 for right justified, 4 for left justified.
#ifndef AL5887_PWM_SHIFT
#define AL5887_PWM_SHIFT            0
#endif

// --- tunable ---------------------------------------------------------------

// Largest payload in one I2C transaction, excluding the register byte. Kept
// below the 64 byte transmit buffers in PIOWire and BitBangWire so that a
// full 36-channel refresh is chunked rather than truncated.
#ifndef AL5887_TX_CHUNK
#define AL5887_TX_CHUNK             60
#endif

#define AL5887_CHANNELS             36

class AL5887LampDriver : public LampDriver {
public:
    AL5887LampDriver(arduino::HardwareI2C &bus, uint8_t address)
        : _bus(bus), _address(address) { }

    bool begin() override;
    uint16_t channels() const override { return AL5887_CHANNELS; }
    bool hasIntensity() const override { return true; }
    uint8_t resolutionBits() const override { return 12; }
    void setChannel(uint16_t channel, uint16_t level) override;
    bool flush() override;
    bool faulted() const override { return _faulted; }

    // Optional: RSTn wired to a GPIO. Call before begin().
    void setResetPin(pin_size_t pin) { _resetPin = pin; }

    // Full register reset via RSTn if wired, otherwise via the reset register.
    bool reset();

    // Force the next flush to rewrite every channel.
    void invalidate();

private:
    bool writeReg(uint8_t reg, uint8_t value);
    bool writeSpan(uint16_t first, uint16_t last);

    arduino::HardwareI2C &_bus;
    uint8_t _address;
    pin_size_t _resetPin = 0xFF;

    uint16_t _pwm[AL5887_CHANNELS] = { 0 };     // 12-bit, right justified

    // Inclusive dirty span. _dirtyLo > _dirtyHi means nothing to do.
    uint16_t _dirtyLo = 0;
    uint16_t _dirtyHi = AL5887_CHANNELS - 1;

    bool _faulted = false;
};
