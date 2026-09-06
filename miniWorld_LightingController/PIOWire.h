/*
    PIOWire - a Wire-compatible I2C master running on an RP2040/RP2350 PIO
              state machine, for arduino-pico.

    Wraps the PIO I2C implementation from pico-examples (pio/i2c) in a class
    derived from arduino::HardwareI2C, so that any library which accepts a
    TwoWire& or HardwareI2C& reference can talk to a PIO bus unchanged.

    Unlike the stock i2c_program_init(), SDA and SCL do NOT have to be
    adjacent GPIOs. The PIO program reaches SDA through the out/set/in/jmp
    pin selectors and SCL through side-set, all independently configurable,
    and the pin setup uses full 32-bit masks. The assert in the example is
    the only thing enforcing adjacency.

    Requires two files copied from pico-examples/pio/i2c into your sketch
    folder or library: i2c.pio and pio_i2c.c / pio_i2c.h (BSD-3-Clause,
    Copyright (c) Raspberry Pi Ltd).

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include <api/HardwareI2C.h>
#include <hardware/pio.h>

#ifndef PIOWIRE_BUFFER_SIZE
#define PIOWIRE_BUFFER_SIZE 64
#endif

class PIOWire : public arduino::HardwareI2C {
public:
    PIOWire(pin_size_t sda, pin_size_t scl);
    ~PIOWire();

    // Must be called before begin()
    bool setSDA(pin_size_t pin);
    bool setSCL(pin_size_t pin);

    // HardwareI2C
    void begin() override;
    void begin(uint8_t address) override;   // slave mode: not supported
    void end() override;
    void setClock(uint32_t freq) override;

    void beginTransmission(uint8_t address) override;
    uint8_t endTransmission(bool stopBit) override;
    uint8_t endTransmission() override;

    size_t requestFrom(uint8_t address, size_t len, bool stopBit) override;
    size_t requestFrom(uint8_t address, size_t len) override;

    void onReceive(void (*)(int)) override { }
    void onRequest(void (*)(void)) override { }

    // Stream / Print
    size_t write(uint8_t data) override;
    size_t write(const uint8_t *buf, size_t len) override;
    int available() override;
    int read() override;
    int peek() override;
    void flush() override { }
    using Print::write;

    // Diagnostics: which PIO and SM this bus ended up on, -1 if not started
    bool running() const { return _running; }
    PIO pio() const { return _pio; }
    int sm() const { return _sm; }

private:
    bool allocate();
    void smInit();
    float clkdivFor(uint32_t freq) const;

    pin_size_t _sda;
    pin_size_t _scl;
    uint32_t _freq = 400000;

    PIO _pio = nullptr;
    int _sm = -1;
    int _offset = -1;
    bool _running = false;

    uint8_t _addr = 0;
    bool _inTx = false;
    bool _pendingWrite = false;   // endTransmission(false) deferred the write

    uint8_t _txBuf[PIOWIRE_BUFFER_SIZE];
    size_t _txLen = 0;
    uint8_t _rxBuf[PIOWIRE_BUFFER_SIZE];
    size_t _rxLen = 0;
    size_t _rxPos = 0;
};
