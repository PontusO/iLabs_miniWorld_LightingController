/*
    Lamps - the application-facing lamp API.

    Nothing below this line is visible to the user: not I2C, not PIO, not bit
    banging, not device registers, not which lamp lives on which bus. A lamp is
    an index from 0 to count()-1 with an intensity.

        Lamps.begin();               // applies the stored configuration
        Lamps.setIntensity(37, 128);
        Lamps.show();

    Which hardware is fitted is a runtime setting (LampConfig), stored in
    flash and editable through the web GUI. The set of devices is built when
    a configuration is applied and torn down when it is replaced.

    Intensity is stored at 16 bits regardless of what the hardware renders.
    An SX1503 quantises it to on and off, an AL5887 renders 12 bits of it.

    Invector Embedded Systems AB
*/

#pragma once

#include <Arduino.h>
#include "LampConfig.h"

class LampDriver;

#define LAMPS_MAX_DEVICES 12
#define LAMPS_MAX_LAMPS   512

class LampController {
public:
    // --- lifecycle -------------------------------------------------------

    // Load the stored configuration and apply it. With nothing stored, the
    // system comes up with zero lamps and waits for the GUI.
    bool begin();

    // Apply a configuration now. With persist, it is also written to flash
    // and becomes the boot configuration. Returns false if a device did not
    // answer, in which case the rest still run.
    bool apply(const LampConfig &cfg, bool persist);

    // Tear everything down: devices, buses, state machines.
    void end();

    // Currently applied configuration.
    const LampConfig &config() const { return _cfg; }

    // Look for hardware. Tries AL5887 at 0x30..0x33 on the primary bus, then
    // SX1503 at 0x20 on every bus in order. Returns what it found; does not
    // apply it. Stops the running configuration while it looks, then
    // restores it.
    LampConfig probe();

    // --- status ----------------------------------------------------------

    uint16_t count() const { return _count; }
    uint8_t deviceCount() const { return _numDev; }
    bool deviceFaulted(uint8_t device) const;
    bool intensitySupported() const;
    uint8_t resolutionBits(uint16_t lamp) const;
    bool faulted(uint16_t lamp) const;

    // --- lamps -----------------------------------------------------------

    void setIntensity(uint16_t lamp, uint8_t level);
    uint8_t intensity(uint16_t lamp) const;

    void setIntensity16(uint16_t lamp, uint16_t level);
    uint16_t intensity16(uint16_t lamp) const;

    void on(uint16_t lamp) { setIntensity16(lamp, 0xFFFF); }
    void off(uint16_t lamp) { setIntensity16(lamp, 0); }
    void toggle(uint16_t lamp) { setIntensity16(lamp, intensity16(lamp) ? 0 : 0xFFFF); }
    void setAll(uint8_t level) { setAll16((uint16_t)(level * 257)); }
    void setAll16(uint16_t level);
    void allOff() { setAll16(0); }

    bool show();
    void setAutoShow(bool enable) { _autoShow = enable; }

private:
    bool build(const LampConfig &cfg);
    void teardown();
    bool resolve(uint16_t lamp, LampDriver **driver, uint16_t *channel) const;

    LampConfig _cfg;
    LampDriver *_dev[LAMPS_MAX_DEVICES] = { nullptr };
    uint8_t _numDev = 0;
    uint16_t _count = 0;
    bool _autoShow = false;
    bool _built = false;
};

extern LampController Lamps;
