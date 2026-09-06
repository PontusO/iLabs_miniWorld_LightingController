/*
    BitBangWire - see BitBangWire.h

    Invector Embedded Systems AB
*/

#include "BitBangWire.h"

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <pico/time.h>

// How long to wait for a slave to release SCL before giving up
#define BBW_STRETCH_TIMEOUT_US 1000

BitBangWire::BitBangWire(pin_size_t sda, pin_size_t scl) : _sda(sda), _scl(scl) {
}

bool BitBangWire::setSDA(pin_size_t pin) {
    if (_running) {
        return false;
    }
    _sda = pin;
    return true;
}

bool BitBangWire::setSCL(pin_size_t pin) {
    if (_running) {
        return false;
    }
    _scl = pin;
    return true;
}

// Open drain: the output register is parked at 0 permanently, and the line is
// driven by flipping the direction. Input direction releases it to the pull-up.
inline void BitBangWire::sdaHigh() {
    gpio_set_dir(_sda, GPIO_IN);
}

inline void BitBangWire::sdaLow() {
    gpio_set_dir(_sda, GPIO_OUT);
}

inline bool BitBangWire::sdaRead() {
    return gpio_get(_sda);
}

inline void BitBangWire::sclLow() {
    gpio_set_dir(_scl, GPIO_OUT);
}

inline void BitBangWire::halfBit() {
    busy_wait_at_least_cycles(_halfCycles);
}

// Release SCL and wait for it to actually rise, honouring clock stretching.
bool BitBangWire::sclHigh() {
    gpio_set_dir(_scl, GPIO_IN);
    absolute_time_t deadline = make_timeout_time_us(BBW_STRETCH_TIMEOUT_US);
    while (!gpio_get(_scl)) {
        if (time_reached(deadline)) {
            return false;
        }
    }
    return true;
}

void BitBangWire::begin() {
    if (_running) {
        return;
    }
    if (_sda > 29 || _scl > 29 || _sda == _scl) {
        return;
    }

    gpio_init(_sda);
    gpio_init(_scl);
    gpio_put(_sda, 0);
    gpio_put(_scl, 0);
    gpio_set_dir(_sda, GPIO_IN);
    gpio_set_dir(_scl, GPIO_IN);
    gpio_pull_up(_sda);
    gpio_pull_up(_scl);

    setClock(_freq);
    _running = true;

    // Recover a slave that is holding SDA low from an aborted transaction
    if (!sdaRead()) {
        for (int i = 0; i < 9; i++) {
            sclLow();
            halfBit();
            sclHigh();
            halfBit();
        }
        stopCond();
    }
}

void BitBangWire::begin(uint8_t address) {
    (void)address;
}

void BitBangWire::end() {
    if (!_running) {
        return;
    }
    gpio_set_dir(_sda, GPIO_IN);
    gpio_set_dir(_scl, GPIO_IN);
    _running = false;
    _inTx = false;
    _pendingRestart = false;
    _txLen = _rxLen = _rxPos = 0;
}

void BitBangWire::setClock(uint32_t freq) {
    if (freq == 0) {
        freq = 100000;
    }
    _freq = freq;
    // Two half-bits per clock period, minus the loop overhead we cannot model.
    uint32_t cycles = clock_get_hz(clk_sys) / (2 * freq);
    _halfCycles = (cycles > 8) ? (cycles - 8) : 1;
}

// ---------------------------------------------------------------------------
// Bus primitives
// ---------------------------------------------------------------------------

bool BitBangWire::startCond() {
    sdaHigh();
    if (!sclHigh()) {
        return false;
    }
    halfBit();
    sdaLow();
    halfBit();
    sclLow();
    halfBit();
    return true;
}

bool BitBangWire::repStartCond() {
    sdaHigh();
    halfBit();
    if (!sclHigh()) {
        return false;
    }
    halfBit();
    sdaLow();
    halfBit();
    sclLow();
    halfBit();
    return true;
}

void BitBangWire::stopCond() {
    sdaLow();
    halfBit();
    sclHigh();
    halfBit();
    sdaHigh();
    halfBit();
}

