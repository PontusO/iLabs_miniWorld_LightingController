/*
    SceneEngine - see SceneEngine.h

    Invector Embedded Systems AB
*/

#include "SceneEngine.h"
#include "Sun.h"

#include <time.h>
#include <string.h>

#define TICK_MS         25          // 40 Hz is plenty for fades
#define FLICKER_MS      110         // television "frame" rate
#define JITTER_FRACTION 0.15f       // daily wobble as a fraction of the window

SceneEngine Scene;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool SceneEngine::begin() {
    SceneConfig cfg;
    bool had = SceneStore::load(cfg);
    apply(had ? cfg : SceneConfig(), false);
    return had;
}

bool SceneEngine::apply(const SceneConfig &in, bool persist) {
    _cfg = in;
    _cfg.clamp();
    rebuild();
    if (persist) {
        return SceneStore::save(_cfg);
    }
    return true;
}

void SceneEngine::rebuild() {
    memset(_lampGroup, 0xFF, sizeof(_lampGroup));
    for (uint8_t g = 0; g < _cfg.groupCount; g++) {
        for (uint16_t l = 0; l < LAMPS_MAX_LAMPS; l++) {
            if (_cfg.groups[g].has(l)) {
                _lampGroup[l] = g;      // later groups win on overlap
            }
        }
    }
    _sunForDoy = -1;
    _startMs = millis();
    _lastTickMs = 0;
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

void SceneEngine::setMode(ClockMode m) {
    if (m == ClockMode::Accelerated && _cfg.mode != ClockMode::Accelerated) {
        // Start the accelerated day where the clock currently is, so a
        // switch from real time does not jump.
        uint32_t dayMs = (uint32_t)_cfg.dayMinutes * 60000UL;
        _startMs = millis() - (uint32_t)(((uint64_t)_sim * dayMs) / 1440);
    }
    _cfg.mode = m;
}

void SceneEngine::setManualTime(uint16_t minutes) {
    _cfg.manualTime = (uint16_t)(minutes % 1440);
    if (_cfg.mode == ClockMode::Accelerated) {
        uint32_t dayMs = (uint32_t)_cfg.dayMinutes * 60000UL;
        _startMs = millis() - (uint32_t)(((uint64_t)_cfg.manualTime * dayMs) / 1440);
    }
}

void SceneEngine::setDayMinutes(uint16_t minutes) {
    if (minutes < 1) minutes = 1;
    // Keep the current sim time when the speed changes.
    uint16_t sim = _sim;
    _cfg.dayMinutes = minutes;
    setManualTime(sim);
}

void SceneEngine::setDayOfYear(uint16_t doy) {
    _cfg.dateFromSystem = false;
    _cfg.dayOfYear = doy < 1 ? 1 : (doy > 366 ? 366 : doy);
    _sunForDoy = -1;
}

// Difference between local time and UTC as the system sees it. Relies on
// the product having set TZ (e.g. "CET-1CEST,M3.5.0,M10.5.0/3") and on
// the clock having been set from NTP.
int SceneEngine::localOffsetMinutes() const {
    if (!_cfg.autoTimezone) {
        return _cfg.utcOffsetMinutes;
    }
    time_t now = time(nullptr);
    if (now < 100000) {
        return _cfg.utcOffsetMinutes;
    }
    struct tm lt, gt;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gt);
    gt.tm_isdst = lt.tm_isdst;
    return (int)(difftime(mktime(&lt), mktime(&gt)) / 60.0);
}

void SceneEngine::updateClock(uint32_t nowMs) {
    time_t now = time(nullptr);
    struct tm lt;
    bool haveTime = (now >= 100000);
    if (haveTime) {
        localtime_r(&now, &lt);
    }

    // Date
    if (_cfg.dateFromSystem && haveTime) {
        _doy = (uint16_t)(lt.tm_yday + 1);
    } else {
        _doy = _cfg.dayOfYear;
    }

    // Time of day
    switch (_cfg.mode) {
        case ClockMode::Real:
            if (haveTime) {
                _sim = (uint16_t)(lt.tm_hour * 60 + lt.tm_min);
                _clockValid = true;
            } else {
                _sim = _cfg.manualTime;     // no NTP yet: hold still
                _clockValid = false;
            }
            break;

        case ClockMode::Accelerated: {
            uint32_t dayMs = (uint32_t)_cfg.dayMinutes * 60000UL;
            uint32_t elapsed = (nowMs - _startMs) % dayMs;
            _sim = (uint16_t)(((uint64_t)elapsed * 1440) / dayMs);
            _clockValid = true;
            break;
        }

        case ClockMode::Manual:
            _sim = _cfg.manualTime;
            _clockValid = true;
            break;
    }

    if ((int)_doy != _sunForDoy) {
        recomputeSun();
    }
}

void SceneEngine::recomputeSun() {
    int tz = localOffsetMinutes();
    _dusk = Sun::civilDusk(_doy, _cfg.latitude, _cfg.longitude, tz);
    _dawn = Sun::civilDawn(_doy, _cfg.latitude, _cfg.longitude, tz);
    _sunset = Sun::sunset(_doy, _cfg.latitude, _cfg.longitude, tz);
    _sunrise = Sun::sunrise(_doy, _cfg.latitude, _cfg.longitude, tz);

    // Far north in summer there is no civil darkness. The model town still
    // wants a night, so give it a short one.
    if (_dusk < 0 || _dawn < 0) {
        _dusk = 23 * 60;
        _dawn = 3 * 60;
    }
    if (_sunset < 0) _sunset = _dusk - 30;
    if (_sunrise < 0) _sunrise = _dawn + 30;

    _sunForDoy = _doy;
}

// ---------------------------------------------------------------------------
// Habits
// ---------------------------------------------------------------------------

