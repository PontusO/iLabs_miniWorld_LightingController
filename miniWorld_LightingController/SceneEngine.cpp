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
#define IDENT_MS        250         // one phase of the identify blink
#define IDENT_PHASES    5           // on, off, on, off, on

SceneEngine Scene;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool SceneEngine::begin() {
    // Static: a SceneConfig is about 14 kB and the core-0 stack is far
    // smaller. The sketch is single-threaded and begin() runs once, from
    // setup(), so this never nests with itself.
    static SceneConfig cfg;
    bool had = SceneStore::load(cfg);
    // One call rather than a ternary with SceneConfig(): the temporary would
    // put another 14 kB on the stack. load() leaves cfg at its defaults when
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
    // A new scene ends a blink in progress. Each lamp is given its level
    // back if it is still fitted, and simply let go if it is not, which is
    // what releaseIdentify() already tests for: leaving a lit lamp behind
    // and hoping the new scene writes it is not safe, since a lamp whose
    // new target happens to equal what it had before the blink is never
    // written again and would stay on.
    releaseIdentify();

    // A lamp belongs to the first unit that lists it, so nothing below has
    // to test for ownership twice. A unit whose model has gone is skipped
    // here as well as dropped by clamp(), so the model index read through a
    // unit is always one the engine can follow.
    memset(_lampUnit, 0xFF, sizeof(_lampUnit));
    for (uint8_t u = 0; u < _cfg.unitCount; u++) {
        const UnitConfig &U = _cfg.units[u];
        if (U.model >= _cfg.modelCount) {
            continue;
        }
        for (uint8_t r = 0; r < U.roomCount; r++) {
            const RoomConfig &R = U.rooms[r];
            uint16_t n = R.count();
            for (uint16_t i = 0; i < n; i++) {
                uint16_t l = R.lamp(i);
                if (l < LAMPS_MAX_LAMPS && _lampUnit[l] == 0xFF) {
                    _lampUnit[l] = u;
                }
            }
        }
    }
    memset(_unitLit, 0, sizeof(_unitLit));
    memset(_unitTv, 0, sizeof(_unitTv));
    for (uint8_t u = 0; u < SCENE_MAX_UNITS; u++) {
        _unitDay[u] = UnitDay();    // the old scene's households are gone
    }
    _unitDoy = -1;                  // no day drawn: draw on the next evaluate

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
    // Turning dateFromSystem off changes where the weekday comes from, so
    // the households have to draw their day again even for the same date.
    _unitDoy = -1;
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

// Stable value in [0, 1) for a given purpose. The key is a lamp index for
// an individual hours lamp and a room key everywhere else.
float SceneEngine::unit(uint32_t key, uint32_t salt) const {
    return (float)(hash(key, salt) >> 8) / 16777216.0f;
}

// Per-key, per-day value in [0, 1). Same lamp or room, different evenings.
float SceneEngine::dayUnit(uint32_t key, uint32_t salt) const {
    return (float)(hash(key + 0x10000u * _doy, salt) >> 8) / 16777216.0f;
}

// Per-unit, per-day value in [0, 1). The 0x20000 keeps the unit draws off
// the lamp draws, which live in the low half of the same space.
float SceneEngine::unitUnit(uint8_t unit, uint16_t doy, uint32_t salt) const {
    return (float)(hash(0x20000u + (uint32_t)unit + 0x10000u * (uint32_t)doy,
                        salt) >> 8) / 16777216.0f;
}

// The draw key of one room. Everything a room decides for itself, the
// minutes around the household's moments and the life events alike, hangs
// on this instead of on a lamp index, so the lamps of a room switch
// together and adding one to a room leaves every other room alone. Sixteen
// per unit covers UNIT_MAX_ROOMS with room to spare. The salts keep these
// draws apart from the lamp and unit draws; the keys themselves can meet
// (a room key equals a low lamp key shifted three days in the event slot
// hash), which is harmless and only means the streams are not disjoint.
// Removing a room shifts the keys of the rooms after it, and removing a
// unit shifts the keys of the units after it.
static inline uint32_t roomKey(uint8_t unit, uint8_t room) {
    return 0x30000u + (uint32_t)unit * 16u + (uint32_t)room;
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
void SceneEngine::lampMoments(uint32_t key, const ModelConfig &M, LampMoments &m) const {
    // Personal moment inside each window, plus a daily wobble.
    float jitterOn = (dayUnit(key, 11) - 0.5f) * JITTER_FRACTION;
    float jitterOff = (dayUnit(key, 12) - 0.5f) * JITTER_FRACTION;

    m.on = anchorBase(M.onAnchor, false) +
           lerp(M.onFrom, M.onTo, clamp01(unit(key, 2) + jitterOn));
    m.off = anchorBase(M.offAnchor, true) +
            lerp(M.offFrom, M.offTo, clamp01(unit(key, 3) + jitterOff));

    if (m.off <= m.on) {
        m.off += 1440;
    }

    // Winter mornings: some households are up before it is light.
    m.morningApplies = M.morning && unit(key, 4) < 0.7f;
    m.morningOn = lerp(6 * 60 + 15, 7 * 60 + 45, unit(key, 5));
    m.morningOff = _dawn + 15;
    if (m.morningOff < m.morningOn + 45) {
        m.morningOff = m.morningOn + 45;
    }
    m.morningLit = false;
}

uint16_t SceneEngine::targetLevel(uint32_t key, const ModelConfig &M, bool &flicker,
                                  LampMoments &m) {
    flicker = false;
    lampMoments(key, M, m);

    if (M.litPercent == 0) {
        return 0;               // a model that is never lit
    }

    // Does this lamp, or this room, take part at all? (Empty flat, shop
    // closed for good.)
    if (unit(key, 1) * 100.0f >= M.litPercent) {
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

    flicker = (M.flickerPercent > 0) && (unit(key, 6) * 100.0f < M.flickerPercent);
    return (uint16_t)(M.level * 257);
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
bool SceneEngine::eventAt(uint32_t key, int m, uint16_t doy, uint8_t kind,
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

        uint32_t h = hash(key + 0x10000u * d, 0x500u + (uint32_t)s);
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
        uint8_t u = _lampUnit[lamp];
        if (u == 0xFF) {
            continue;
        }
        const ModelConfig &M = _cfg.models[_cfg.units[u].model];
        if (M.kind != ModelKind::Hours || !M.individual) {
            continue;                   // the room decides, not the lamp
        }
        uint8_t dayLevel = M.dayActivity > 3 ? 3 : M.dayActivity;
        uint8_t nightLevel = M.nightActivity > 3 ? 3 : M.nightActivity;
        if (dayLevel == 0 && nightLevel == 0) {
            continue;                   // a street lamp has no life
        }

        bool flicker;
        LampMoments mo;
        uint16_t base = targetLevel(lamp, M, flicker, mo);

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

    // Every other lamp belongs to a room rather than to itself, so the loop
    // above skipped it. Its life is the same machinery, drawn once for the
    // room with rates scaled by what the room is for.
    evaluateUnitEvents(m);
}

// ---------------------------------------------------------------------------
// Units: the household, or the room, is what behaves, and the lamp is not
// ---------------------------------------------------------------------------

// Salts for the per-room variation, drawn against the room's key. They only
// have to differ from each other and from the habit salts (1..6, 11, 12)
// and the slot salts (0x500..0x61F).
#define SALT_KITCHEN_WAKE   0x700
#define SALT_KITCHEN_ON     0x701
#define SALT_KITCHEN_OFF    0x702
#define SALT_LIVING_OFF     0x703
#define SALT_BED_ON         0x704
#define SALT_BED_OFF        0x705
#define SALT_KITCHEN_EVE    0x706   // the weekend evening, before dinner

// A model has no household type, so the weekend is read off the rhythm
// itself. In bed by 22:30 is the elderly shape: up early whatever the day,
// so the Saturday lie-in is short, the errands are done in the late
// morning, and the weekend is often spent out. This gives the four
// templates the values they had when the type decided it.
static bool earlyToBed(const ModelConfig &M) {
    return !M.isAway() && M.bedTo <= 1350;      // 22:30
}

// How much later the household gets up on a Saturday, and how often it goes
// out in the middle of a weekend day.
static int weekendWakeShift(const ModelConfig &M) {
    if (M.isAway()) return 0;
    if (earlyToBed(M)) return 30;
    return 90;                                  // late risers and everyone else
}

static int weekendOutPercent(const ModelConfig &M) {
    if (M.isAway()) return 0;
    if (earlyToBed(M)) return 40;
    if (M.wakeFrom >= 540) return 30;           // up at 09:00 or later
    return 50;
}

// The day of the week for a day of the year. The real calendar when the
// clock has one and the simulation is following it; otherwise derived from
// the day number, with 2026-01-01 a Thursday. 0 is Sunday, as in struct tm.
uint8_t SceneEngine::weekday(uint16_t doy) const {
    time_t now = time(nullptr);
    if (_cfg.dateFromSystem && now >= 100000) {
        struct tm lt;
        localtime_r(&now, &lt);
        if ((uint16_t)(lt.tm_yday + 1) == doy) {
            return (uint8_t)lt.tm_wday;
        }
    }
    return (uint8_t)((doy + 3) % 7);
}

// The eight draws of one day of one unit. Everything the rooms need is
// decided here, once, so the room schedule is pure arithmetic afterwards.
void SceneEngine::computeUnitDay(uint8_t unit, uint16_t doy, UnitDay &d) const {
    d = UnitDay();
    if (unit >= _cfg.unitCount) {
        return;
    }
    const UnitConfig &U = _cfg.units[unit];
    if (U.model >= _cfg.modelCount) {
        return;
    }
    const ModelConfig &M = _cfg.models[U.model];
    if (M.kind != ModelKind::Rhythm) {
        return;                     // an hours unit has no household day
    }
    if (M.isAway()) {
        return;                     // no rhythm at all, only the timer lamp
    }

    uint8_t wd = weekday(doy);
    d.weekend = M.weekend && (wd == 0 || wd == 6);

    float u1 = unitUnit(unit, doy, 1);
    float u2 = unitUnit(unit, doy, 2);
    float u3 = unitUnit(unit, doy, 3);
    float u4 = unitUnit(unit, doy, 4);
    float u5 = unitUnit(unit, doy, 5);
    float u6 = unitUnit(unit, doy, 6);
    float u7 = unitUnit(unit, doy, 7);
    float u8 = unitUnit(unit, doy, 8);

    if (M.wakeFrom >= 0) {
        d.wake = (int16_t)(lerp(M.wakeFrom, M.wakeTo, u1) +
                           (d.weekend ? weekendWakeShift(M) : 0));
    }
    // Nobody leaves for work on a Saturday, and some households never do.
    if (!d.weekend && M.leaveFrom >= 0) {
        d.leave = (int16_t)lerp(M.leaveFrom, M.leaveTo, u2);
    }
    if (M.homeFrom >= 0) {
        d.home = (int16_t)lerp(M.homeFrom, M.homeTo, u3);
    }
    // Dinner follows coming home; on a weekend, and for a household with no
    // homecoming at all, it sits in the early evening on its own.
    d.dinner = (d.weekend || d.home < 0)
                   ? (int16_t)lerp(17 * 60 + 30, 18 * 60 + 30, u4)
                   : (int16_t)(d.home + lerp(45, 90, u4));
    if (M.bedFrom >= 0) {
        d.bed = (int16_t)(lerp(M.bedFrom, M.bedTo, u5) + (d.weekend ? 30 : 0));
    }

    int outPct = M.outPercent * (d.weekend ? 2 : 1);
    d.out = (d.bed >= 0) && (u6 * 100.0f < (float)outPct);
    d.tv = (u7 * 100.0f < (float)M.tvPercent);
    if (d.out) {
        d.ret = (int16_t)(d.bed - lerp(15, 25, u8));
    }

    // A weekend day out: the errands of an early-to-bed household in the
    // late morning, everyone else in the middle of the day.
    if (d.weekend && u2 * 100.0f < (float)weekendOutPercent(M)) {
        if (earlyToBed(M)) {
            d.outLeave = (int16_t)lerp(10 * 60, 11 * 60 + 30, u3);
            d.outHome = (int16_t)lerp(12 * 60 + 30, 14 * 60, u8);
        } else {
            d.outLeave = (int16_t)lerp(11 * 60, 13 * 60, u3);
            d.outHome = (int16_t)lerp(15 * 60, 17 * 60 + 30, u8);
        }
    }
}

// Is the household out at minute t? Away from work or school, away on a
// weekend errand, or out for the evening and not yet back.
bool SceneEngine::unitOut(const UnitDay &d, int t) const {
    if (d.leave >= 0 && d.home >= 0 && inWindow(t, d.leave, d.home)) {
        return true;
    }
    if (d.outLeave >= 0 && d.outHome >= 0 && inWindow(t, d.outLeave, d.outHome)) {
        return true;
    }
    if (d.out && d.dinner >= 0 && d.ret >= 0 && inWindow(t, d.dinner, d.ret)) {
        return true;
    }
    return false;
}

bool SceneEngine::span(int t, int from, int to, int gateFrom, int gateTo) {
    if (gateFrom >= 0 && from < gateFrom) from = gateFrom;
    if (gateTo >= 0 && to > gateTo) to = gateTo;
    if (to <= from) {
        return false;               // daylight left nothing of it
    }
    return inWindow(t, from, to);
}

// The room schedule. Everything a room does is anchored to a moment of the
// household's day, with the minutes around it drawn from the room's key so
// that two kitchens in one building differ and the lamps of one kitchen do
// not. Evening intervals wait for sunset and
// morning intervals stop at sunrise; the bathroom and the hall do not,
// because a light there is a person, not the light of the room.
bool SceneEngine::roomLit(const UnitDay &d, uint32_t key, Room role, int t,
                          bool out, bool &tv) const {
    tv = false;

    int eveGate = _sunset - 30;     // an evening interval starts no earlier
    int mornGate = _sunrise + 30;   // a morning interval ends no later

    bool lit = false;
    // The hall is the door: its intervals deliberately straddle the moment
    // the household leaves or arrives, and the living room wakes up on an
    // evening out at the moment they are back. Those are the transitions
    // themselves, so being "out" does not hide them.
    bool transition = false;

    switch (role) {
        case Room::Kitchen:
            if (d.wake >= 0) {
                lit = span(t, d.wake,
                           d.wake + lerp(20, 40, unit(key, SALT_KITCHEN_WAKE)),
                           -1, mornGate);
            }
            if (!lit && d.dinner >= 0) {
                int to = d.dinner + lerp(45, 75, unit(key, SALT_KITCHEN_OFF));
                if (d.weekend) {
                    // No homecoming to hang the evening on, so it hangs on
                    // dinner itself. Whether the day was spent out or in,
                    // somebody cooks.
                    lit = span(t, d.dinner - lerp(40, 60, unit(key, SALT_KITCHEN_EVE)),
                               to, eveGate, -1);
                } else if (d.home >= 0) {
                    lit = span(t, d.home + lerp(5, 15, unit(key, SALT_KITCHEN_ON)),
                               to, eveGate, -1);
                }
            }
            if (!lit && d.weekend) {
                lit = span(t, 12 * 60, 13 * 60, -1, -1);    // lunch at home
            }
            break;

        case Room::Hall:
            lit = (d.leave >= 0 && span(t, d.leave - 5, d.leave + 2, -1, -1));
            if (!lit) {
                // The blip of coming in: the homecoming on a weekday, the
                // return from the afternoon out on a weekend, and nothing
                // at all on a weekend day that was spent at home.
                int back = d.weekend ? d.outHome : d.home;
                if (back >= 0) {
                    lit = span(t, back - 1, back + 6, -1, -1);
                }
            }
            if (!lit && d.out && d.ret >= 0) {
                lit = span(t, d.ret - 1, d.ret + 5, -1, -1);
            }
            transition = lit;
            break;

        // A front, a back and a sign are somebody's outside lights under a
        // household: they follow the evening in the same way as the room
        // with no name of its own.
        case Room::Living:
        case Room::Front:
        case Room::Back:
        case Room::Sign:
        case Room::Other:
            if (d.out && d.ret >= 0 && d.bed >= 0) {
                // An evening out: the room only wakes up when they get back.
                lit = span(t, d.ret, d.bed, eveGate, -1);
                transition = lit;
            } else if (d.dinner >= 0 && d.bed >= 0) {
                lit = span(t, d.dinner,
                           d.bed - lerp(5, 15, unit(key, SALT_LIVING_OFF)),
                           eveGate, -1);
            }
            if (lit && role == Room::Living) {
                tv = d.tv;
            }
            break;

        case Room::Bedroom:
            if (d.bed >= 0) {
                lit = span(t, d.bed - lerp(10, 20, unit(key, SALT_BED_ON)),
                           d.bed + lerp(5, 10, unit(key, SALT_BED_OFF)),
                           eveGate, -1);
            }
            if (!lit && d.wake >= 0) {
                lit = span(t, d.wake - 2, d.wake + 8, -1, mornGate);
            }
            break;

        case Room::Bathroom:
            if (d.wake >= 0) {
                lit = span(t, d.wake + 5, d.wake + 15, -1, -1);
            }
            break;

        default:
            break;
    }

    if (lit && out && !transition) {
        lit = false;                // nobody is home, whatever the table says
        tv = false;
    }
    return lit;
}

// Light every lamp of one room, and give it the television if the room has
// one. A lamp an earlier unit listed is left alone.
void SceneEngine::litRoom(uint8_t unitIndex, const RoomConfig &R, bool tv) {
    uint16_t n = R.count();
    for (uint16_t i = 0; i < n; i++) {
        uint16_t lamp = R.lamp(i);
        if (lamp >= LAMPS_MAX_LAMPS || _lampUnit[lamp] != unitIndex) {
            continue;
        }
        _unitLit[lamp >> 5] |= 1u << (lamp & 31);
        if (tv) {
            _unitTv[lamp >> 5] |= 1u << (lamp & 31);
        }
    }
}

// Rebuild the two unit bitmaps, and the days behind them when the date has
// moved. Called once per simulated minute, like the events. Every unit
// whose rooms decide together is in here: a household, and an hours unit
// that switches as one. An individual hours lamp is not, since it decides
// for itself in tick().
void SceneEngine::evaluateUnits() {
    if ((int)_doy != _unitDoy) {
        for (uint8_t u = 0; u < _cfg.unitCount; u++) {
            computeUnitDay(u, _doy, _unitDay[u]);
        }
        _unitDoy = (int)_doy;
    }

    memset(_unitLit, 0, sizeof(_unitLit));
    memset(_unitTv, 0, sizeof(_unitTv));

    int t = (int)_sim;

    for (uint8_t u = 0; u < _cfg.unitCount; u++) {
        const UnitConfig &U = _cfg.units[u];
        if (U.model >= _cfg.modelCount) {
            continue;
        }
        const ModelConfig &M = _cfg.models[U.model];

        if (M.kind == ModelKind::Hours) {
            if (M.individual) {
                continue;           // every lamp keeps its own moment
            }
            // One draw per room, so a street switches in one sweep and a
            // shop's windows go out together.
            for (uint8_t r = 0; r < U.roomCount; r++) {
                bool flicker = false;
                LampMoments mo;
                if (targetLevel(roomKey(u, r), M, flicker, mo)) {
                    litRoom(u, U.rooms[r], flicker);
                }
            }
            continue;
        }

        const UnitDay &d = _unitDay[u];
        if (M.isAway()) {
            // Nobody home this week: one timer room in the living room, or
            // the other room when there is no living room, and nothing else.
            int timer = -1;
            for (uint8_t r = 0; r < U.roomCount; r++) {
                if (U.rooms[r].role == Room::Living) {
                    timer = r;
                    break;
                }
                if (timer < 0 && U.rooms[r].role == Room::Other) {
                    timer = r;
                }
            }
            if (timer >= 0 && inWindow(t, 19 * 60, 22 * 60 + 30)) {
                litRoom(u, U.rooms[timer], false);
            }
            continue;
        }

        bool out = unitOut(d, t);
        for (uint8_t r = 0; r < U.roomCount; r++) {
            const RoomConfig &R = U.rooms[r];
            // One decision per room, taken from the room's key, then handed
            // to every lamp the room owns: a room with three lamps is one
            // room and not three.
            bool tv = false;
            if (!roomLit(d, roomKey(u, r), R.role, t, out, tv)) {
                continue;
            }
            litRoom(u, R, tv);
        }
    }
}

// The life layer for the lamps a room decides for. Same slots, kinds and
// lengths as the per-lamp path; the rate is the model's level scaled by
// what the room is for. A sign has no life of its own.
static const float roomDayRate[(uint8_t)Room::COUNT] =
    { 0.3f, 1.0f, 0.2f, 0.5f, 0.7f, 0.3f, 0.7f, 0.0f, 0.3f };
static const float roomDipRate[(uint8_t)Room::COUNT] =
    { 1.0f, 0.3f, 0.2f, 0.0f, 0.0f, 0.5f, 0.3f, 0.0f, 0.5f };
static const float roomNightRate[(uint8_t)Room::COUNT] =
    { 0.0f, 0.0f, 0.2f, 1.0f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f };

void SceneEngine::evaluateUnitEvents(int m) {
    for (uint8_t u = 0; u < _cfg.unitCount; u++) {
        const UnitConfig &U = _cfg.units[u];
        if (U.model >= _cfg.modelCount) {
            continue;
        }
        const ModelConfig &M = _cfg.models[U.model];
        bool rhythm = (M.kind == ModelKind::Rhythm);
        if (!rhythm && M.individual) {
            continue;               // done lamp by lamp in evaluateEvents()
        }
        if (rhythm && M.isAway()) {
            continue;               // a timer lamp has no life at all
        }
        uint8_t dayLevel = M.dayActivity > 3 ? 3 : M.dayActivity;
        uint8_t nightLevel = M.nightActivity > 3 ? 3 : M.nightActivity;
        if (dayLevel == 0 && nightLevel == 0) {
            continue;
        }

        const UnitDay &d = _unitDay[u];
        // The night of a household is the household's night, not a lamp's
        // own off moment: from bed until they are up again.
        int nightFrom = d.bed;
        int nightTo = d.wake + 1440;
        while (nightTo <= nightFrom) {
            nightTo += 1440;
        }
        bool night = rhythm && d.bed >= 0 && d.wake >= 0 &&
                     inWindow(m, nightFrom, nightTo);
        if (rhythm && unitOut(d, m)) {
            continue;               // nobody is home to switch anything on
        }

        for (uint8_t r = 0; r < U.roomCount; r++) {
            const RoomConfig &R = U.rooms[r];
            uint8_t role = (uint8_t)R.role;
            if (role >= (uint8_t)Room::COUNT) {
                continue;
            }
            // The room is lit or not as a whole, so the first lamp it still
            // owns speaks for all of them. A room whose every lamp went to
            // an earlier unit has nothing left to switch.
            int first = -1;
            uint16_t n = R.count();
            for (uint16_t i = 0; i < n && first < 0; i++) {
                uint16_t lamp = R.lamp(i);
                if (lamp < LAMPS_MAX_LAMPS && _lampUnit[lamp] == u) {
                    first = lamp;
                }
            }
            if (first < 0) {
                continue;
            }
            bool lit = (_unitLit[first >> 5] >> (first & 31)) & 1;

            // One draw for the room; the bitmaps below are per lamp.
            uint32_t key = roomKey(u, r);
            bool morningLit = false;
            if (!rhythm) {
                // The night of a room under an hours model is the room's
                // own, as a lamp's is on the per-lamp path: from its off
                // moment to 05:30, or to its morning light if that is
                // sooner. targetLevel() rather than lampMoments(), because
                // it is what says whether the morning light is what has the
                // room lit; it is the same draw evaluateUnits() made, from
                // the same key, so the answer is the one in the bitmap.
                bool flicker = false;
                LampMoments mo;
                targetLevel(key, M, flicker, mo);
                morningLit = mo.morningLit;
                nightFrom = mo.off;
                nightTo = 1440 + 330;
                if (mo.morningApplies && mo.morningOn + 1440 < nightTo) {
                    nightTo = mo.morningOn + 1440;
                }
                night = (nightTo > nightFrom) && inWindow(m, nightFrom, nightTo);
            }

            bool hit;
            bool on;
            if (lit) {
                // Lit: someone can leave the room for a moment. Not during
                // the morning light, which is a short errand already, just
                // as on the per-lamp path.
                if (morningLit) {
                    continue;
                }
                hit = eventAt(key, m, _doy, EVENT_DIP,
                              dipRate[dayLevel] * roomDipRate[role], 0, 0);
                on = false;
            } else if (night) {
                hit = eventAt(key, m, _doy, EVENT_NIGHT,
                              nightRate[nightLevel] * roomNightRate[role],
                              nightFrom, nightTo);
                on = true;
            } else {
                hit = eventAt(key, m, _doy, EVENT_DAY,
                              dayRate[dayLevel] * roomDayRate[role], 0, 0);
                on = true;
            }
            if (!hit) {
                continue;
            }

            for (uint16_t i = 0; i < n; i++) {
                uint16_t lamp = R.lamp(i);
                if (lamp >= LAMPS_MAX_LAMPS || _lampUnit[lamp] != u) {
                    continue;
                }
                uint32_t word = lamp >> 5;
                uint32_t bit = 1u << (lamp & 31);
                if (on) {
                    _eventOn[word] |= bit;
                } else {
                    _eventOff[word] |= bit;
                }
                _active++;          // a count of lamps, as on the lamp path
            }
        }
    }
}

// ---------------------------------------------------------------------------
// What a unit looks like from outside, for the status API
// ---------------------------------------------------------------------------

const char *SceneEngine::unitState(uint8_t unit) const {
    if (unit >= _cfg.unitCount || _cfg.units[unit].model >= _cfg.modelCount) {
        return "dark";
    }
    const ModelConfig &M = _cfg.models[_cfg.units[unit].model];
    int t = (int)_sim;

    if (M.kind == ModelKind::Hours) {
        // The middle of each window, which is the unit's day rather than
        // any one lamp's. It does not look at litPercent, so a shop whose
        // every lamp opted out is still open; the letters say what is
        // actually lit.
        int on = anchorBase(M.onAnchor, false) + (M.onFrom + M.onTo) / 2;
        int off = anchorBase(M.offAnchor, true) + (M.offFrom + M.offTo) / 2;
        if (off <= on) {
            off += 1440;
        }
        bool lit = inWindow(t, on, off);
        bool byTheClock = (M.onAnchor == Anchor::Clock && M.offAnchor == Anchor::Clock);
        if (byTheClock) {
            return lit ? "open" : "closed";
        }
        return lit ? "lit" : "dark";
    }

    if (M.isAway()) {
        return "away";
    }
    const UnitDay &d = _unitDay[unit];

    if (d.bed >= 0 && d.wake >= 0) {
        int to = d.wake + 1440;
        while (to <= d.bed) {
            to += 1440;
        }
        if (inWindow(t, d.bed, to)) {
            return "asleep";
        }
    }
    if (unitOut(d, t)) {
        return "out";
    }
    return "awake";
}

void SceneEngine::unitLitRooms(uint8_t unit, char *out, size_t len) const {
    static const char letters[(uint8_t)Room::COUNT] =
        { 'l', 'k', 'b', 't', 'h', 'f', 'r', 's', 'o' };

    if (!out || len == 0) {
        return;
    }
    out[0] = 0;
    if (unit >= _cfg.unitCount || _cfg.units[unit].model >= _cfg.modelCount) {
        return;
    }

    // One bit per role first, so a role that has two lit rooms still gives
    // one letter and the letters come out in role order however the unit
    // happens to list its rooms.
    const UnitConfig &U = _cfg.units[unit];
    const ModelConfig &M = _cfg.models[U.model];
    bool perLamp = (M.kind == ModelKind::Hours && M.individual);
    uint16_t seen = 0;
    for (uint8_t r = 0; r < U.roomCount; r++) {
        const RoomConfig &R = U.rooms[r];
        uint8_t role = (uint8_t)R.role;
        if (role >= (uint8_t)Room::COUNT) {
            continue;
        }
        // The lamps of a room carry the same bits, so any lit lamp lights
        // the role. Reading them all keeps this true whatever an earlier
        // unit has taken away. A lamp that decides for itself is lit when
        // it is lit: there is no room bit to read.
        uint16_t n = R.count();
        for (uint16_t i = 0; i < n; i++) {
            uint16_t lamp = R.lamp(i);
            if (lamp >= LAMPS_MAX_LAMPS || _lampUnit[lamp] != unit) {
                continue;
            }
            uint32_t word = lamp >> 5;
            uint32_t bit = 1u << (lamp & 31);
            bool lit;
            if (_eventOn[word] & bit) {
                lit = true;         // a short light counts as lit
            } else if (_eventOff[word] & bit) {
                lit = false;        // and a dip counts as dark
            } else if (perLamp) {
                lit = _current[lamp] != 0;
            } else {
                lit = (_unitLit[word] & bit) != 0;
            }
            if (lit) {
                seen |= (uint16_t)(1u << role);
                break;
            }
        }
    }

    size_t n = 0;
    for (uint8_t role = 0; role < (uint8_t)Room::COUNT && n + 1 < len; role++) {
        if (seen & (1u << role)) {
            out[n++] = letters[role];
        }
    }
    out[n] = 0;
}

// ---------------------------------------------------------------------------
// Identify
// ---------------------------------------------------------------------------

void SceneEngine::identify(const uint16_t *lamps, uint8_t n) {
    // Sift first, release second: a call with nothing blinkable in it left
    // the lamp that was already blinking alone before rooms existed, and it
    // still does. The list is at most eight, so it sits on the stack.
    uint16_t wanted[IDENT_MAX_LAMPS];
    uint8_t k = 0;
    uint16_t count = Lamps.count();
    for (uint8_t i = 0; lamps && i < n && k < IDENT_MAX_LAMPS; i++) {
        uint16_t lamp = lamps[i];
        if (lamp >= count) {
            continue;               // no such lamp: nothing to blink
        }
        bool dup = false;
        for (uint8_t q = 0; q < k && !dup; q++) {
            if (wanted[q] == lamp) dup = true;
        }
        if (!dup) {
            wanted[k++] = lamp;
        }
    }
    if (k == 0) {
        return;
    }

    // One blink at a time. Every lamp that was blinking gets its level back
    // now rather than when its own five phases would have run out, and it
    // has to happen before the levels below are read: a lamp in both blinks
    // must be saved as the scene left it, not as a phase left it.
    releaseIdentify();

    _identCount = k;
    _identStart = millis();
    _identPhase = 0xFF;             // nothing written yet: phase 0 will write
    for (uint8_t i = 0; i < k; i++) {
        uint16_t lamp = wanted[i];
        _identLamp[i] = lamp;
        // What to give back. A lamp the scene owns is mid-fade, and _current
        // is where that fade had got to; a lamp the scene does not own is
        // whatever somebody else last set it to, which only the driver knows.
        _identSaved[i] = (_lampUnit[lamp] != 0xFF)
                       ? _current[lamp] : Lamps.intensity16(lamp);
    }
}

void SceneEngine::releaseIdentify() {
    uint8_t n = _identCount;
    if (n == 0) {
        return;
    }
    _identCount = 0;
    _identPhase = 0xFF;

    // Put every lamp back where the blink found it, in the driver and in
    // _current together. For a lamp in a unit that hands it to the scene's
    // normal path, which fades on from there to whatever the target is
    // now; writing only _current would leave a lamp lit whenever the target
    // happened to equal the level the last phase left behind. For a lamp in
    // no unit, this is the whole restore: the per-lamp loop never touches
    // it.
    uint16_t count = Lamps.count();
    bool changed = false;
    for (uint8_t i = 0; i < n; i++) {
        uint16_t lamp = _identLamp[i];
        if (lamp >= count) {
            continue;               // gone with a lamp config: let it go
        }
        _current[lamp] = _identSaved[i];
        Lamps.setIntensity16(lamp, _identSaved[i]);
        changed = true;
    }
    if (changed) {
        Lamps.show();
    }
}

void SceneEngine::driveIdentify(uint32_t nowMs) {
    if (_identCount == 0) {
        return;
    }
    uint32_t elapsed = nowMs - _identStart;
    if (elapsed >= (uint32_t)IDENT_MS * IDENT_PHASES) {
        releaseIdentify();
        return;
    }
    uint8_t phase = (uint8_t)(elapsed / IDENT_MS);
    if (phase == _identPhase) {
        return;                     // still inside the phase: nothing to send
    }
    _identPhase = phase;
    // Even phases on, odd phases off, so five phases end on. The whole room
    // takes the same phase, so it reads as one room blinking.
    uint16_t level = (phase & 1) ? 0 : 0xFFFF;
    for (uint8_t i = 0; i < _identCount; i++) {
        Lamps.setIntensity16(_identLamp[i], level);
    }
    Lamps.show();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void SceneEngine::tick() {
    uint32_t nowMs = millis();

    // Above the enabled test and above the rate limit: a lamp has to be
    // findable while the scene is paused, and the phase edges are sharper
    // for being taken straight off millis().
    driveIdentify(nowMs);

    if (!_enabled) {
        return;
    }
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
        evaluateUnits();            // the rooms first: the events read them
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
        if (identifying(lamp)) {
            continue;               // blinking: the scene keeps its hands off
        }
        uint32_t word = lamp >> 5;
        uint32_t bit = 1u << (lamp & 31);

        bool flicker;
        uint16_t base;
        uint8_t level;              // the owner's brightness and fade
        uint16_t ownerFade;

        uint8_t u = _lampUnit[lamp];
        if (u == 0xFF) {
            continue;               // not in the scene: leave it alone
        }
        const ModelConfig &M = _cfg.models[_cfg.units[u].model];
        level = M.level;
        ownerFade = M.fadeMs;

        if (M.kind == ModelKind::Rhythm || !M.individual) {
            // The room has already decided, in evaluateUnits(), so this is
            // two bit tests.
            base = (_unitLit[word] & bit) ? (uint16_t)(M.level * 257) : 0;
            flicker = (_unitTv[word] & bit) != 0;
        } else {
            // A lamp with a moment of its own.
            LampMoments moments;
            base = targetLevel(lamp, M, flicker, moments);
        }

        // The activity layer, decided above: a short light overrides the base
        // to the owner's level, a dip overrides it to dark. Neither flickers.
        if (_eventOn[word] & bit) {
            base = (uint16_t)(level * 257);
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
        uint16_t fadeMs = flicker ? 80 : ownerFade;
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
