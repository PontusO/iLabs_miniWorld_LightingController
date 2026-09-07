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
    // Static: a SceneConfig is about 4.8 kB and the core-0 stack is far
    // smaller. The sketch is single-threaded and begin() runs once, from
    // setup(), so this never nests with itself.
    static SceneConfig cfg;
    bool had = SceneStore::load(cfg);
    // One call rather than a ternary with SceneConfig(): the temporary would
    // put another 4.8 kB on the stack. load() leaves cfg at its defaults when
    // there is nothing stored or the file does not parse.
    apply(cfg, false);
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
    memset(_eventOn, 0, sizeof(_eventOn));
    memset(_eventOff, 0, sizeof(_eventOff));
    _active = 0;
    _eventSim = 0xFFFF;             // no simulated minute: evaluate on next tick
    _eventDoy = 0;

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

// The two personal moments and the morning light, on their own. The activity
// layer needs the off moment even for a lamp that is dark or that never takes
// part, so this runs before targetLevel()'s early exits. The arithmetic is
// the arithmetic targetLevel() has always done, moved and not changed.
void SceneEngine::lampMoments(uint16_t lamp, const GroupConfig &G, LampMoments &m) const {
    // Personal moment inside each window, plus a daily wobble.
    float jitterOn = (dayUnit(lamp, 11) - 0.5f) * JITTER_FRACTION;
    float jitterOff = (dayUnit(lamp, 12) - 0.5f) * JITTER_FRACTION;

    m.on = anchorBase(G.onAnchor, false) +
           lerp(G.onFrom, G.onTo, clamp01(unit(lamp, 2) + jitterOn));
    m.off = anchorBase(G.offAnchor, true) +
            lerp(G.offFrom, G.offTo, clamp01(unit(lamp, 3) + jitterOff));

    if (m.off <= m.on) {
        m.off += 1440;
    }

    // Winter mornings: some households are up before it is light.
    m.morningApplies = G.morning && unit(lamp, 4) < 0.7f;
    m.morningOn = lerp(6 * 60 + 15, 7 * 60 + 45, unit(lamp, 5));
    m.morningOff = _dawn + 15;
    if (m.morningOff < m.morningOn + 45) {
        m.morningOff = m.morningOn + 45;
    }
    m.morningLit = false;
}

uint16_t SceneEngine::targetLevel(uint16_t lamp, const GroupConfig &G, bool &flicker,
                                  LampMoments &m) {
    flicker = false;
    lampMoments(lamp, G, m);

    if (G.behaviour == Behaviour::Off || G.litPercent == 0) {
        return 0;
    }

    // Does this lamp take part at all? (Empty flat, shop closed for good.)
    if (unit(lamp, 1) * 100.0f >= G.litPercent) {
        return 0;
    }

    bool lit = inWindow(_sim, m.on, m.off);

    // Winter mornings: some households are up before it is light.
    if (!lit && m.morningApplies) {
        lit = (_sim >= m.morningOn && _sim < m.morningOff);
        m.morningLit = lit;
    }

    if (!lit) {
        return 0;
    }

    flicker = (G.flickerPercent > 0) && (unit(lamp, 6) * 100.0f < G.flickerPercent);
    return (uint16_t)(G.level * 257);
}

// ---------------------------------------------------------------------------
// Activity: the life on top of the habits
// ---------------------------------------------------------------------------

#define SLOT_MIN        5           // minutes in one slot
#define SLOTS_PER_DAY   288         // 1440 / SLOT_MIN
#define SLOTS_PER_HOUR  12
#define SLOT_SCAN       4           // slots that can still be running at m
#define EVENT_P_MAX     0.5f        // no slot is ever more likely than this

// Event kinds, which are also indices into the length tables below.
#define EVENT_DAY       0
#define EVENT_DIP       1
#define EVENT_NIGHT     2

static const uint8_t eventMinLen[3] = {  3, 2, 1 };     // minutes
static const uint8_t eventMaxLen[3] = { 15, 5, 4 };

// Rates by activity level. Day and dip are per lamp per hour, night is the
// expected count per lamp per night.
static const float dayRate[4]   = { 0.0f, 0.15f, 0.35f, 0.70f };
static const float dipRate[4]   = { 0.0f, 0.08f, 0.15f, 0.30f };
static const float nightRate[4] = { 0.0f, 0.40f, 0.80f, 1.60f };

// The daily curve w(t): how much of the daytime rate applies at minute t.
// Mornings are busy, the middle of the day is quiet, the evening is busy
// again, and nothing much happens in the small hours.
static float dayCurve(int t) {
    if (t < 330)  return 0.1f;      // 23:00 to 05:30, wrapped
    if (t < 390)  return 0.4f;      // 05:30 to 06:30
    if (t < 510)  return 1.0f;      // 06:30 to 08:30
    if (t < 660)  return 0.5f;      // 08:30 to 11:00
    if (t < 960)  return 0.3f;      // 11:00 to 16:00
    if (t < 1260) return 1.0f;      // 16:00 to 21:00
    if (t < 1380) return 0.6f;      // 21:00 to 23:00
    return 0.1f;                    // 23:00 to midnight
}

