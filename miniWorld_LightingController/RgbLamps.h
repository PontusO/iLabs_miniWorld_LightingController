/*
    RgbLamps - a view over the Lamps API for when the AL5887 channels are
               wired as 12 RGB modules rather than 36 single colour lamps.

    Header only, and deliberately so. It owns no state and touches no
    hardware. Module n is lamps 3n, 3n+1, 3n+2 in the order the channels are
    wired, which is the only thing you configure here.

    Invector Embedded Systems AB
*/

#pragma once

#include "Lamps.h"

// Channel order within a module. Change to suit the schematic.
#ifndef RGB_OFFSET_R
#define RGB_OFFSET_R 0
#endif
#ifndef RGB_OFFSET_G
#define RGB_OFFSET_G 1
#endif
#ifndef RGB_OFFSET_B
#define RGB_OFFSET_B 2
#endif

class RgbLampView {
public:
    uint16_t count() const { return (uint16_t)(Lamps.count() / 3); }

    void setColor(uint16_t module, uint8_t r, uint8_t g, uint8_t b) {
        uint16_t base = (uint16_t)(module * 3);
        Lamps.setIntensity((uint16_t)(base + RGB_OFFSET_R), r);
        Lamps.setIntensity((uint16_t)(base + RGB_OFFSET_G), g);
        Lamps.setIntensity((uint16_t)(base + RGB_OFFSET_B), b);
    }

    // Full 12-bit reach, for smooth fades at the bottom of the range where
    // 8 bits visibly steps.
    void setColor16(uint16_t module, uint16_t r, uint16_t g, uint16_t b) {
        uint16_t base = (uint16_t)(module * 3);
        Lamps.setIntensity16((uint16_t)(base + RGB_OFFSET_R), r);
        Lamps.setIntensity16((uint16_t)(base + RGB_OFFSET_G), g);
        Lamps.setIntensity16((uint16_t)(base + RGB_OFFSET_B), b);
    }

    void setColor(uint16_t module, uint32_t rgb) {
        setColor(module,
                 (uint8_t)(rgb >> 16),
                 (uint8_t)(rgb >> 8),
                 (uint8_t)(rgb));
    }

    void off(uint16_t module) { setColor16(module, 0, 0, 0); }

    void show() { Lamps.show(); }
};

static RgbLampView RgbLamps;
