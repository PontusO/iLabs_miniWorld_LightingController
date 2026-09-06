/*
    Sun - see Sun.h

    Invector Embedded Systems AB
*/

#include "Sun.h"
#include <math.h>

static inline float d2r(float d) { return d * 0.017453292f; }
static inline float r2d(float r) { return r * 57.29578f; }

static float norm360(float a) {
    while (a < 0) a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return a;
}

static float norm24(float h) {
    while (h < 0) h += 24.0f;
    while (h >= 24.0f) h -= 24.0f;
    return h;
}

int Sun::eventMinutes(int dayOfYear, float lat, float lon,
                      int utcOffsetMinutes, float zenith, bool rise) {
    float lngHour = lon / 15.0f;
    float t = dayOfYear + (((rise ? 6.0f : 18.0f) - lngHour) / 24.0f);

    // Sun's mean anomaly and true longitude
    float M = 0.9856f * t - 3.289f;
    float L = norm360(M + 1.916f * sinf(d2r(M)) + 0.020f * sinf(d2r(2.0f * M)) + 282.634f);

    // Right ascension, forced into the same quadrant as L, in hours
    float RA = norm360(r2d(atanf(0.91764f * tanf(d2r(L)))));
    float Lq = floorf(L / 90.0f) * 90.0f;
    float RAq = floorf(RA / 90.0f) * 90.0f;
    RA = (RA + (Lq - RAq)) / 15.0f;

    // Declination
    float sinDec = 0.39782f * sinf(d2r(L));
    float cosDec = cosf(asinf(sinDec));

    // Local hour angle
    float cosH = (cosf(d2r(zenith)) - sinDec * sinf(d2r(lat))) / (cosDec * cosf(d2r(lat)));
    if (cosH > 1.0f) {
        return NEVER_ABOVE;
    }
    if (cosH < -1.0f) {
        return NEVER_BELOW;
    }

    float H = rise ? (360.0f - r2d(acosf(cosH))) : r2d(acosf(cosH));
    H /= 15.0f;

    float T = H + RA - 0.06571f * t - 6.622f;
    float local = norm24(norm24(T - lngHour) + utcOffsetMinutes / 60.0f);

    return (int)(local * 60.0f + 0.5f);
}
