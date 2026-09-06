/*
    BitBangWire - a software I2C master for arduino-pico, derived from
                  arduino::HardwareI2C so it is interchangeable with Wire
                  and with PIOWire.

    No PIO, no hardware peripheral, any two GPIOs. Slower than the other two
    but it does support a genuine repeated start, which the PIO wrapper
    currently does not.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include <api/HardwareI2C.h>

#ifndef BITBANGWIRE_BUFFER_SIZE
#define BITBANGWIRE_BUFFER_SIZE 64
#endif

class BitBangWire : public arduino::HardwareI2C {
public:
    BitBangWire(pin_size_t sda, pin_size_t scl);

    bool setSDA(pin_size_t pin);
    bool setSCL(pin_size_t pin);

    void begin() override;
    void begin(uint8_t address) override;       // slave mode: not supported
    void end() override;
    void setClock(uint32_t freq) override;

    void beginTransmission(uint8_t address) override;
    uint8_t endTransmission(bool stopBit) override;
    uint8_t endTransmission() override;

    size_t requestFrom(uint8_t address, size_t len, bool stopBit) override;
    size_t requestFrom(uint8_t address, size_t len) override;

    void onReceive(void (*)(int)) override { }
    void onRequest(void (*)(void)) override { }

    size_t write(uint8_t data) override;
    size_t write(const uint8_t *buf, size_t len) override;
    int available() override;
    int read() override;
    int peek() override;
    void flush() override { }
    using Print::write;

    bool running() const { return _running; }

private:
    void sdaHigh();
    void sdaLow();
    bool sdaRead();
    bool sclHigh();                 // false on clock-stretch timeout
    void sclLow();
    void halfBit();

    bool startCond();
    bool repStartCond();
    void stopCond();
    bool writeByte(uint8_t b);      // returns ACK
    uint8_t readByte(bool ack);

    pin_size_t _sda;
    pin_size_t _scl;
    uint32_t _freq = 400000;
    uint32_t _halfCycles = 1;
    bool _running = false;

    uint8_t _addr = 0;
    bool _inTx = false;
    bool _pendingRestart = false;

    uint8_t _txBuf[BITBANGWIRE_BUFFER_SIZE];
    size_t _txLen = 0;
    uint8_t _rxBuf[BITBANGWIRE_BUFFER_SIZE];
    size_t _rxLen = 0;
    size_t _rxPos = 0;
};
