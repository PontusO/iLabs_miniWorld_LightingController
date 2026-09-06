/*
    AL5887LampDriver - see AL5887LampDriver.h

    Invector Embedded Systems AB
*/

#include "AL5887LampDriver.h"

bool AL5887LampDriver::writeReg(uint8_t reg, uint8_t value) {
    _bus.beginTransmission(_address);
    _bus.write(reg);
    _bus.write(value);
    return _bus.endTransmission() == 0;
}

void AL5887LampDriver::invalidate() {
    _dirtyLo = 0;
    _dirtyHi = AL5887_CHANNELS - 1;
}

bool AL5887LampDriver::reset() {
    if (_resetPin != 0xFF) {
        // Held low for more than 20 ms clears all registers.
        pinMode(_resetPin, OUTPUT);
        digitalWrite(_resetPin, LOW);
        delay(30);
        digitalWrite(_resetPin, HIGH);
        delay(2);
        return true;
    }
    return writeReg(AL5887_REG_RESET, AL5887_RESET_VALUE);
}

bool AL5887LampDriver::begin() {
    if (_resetPin != 0xFF) {
        reset();
    }

    if (!writeReg(AL5887_REG_DEVICE_CONFIG0, AL5887_CHIP_EN_BIT)) {
        _faulted = true;
        return false;
    }

    _faulted = false;
    for (uint16_t i = 0; i < AL5887_CHANNELS; i++) {
        _pwm[i] = 0;
    }
    invalidate();
    return flush();
}

void AL5887LampDriver::setChannel(uint16_t channel, uint16_t level) {
    if (channel >= AL5887_CHANNELS) {
        return;
    }

    // 16 bits of intent down to the 12 the hardware renders.
    uint16_t v = (uint16_t)(level >> 4);

    if (v == _pwm[channel]) {
        return;
    }
    _pwm[channel] = v;

    // Widen the dirty span rather than flagging the whole device.
    if (_dirtyLo > _dirtyHi) {
        _dirtyLo = _dirtyHi = channel;
    } else {
        if (channel < _dirtyLo) {
            _dirtyLo = channel;
        }
        if (channel > _dirtyHi) {
            _dirtyHi = channel;
        }
    }
}

// Write channels [first, last] inclusive as one auto-incrementing burst.
bool AL5887LampDriver::writeSpan(uint16_t first, uint16_t last) {
    uint8_t reg = (uint8_t)(AL5887_REG_PWM_BASE +
                            first * AL5887_PWM_BYTES_PER_CHANNEL);

    _bus.beginTransmission(_address);
    _bus.write(reg);

    for (uint16_t ch = first; ch <= last; ch++) {
        uint16_t v = (uint16_t)(_pwm[ch] << AL5887_PWM_SHIFT);

#if AL5887_PWM_BYTES_PER_CHANNEL == 2
#if AL5887_PWM_LSB_FIRST
        _bus.write((uint8_t)(v & 0xFF));
        _bus.write((uint8_t)(v >> 8));
#else
        _bus.write((uint8_t)(v >> 8));
        _bus.write((uint8_t)(v & 0xFF));
#endif
#else
        // Single byte per channel: the top 8 bits of the 12 survive.
        _bus.write((uint8_t)(_pwm[ch] >> 4));
#endif
    }

    return _bus.endTransmission() == 0;
}

bool AL5887LampDriver::flush() {
    if (_dirtyLo > _dirtyHi) {
        return true;                // nothing staged
    }

    const uint16_t perTx = AL5887_TX_CHUNK / AL5887_PWM_BYTES_PER_CHANNEL;

    uint16_t ch = _dirtyLo;
    while (ch <= _dirtyHi) {
        uint16_t last = (uint16_t)(ch + perTx - 1);
        if (last > _dirtyHi) {
            last = _dirtyHi;
        }

        if (!writeSpan(ch, last)) {
            _faulted = true;
            // Leave the span dirty from here on so a retry picks it up.
            _dirtyLo = ch;
            return false;
        }
        ch = (uint16_t)(last + 1);
    }

    // Mark clean: lo above hi.
    _dirtyLo = 1;
    _dirtyHi = 0;
    _faulted = false;
    return true;
}
