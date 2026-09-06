/*
    SX1503LampDriver - see LampDriver.h

    Invector Embedded Systems AB
*/

#include "LampDriver.h"

// Bank B register comes first in the map and holds IO15..8, so the high byte
// of our IO0-based pattern goes out first.
static bool writeReg16(arduino::HardwareI2C &bus, uint8_t address,
                       uint8_t regB, uint16_t value) {
    bus.beginTransmission(address);
    bus.write(regB);
    bus.write((uint8_t)(value >> 8));       // bank B, IO15..8
    bus.write((uint8_t)(value & 0xFF));     // bank A, IO7..0
    return bus.endTransmission() == 0;
}

bool SX1503LampDriver::begin() {
    // Make sure multi-byte register access auto-increments. Power-on default
    // is enabled, but a warm restart may have left it otherwise.
    _bus.beginTransmission(_address);
    _bus.write(SX1503_REG_ADVANCED);
    _bus.write((uint8_t)0x00);
    if (_bus.endTransmission() != 0) {
        _faulted = true;
        return false;
    }

    // RegDir: 1 = input (power-on default), 0 = output. All 16 to outputs.
    if (!writeReg16(_bus, _address, SX1503_REG_DIR_B, 0x0000)) {
        _faulted = true;
        return false;
    }

    _faulted = false;
    _state = 0;
    _dirty = true;              // force the first flush to write
    return flush();
}

void SX1503LampDriver::setChannel(uint16_t channel, uint16_t level) {
    if (channel >= 16) {
        return;
    }

    // The interface carries 16 bits of intent. This device renders one of them.
    bool on = (level >= _threshold);

    uint16_t mask = (uint16_t)(1u << channel);
    uint16_t next = on ? (uint16_t)(_state | mask) : (uint16_t)(_state & ~mask);

    if (next != _state) {
        _state = next;
        _dirty = true;
    }
}

bool SX1503LampDriver::flush() {
    if (!_dirty && !_faulted) {
        return true;
    }

    uint16_t out = _activeLow ? (uint16_t)~_state : _state;
    if (!writeReg16(_bus, _address, SX1503_REG_DATA_B, out)) {
        _faulted = true;
        return false;
    }

    _dirty = false;
    _faulted = false;
    return true;
}