// One hash decides everything about one slot: whether an event starts in it,
// which minute of the slot it starts on, and how long it runs. Nothing is
// carried from slot to slot, so the town is the same whichever way the clock
// is scrubbed and whatever speed it runs at.
bool SceneEngine::eventAt(uint16_t lamp, int m, uint16_t doy, uint8_t kind,
                          float rate, int windowFrom, int windowTo) const {
    if (kind > EVENT_NIGHT || rate <= 0.0f) {
        return false;
    }

    // The night rate is a count per night, so it is spread over the slots of
    // this lamp's own night rather than over an hour.
    float nightSlots = 0.0f;
    if (kind == EVENT_NIGHT) {
        int len = windowTo - windowFrom;
        if (len <= 0) {
            return false;
        }
        nightSlots = (float)len / (float)SLOT_MIN;
    }

    // The slots that could still be covering m: the one m falls in and the
    // three before it, which is the longest event plus a slot. Oldest first,
    // so that the first event to contain m wins; events never stack.
    int slot = m / SLOT_MIN;
    for (int k = SLOT_SCAN - 1; k >= 0; k--) {
        int s = slot - k;
        uint16_t d = doy;
        int dayBase = 0;
        if (s < 0) {
            // Before midnight, so yesterday's slot on yesterday's day.
            s += SLOTS_PER_DAY;
            d = (doy > 1) ? (uint16_t)(doy - 1) : 366;
            dayBase = -1440;
        }

        uint32_t h = hash((uint32_t)lamp + 0x10000u * d, 0x500u + (uint32_t)s);
        float u = (float)(h >> 8) / 16777216.0f;

        int ts = s * SLOT_MIN;
        float p;
        if (kind == EVENT_DAY) {
            p = rate * dayCurve(ts) / (float)SLOTS_PER_HOUR;
        } else if (kind == EVENT_DIP) {
            p = rate / (float)SLOTS_PER_HOUR;
        } else {
            p = rate / nightSlots;
        }
        if (p > EVENT_P_MAX) {
            p = EVENT_P_MAX;
        }
        if (u >= p) {
            continue;
        }

        int offset = (int)(h & 0x07u) % 5;
        float lenUnit = (float)((h >> 3) & 0x1Fu) / 31.0f;
        int len = lerp(eventMinLen[kind], eventMaxLen[kind], lenUnit);
        int start = dayBase + ts + offset;
        if (m >= start && m < start + len) {
            return true;
        }
    }
    return false;
}

// Rebuild both bitmaps. Called once per simulated minute, not once per tick.
void SceneEngine::evaluateEvents() {
    memset(_eventOn, 0, sizeof(_eventOn));
    memset(_eventOff, 0, sizeof(_eventOff));
    _active = 0;

    uint16_t count = Lamps.count();
    int m = (int)_sim;

    for (uint16_t lamp = 0; lamp < count; lamp++) {
        uint8_t g = _lampGroup[lamp];
        if (g == 0xFF) {
            continue;
        }
        const GroupConfig &G = _cfg.groups[g];
        uint8_t dayLevel = G.dayActivity > 3 ? 3 : G.dayActivity;
        uint8_t nightLevel = G.nightActivity > 3 ? 3 : G.nightActivity;
        if (dayLevel == 0 && nightLevel == 0) {
            continue;                   // a street lamp has no life
        }

        bool flicker;
        LampMoments mo;
        uint16_t base = targetLevel(lamp, G, flicker, mo);

        bool hit = false;
        if (base) {
            // Lit: someone can leave the room for a moment. Not during the
            // morning light, which is a short errand already.
            if (!mo.morningLit) {
                hit = eventAt(lamp, m, _doy, EVENT_DIP, dipRate[dayLevel], 0, 0);
                if (hit) {
                    _eventOff[lamp >> 5] |= 1u << (lamp & 31);
                }
            }
        } else {
            // Dark: a wake-up inside the lamp's own night, a short light
            // outside it. The night runs from the lamp's off moment to 05:30,
            // or to the start of its morning light if that comes first.
            int from = mo.off;
            int to = 1440 + 330;
            if (mo.morningApplies && mo.morningOn + 1440 < to) {
                to = mo.morningOn + 1440;
            }
            bool night = (to > from) && inWindow(m, from, to);
            if (night) {
                hit = eventAt(lamp, m, _doy, EVENT_NIGHT, nightRate[nightLevel],
                              from, to);
            } else {
                hit = eventAt(lamp, m, _doy, EVENT_DAY, dayRate[dayLevel], 0, 0);
            }
            if (hit) {
                _eventOn[lamp >> 5] |= 1u << (lamp & 31);
            }
        }

        if (hit) {
            _active++;
        }
    }
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

    // The activity layer changes only when the simulated clock does, so it is
    // worked out once a simulated minute rather than forty times a second.
    if (_sim != _eventSim || _doy != _eventDoy) {
        _eventSim = _sim;
        _eventDoy = _doy;
        evaluateEvents();
    }

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
        LampMoments moments;
        uint16_t base = targetLevel(lamp, G, flicker, moments);

        // The activity layer, decided above: a short light overrides the base
        // to the group's level, a dip overrides it to dark. Neither flickers.
        uint32_t word = lamp >> 5;
        uint32_t bit = 1u << (lamp & 31);
        if (_eventOn[word] & bit) {
            base = (uint16_t)(G.level * 257);
            flicker = false;
        } else if (_eventOff[word] & bit) {
            base = 0;
            flicker = false;
        }

        if (base) {
            lit++;
        }

        uint16_t target = base;
        if (flicker && base) {
            // A television: dim, restless, mostly blue but we only have one
            // channel, so restless will have to do.
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