bool BitBangWire::writeByte(uint8_t b) {
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) {
            sdaHigh();
        } else {
            sdaLow();
        }
        b <<= 1;
        halfBit();
        if (!sclHigh()) {
            return false;
        }
        halfBit();
        sclLow();
    }

    // Ninth clock: release SDA and sample the slave's ACK
    sdaHigh();
    halfBit();
    if (!sclHigh()) {
        return false;
    }
    bool ack = !sdaRead();
    halfBit();
    sclLow();
    return ack;
}

uint8_t BitBangWire::readByte(bool ack) {
    uint8_t b = 0;
    sdaHigh();
    for (int i = 0; i < 8; i++) {
        halfBit();
        if (!sclHigh()) {
            return b;
        }
        b = (uint8_t)((b << 1) | (sdaRead() ? 1 : 0));
        halfBit();
        sclLow();
    }

    // Ninth clock: we drive ACK or leave the line high for NAK
    if (ack) {
        sdaLow();
    } else {
        sdaHigh();
    }
    halfBit();
    sclHigh();
    halfBit();
    sclLow();
    sdaHigh();
    return b;
}

// ---------------------------------------------------------------------------
// Master transactions
// ---------------------------------------------------------------------------

void BitBangWire::beginTransmission(uint8_t address) {
    _addr = address;
    _txLen = 0;
    _inTx = true;
}

uint8_t BitBangWire::endTransmission(bool stopBit) {
    if (!_running || !_inTx) {
        return 4;
    }
    _inTx = false;

    if (!_pendingRestart) {
        if (!startCond()) {
            return 4;
        }
    } else if (!repStartCond()) {
        _pendingRestart = false;
        return 4;
    }
    _pendingRestart = false;

    if (!writeByte((uint8_t)(_addr << 1))) {
        stopCond();
        _txLen = 0;
        return 2;               // address NAK
    }

    for (size_t i = 0; i < _txLen; i++) {
        if (!writeByte(_txBuf[i])) {
            stopCond();
            _txLen = 0;
            return 3;           // data NAK
        }
    }

    _txLen = 0;
    if (stopBit) {
        stopCond();
    } else {
        _pendingRestart = true; // leave the bus held for a repeated start
    }
    return 0;
}

uint8_t BitBangWire::endTransmission() {
    return endTransmission(true);
}

size_t BitBangWire::requestFrom(uint8_t address, size_t len, bool stopBit) {
    _rxLen = 0;
    _rxPos = 0;

    if (!_running || len == 0) {
        return 0;
    }
    if (len > BITBANGWIRE_BUFFER_SIZE) {
        len = BITBANGWIRE_BUFFER_SIZE;
    }

    bool ok = _pendingRestart ? repStartCond() : startCond();
    _pendingRestart = false;
    if (!ok) {
        return 0;
    }

    if (!writeByte((uint8_t)((address << 1) | 1))) {
        stopCond();
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        _rxBuf[i] = readByte(i < (len - 1));    // NAK the last byte
    }

    if (stopBit) {
        stopCond();
    } else {
        _pendingRestart = true;
    }

    _rxLen = len;
    return len;
}

size_t BitBangWire::requestFrom(uint8_t address, size_t len) {
    return requestFrom(address, len, true);
}

// ---------------------------------------------------------------------------
// Stream / Print
// ---------------------------------------------------------------------------

size_t BitBangWire::write(uint8_t data) {
    if (!_inTx || _txLen >= BITBANGWIRE_BUFFER_SIZE) {
        return 0;
    }
    _txBuf[_txLen++] = data;
    return 1;
}

size_t BitBangWire::write(const uint8_t *buf, size_t len) {
    size_t n = 0;
    while (n < len && write(buf[n])) {
        n++;
    }
    return n;
}

int BitBangWire::available() {
    return (int)(_rxLen - _rxPos);
}

int BitBangWire::read() {
    if (_rxPos >= _rxLen) {
        return -1;
    }
    return _rxBuf[_rxPos++];
}

int BitBangWire::peek() {
    if (_rxPos >= _rxLen) {
        return -1;
    }
    return _rxBuf[_rxPos];
}
