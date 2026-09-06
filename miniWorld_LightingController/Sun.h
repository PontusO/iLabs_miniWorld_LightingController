/*
    Sun - when does it get dark, and when does it get light again.

    Standard low-precision solar position algorithm, good to a couple of
    minutes, which is more than adequate for deciding when the tiny street
    lights come on. Results are minutes since local midnight.

    Invector Embedded Systems AB
*/

#pragma once

#include <stdint.h>

namespace Sun {

    // Sun centre 0.833 degrees below the horizon: the usual sunrise/sunset.
    constexpr float ZENITH_OFFICIAL = 90.833f;

    // Sun 6 degrees below: civil twilight. Street lights come on around here.
    constexpr float ZENITH_CIVIL = 96.0f;

    // Return values that are not times:
    constexpr int NEVER_ABOVE = -1;     // sun never reaches this height that day
    constexpr int NEVER_BELOW = -2;     // sun never drops to this depth that day

    // dayOfYear 1..366, latitude/longitude in degrees (east positive),
    // utcOffsetMinutes for the local clock. rise=true for the morning event.
    int eventMinutes(int dayOfYear, float latitude, float longitude,
                     int utcOffsetMinutes, float zenith, bool rise);

    inline int sunrise(int doy, float lat, float lon, int tz) {
        return eventMinutes(doy, lat, lon, tz, ZENITH_OFFICIAL, true);
    }
    inline int sunset(int doy, float lat, float lon, int tz) {
        return eventMinutes(doy, lat, lon, tz, ZENITH_OFFICIAL, false);
    }
    inline int civilDawn(int doy, float lat, float lon, int tz) {
        return eventMinutes(doy, lat, lon, tz, ZENITH_CIVIL, true);
    }
    inline int civilDusk(int doy, float lat, float lon, int tz) {
        return eventMinutes(doy, lat, lon, tz, ZENITH_CIVIL, false);
    }
}