// Murmur3 finaliser over two words. Cheap, well mixed, deterministic.
uint32_t SceneEngine::hash(uint32_t a, uint32_t b) const {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) ^ _cfg.seed;
    h ^= h >> 16; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

// Stable per-lamp value in [0, 1) for a given purpose.
float SceneEngine::unit(uint16_t lamp, uint32_t salt) const {
    return (float)(hash(lamp, salt) >> 8) / 16777216.0f;
}

// Per-lamp, per-day value in [0, 1). Same lamp, different evenings.
float SceneEngine::dayUnit(uint16_t lamp, uint32_t salt) const {
    return (float)(hash(lamp + 0x10000u * _doy, salt) >> 8) / 16777216.0f;
}

int SceneEngine::anchorBase(Anchor a, bool forOff) const {
    switch (a) {
        case Anchor::Dusk: return _dusk;
        case Anchor::Dawn: return forOff ? _dawn + 1440 : _dawn;
        default:           return 0;
    }
}

// t is 0..1439. on and off are minutes on a 0..2880 line where values past
// 1440 mean "tomorrow". A lamp is lit if the current time, or the current
// time viewed as tomorrow, falls inside [on, off).
bool SceneEngine::inWindow(int t, int on, int off) {
    return (t >= on && t < off) || (t + 1440 >= on && t + 1440 < off);
}

static inline int lerp(int a, int b, float u) {
    return a + (int)((b - a) * u + 0.5f);
}

static inline float clamp01(float v) {
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

uint16_t SceneEngine::targetLevel(uint16_t lamp, const GroupConfig &G, bool &flicker) {
    flicker = false;
    if (G.behaviour == Behaviour::Off || G.litPercent == 0) {
        return 0;
    }

    // Does this lamp take part at all? (Empty flat, shop closed for good.)
    if (unit(lamp, 1) * 100.0f >= G.litPercent) {
        return 0;
    }

    // Personal moment inside each window, plus a daily wobble.
    float jitterOn = (dayUnit(lamp, 11) - 0.5f) * JITTER_FRACTION;
    float jitterOff = (dayUnit(lamp, 12) - 0.5f) * JITTER_FRACTION;

    int on = anchorBase(G.onAnchor, false) +
             lerp(G.onFrom, G.onTo, clamp01(unit(lamp, 2) + jitterOn));
    int off = anchorBase(G.offAnchor, true) +
              lerp(G.offFrom, G.offTo, clamp01(unit(lamp, 3) + jitterOff));

    if (off <= on) {
        off += 1440;
    }

    bool lit = inWindow(_sim, on, off);

    // Winter mornings: some households are up before it is light.
    if (!lit && G.morning && unit(lamp, 4) < 0.7f) {
        int mOn = lerp(6 * 60 + 15, 7 * 60 + 45, unit(lamp, 5));
        int mOff = _dawn + 15;
        if (mOff < mOn + 45) {
            mOff = mOn + 45;
        }
        lit = (_sim >= mOn && _sim < mOff);
    }

    if (!lit) {
        return 0;
    }

    flicker = (G.flickerPercent > 0) && (unit(lamp, 6) * 100.0f < G.flickerPercent);
    return (uint16_t)(G.level * 257);
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void SceneEngine::tick() {
    if (!_enabled) {
        return;
    }
    uint32_t nowMs = millis();
    if (nowMs - _lastTickMs < TICK_MS) {
        return;
    }
    uint32_t dt = _lastTickMs ? (nowMs - _lastTickMs) : TICK_MS;
    _lastTickMs = nowMs;

    updateClock(nowMs);

    bool flickerFrame = (nowMs - _lastFlickerMs) >= FLICKER_MS;
    if (flickerFrame) {
        _lastFlickerMs = nowMs;
    }

    uint16_t count = Lamps.count();
    bool changed = false;
    uint16_t lit = 0;

    for (uint16_t lamp = 0; lamp < count; lamp++) {
        uint8_t g = _lampGroup[lamp];
        if (g == 0xFF) {
            continue;                   // not in the scene: leave it alone
        }
        const GroupConfig &G = _cfg.groups[g];

        bool flicker;
        uint16_t base = targetLevel(lamp, G, flicker);
        if (base) {
            lit++;
        }

        uint16_t target = base;
        if (flicker && base) {
            // A television: dim, restless, mostly blue but we only have one
            // channel, so restless will have to do.
            uint32_t word = lamp >> 5;
            uint32_t bit = 1u << (lamp & 31);
            if (flickerFrame || !(_flickerNext[word] & bit)) {
                _rng ^= _rng << 13; _rng ^= _rng >> 17; _rng ^= _rng << 5;
                uint32_t n = _rng & 0xFF;
                _target[lamp] = (uint16_t)((uint32_t)base * (90 + n * 90 / 255) / 255);
                _flickerNext[word] |= bit;
            }
            target = _target[lamp];
        } else {
            _target[lamp] = target;
        }

        // Fade toward target in real time. Flicker uses a fast fixed fade so
        // it looks like picture changes rather than a dimmer being turned.
        uint16_t fadeMs = flicker ? 80 : G.fadeMs;
        uint16_t cur = _current[lamp];
        if (cur != target) {
            if (fadeMs == 0) {
                cur = target;
            } else {
                uint32_t step = (65535UL * dt) / fadeMs;
                if (step == 0) step = 1;
                if (target > cur) {
                    cur = (uint16_t)((cur + step > target) ? target : cur + step);
                } else {
                    cur = (uint16_t)((cur - step < target || cur < step) ? target : cur - step);
                }
            }
            _current[lamp] = cur;
            Lamps.setIntensity16(lamp, cur);
            changed = true;
        }
    }

    _lit = lit;
    if (changed) {
        Lamps.show();
    }
}
