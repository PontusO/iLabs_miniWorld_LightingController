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

    // A lamp a flat lists belongs to the flat, and the first flat listing it
    // wins. Clearing its group entry keeps the group path from ever seeing
    // it, so nothing below has to test for ownership twice.
    memset(_lampFlat, 0xFF, sizeof(_lampFlat));
    for (uint8_t f = 0; f < _cfg.flatCount; f++) {
        const FlatConfig &F = _cfg.flats[f];
        for (uint8_t r = 0; r < F.roomCount; r++) {
            uint16_t l = F.rooms[r].lamp;
            if (l < LAMPS_MAX_LAMPS && _lampFlat[l] == 0xFF) {
                _lampFlat[l] = f;
                _lampGroup[l] = 0xFF;
            }
        }
    }
    memset(_flatLit, 0, sizeof(_flatLit));
    memset(_flatTv, 0, sizeof(_flatTv));
    for (uint8_t f = 0; f < SCENE_MAX_FLATS; f++) {
        _flatDay[f] = FlatDay();    // the old scene's households are gone
    }
    _flatDoy = -1;                  // no day drawn: draw on the next evaluate

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
    _flatDoy = -1;
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

// Per-flat, per-day value in [0, 1). The 0x20000 keeps the flat draws off
// the lamp draws, which live in the low half of the same space.
float SceneEngine::flatUnit(uint8_t flat, uint16_t doy, uint32_t salt) const {
    return (float)(hash(0x20000u + (uint32_t)flat + 0x10000u * (uint32_t)doy,
                        salt) >> 8) / 16777216.0f;
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

    // Flat lamps are not in any group, so the loop above never saw them.
    // Their life is the same machinery with rates scaled by the room.
    evaluateFlatEvents(m);
}

// ---------------------------------------------------------------------------
// Flats: the household is the unit of behaviour, the lamp is not
// ---------------------------------------------------------------------------

// Salts for the per-room variation. They only have to differ from each other
// and from the habit salts (1..6, 11, 12) and the slot salts (0x500..0x61F).
#define SALT_KITCHEN_WAKE   0x700
#define SALT_KITCHEN_ON     0x701
#define SALT_KITCHEN_OFF    0x702
#define SALT_LIVING_OFF     0x703
#define SALT_BED_ON         0x704
#define SALT_BED_OFF        0x705
#define SALT_KITCHEN_EVE    0x706   // the weekend evening, before dinner

// How much later the household gets up on a Saturday, and how often it goes
// out in the middle of a weekend day, by household type. Custom starts from
// the family rhythm, so it keeps the family weekend too.
static int weekendWakeShift(Household t) {
    switch (t) {
        case Household::Elderly:  return 30;
        case Household::NightOwl: return 90;
        case Household::Away:     return 0;
        default:                  return 90;    // family, custom
    }
}

static int weekendOutPercent(Household t) {
    switch (t) {
        case Household::Elderly:  return 40;
        case Household::NightOwl: return 30;
        case Household::Away:     return 0;
        default:                  return 50;    // family, custom
    }
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

// The eight draws of one day of one flat. Everything the rooms need is
// decided here, once, so the room schedule is pure arithmetic afterwards.
void SceneEngine::computeFlatDay(uint8_t flat, uint16_t doy, FlatDay &d) const {
    d = FlatDay();
    if (flat >= _cfg.flatCount) {
        return;
    }
    const FlatConfig &F = _cfg.flats[flat];
    if (F.type == Household::Away) {
        return;                     // no rhythm at all, only the timer lamp
    }

    uint8_t wd = weekday(doy);
    d.weekend = F.weekend && (wd == 0 || wd == 6);

    float u1 = flatUnit(flat, doy, 1);
    float u2 = flatUnit(flat, doy, 2);
    float u3 = flatUnit(flat, doy, 3);
    float u4 = flatUnit(flat, doy, 4);
    float u5 = flatUnit(flat, doy, 5);
    float u6 = flatUnit(flat, doy, 6);
    float u7 = flatUnit(flat, doy, 7);
    float u8 = flatUnit(flat, doy, 8);

    if (F.wakeFrom >= 0) {
        d.wake = (int16_t)(lerp(F.wakeFrom, F.wakeTo, u1) +
                           (d.weekend ? weekendWakeShift(F.type) : 0));
    }
    // Nobody leaves for work on a Saturday, and some households never do.
    if (!d.weekend && F.leaveFrom >= 0) {
        d.leave = (int16_t)lerp(F.leaveFrom, F.leaveTo, u2);
    }
    if (F.homeFrom >= 0) {
        d.home = (int16_t)lerp(F.homeFrom, F.homeTo, u3);
    }
    // Dinner follows coming home; on a weekend, and for a household with no
    // homecoming at all, it sits in the early evening on its own.
    d.dinner = (d.weekend || d.home < 0)
                   ? (int16_t)lerp(17 * 60 + 30, 18 * 60 + 30, u4)
                   : (int16_t)(d.home + lerp(45, 90, u4));
    if (F.bedFrom >= 0) {
        d.bed = (int16_t)(lerp(F.bedFrom, F.bedTo, u5) + (d.weekend ? 30 : 0));
    }

    int outPct = F.outPercent * (d.weekend ? 2 : 1);
    d.out = (d.bed >= 0) && (u6 * 100.0f < (float)outPct);
    d.tv = (u7 * 100.0f < (float)F.tvPercent);
    if (d.out) {
        d.ret = (int16_t)(d.bed - lerp(15, 25, u8));
    }

    // A weekend day out: the errands of an elderly couple in the late
    // morning, everyone else in the middle of the day.
    if (d.weekend && u2 * 100.0f < (float)weekendOutPercent(F.type)) {
        if (F.type == Household::Elderly) {
            d.outLeave = (int16_t)lerp(10 * 60, 11 * 60 + 30, u3);
            d.outHome = (int16_t)lerp(12 * 60 + 30, 14 * 60, u8);
        } else {
            d.outLeave = (int16_t)lerp(11 * 60, 13 * 60, u3);
            d.outHome = (int16_t)lerp(15 * 60, 17 * 60 + 30, u8);
        }
    }
}

// Is the household out of the flat at minute t? Away from work or school,
// away on a weekend errand, or out for the evening and not yet back.
bool SceneEngine::flatOut(const FlatDay &d, int t) const {
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
// household's day, with the minutes around it drawn per lamp so that two
// kitchens in one building differ. Evening intervals wait for sunset and
// morning intervals stop at sunrise; the bathroom and the hall do not,
// because a light there is a person, not the light of the room.
bool SceneEngine::roomLit(const FlatDay &d, uint16_t lamp, Room role, int t,
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
                           d.wake + lerp(20, 40, unit(lamp, SALT_KITCHEN_WAKE)),
                           -1, mornGate);
            }
            if (!lit && d.dinner >= 0) {
                int to = d.dinner + lerp(45, 75, unit(lamp, SALT_KITCHEN_OFF));
                if (d.weekend) {
                    // No homecoming to hang the evening on, so it hangs on
                    // dinner itself. Whether the day was spent out or in,
                    // somebody cooks.
                    lit = span(t, d.dinner - lerp(40, 60, unit(lamp, SALT_KITCHEN_EVE)),
                               to, eveGate, -1);
                } else if (d.home >= 0) {
                    lit = span(t, d.home + lerp(5, 15, unit(lamp, SALT_KITCHEN_ON)),
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

        case Room::Living:
        case Room::Other:
            if (d.out && d.ret >= 0 && d.bed >= 0) {
                // An evening out: the room only wakes up when they get back.
                lit = span(t, d.ret, d.bed, eveGate, -1);
                transition = lit;
            } else if (d.dinner >= 0 && d.bed >= 0) {
                lit = span(t, d.dinner,
                           d.bed - lerp(5, 15, unit(lamp, SALT_LIVING_OFF)),
                           eveGate, -1);
            }
            if (lit && role == Room::Living) {
                tv = d.tv;
            }
            break;

        case Room::Bedroom:
            if (d.bed >= 0) {
                lit = span(t, d.bed - lerp(10, 20, unit(lamp, SALT_BED_ON)),
                           d.bed + lerp(5, 10, unit(lamp, SALT_BED_OFF)),
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

// Rebuild the two flat bitmaps, and the day behind them when the date has
// moved. Called once per simulated minute, like the events.
void SceneEngine::evaluateFlats() {
    if ((int)_doy != _flatDoy) {
        for (uint8_t f = 0; f < _cfg.flatCount; f++) {
            computeFlatDay(f, _doy, _flatDay[f]);
        }
        _flatDoy = (int)_doy;
    }

    memset(_flatLit, 0, sizeof(_flatLit));
    memset(_flatTv, 0, sizeof(_flatTv));

    int t = (int)_sim;

    for (uint8_t f = 0; f < _cfg.flatCount; f++) {
        const FlatConfig &F = _cfg.flats[f];
        const FlatDay &d = _flatDay[f];

        if (F.type == Household::Away) {
            // Nobody home this week: one timer lamp in the living room, or
            // the other room when there is no living room, and nothing else.
            int timer = -1;
            for (uint8_t r = 0; r < F.roomCount; r++) {
                if (F.rooms[r].role == Room::Living) {
                    timer = r;
                    break;
                }
                if (timer < 0 && F.rooms[r].role == Room::Other) {
                    timer = r;
                }
            }
            if (timer >= 0) {
                uint16_t lamp = F.rooms[timer].lamp;
                if (_lampFlat[lamp] == f && inWindow(t, 19 * 60, 22 * 60 + 30)) {
                    _flatLit[lamp >> 5] |= 1u << (lamp & 31);
                }
            }
            continue;
        }

        bool out = flatOut(d, t);
        for (uint8_t r = 0; r < F.roomCount; r++) {
            uint16_t lamp = F.rooms[r].lamp;
            if (_lampFlat[lamp] != f) {
                continue;           // an earlier flat listed this lamp
            }
            bool tv = false;
            if (roomLit(d, lamp, F.rooms[r].role, t, out, tv)) {
                _flatLit[lamp >> 5] |= 1u << (lamp & 31);
                if (tv) {
                    _flatTv[lamp >> 5] |= 1u << (lamp & 31);
                }
            }
        }
    }
}

// The life layer for flat lamps. Same slots, kinds and lengths as the group
// path; the rate is the flat's level scaled by what the room is for.
static const float roomDayRate[(uint8_t)Room::COUNT] =
    { 0.3f, 1.0f, 0.2f, 0.5f, 0.7f, 0.3f };
static const float roomDipRate[(uint8_t)Room::COUNT] =
    { 1.0f, 0.3f, 0.2f, 0.0f, 0.0f, 0.5f };
static const float roomNightRate[(uint8_t)Room::COUNT] =
    { 0.0f, 0.0f, 0.2f, 1.0f, 0.3f, 0.0f };

void SceneEngine::evaluateFlatEvents(int m) {
    for (uint8_t f = 0; f < _cfg.flatCount; f++) {
        const FlatConfig &F = _cfg.flats[f];
        if (F.type == Household::Away) {
            continue;               // a timer lamp has no life at all
        }
        uint8_t dayLevel = F.dayActivity > 3 ? 3 : F.dayActivity;
        uint8_t nightLevel = F.nightActivity > 3 ? 3 : F.nightActivity;
        if (dayLevel == 0 && nightLevel == 0) {
            continue;
        }

        const FlatDay &d = _flatDay[f];
        if (flatOut(d, m)) {
            continue;               // nobody is home to switch anything on
        }

        // The night of a flat is the household's night, not the lamp's own
        // off moment: from bed until the household is up again.
        int nightFrom = d.bed;
        int nightTo = d.wake + 1440;
        while (nightTo <= nightFrom) {
            nightTo += 1440;
        }
        bool haveNight = (d.bed >= 0 && d.wake >= 0);
        bool night = haveNight && inWindow(m, nightFrom, nightTo);

        for (uint8_t r = 0; r < F.roomCount; r++) {
            uint16_t lamp = F.rooms[r].lamp;
            if (_lampFlat[lamp] != f) {
                continue;
            }
            uint8_t role = (uint8_t)F.rooms[r].role;
            if (role >= (uint8_t)Room::COUNT) {
                continue;
            }
            uint32_t word = lamp >> 5;
            uint32_t bit = 1u << (lamp & 31);

            bool hit = false;
            if (_flatLit[word] & bit) {
                hit = eventAt(lamp, m, _doy, EVENT_DIP,
                              dipRate[dayLevel] * roomDipRate[role], 0, 0);
                if (hit) {
                    _eventOff[word] |= bit;
                }
            } else {
                if (night) {
                    hit = eventAt(lamp, m, _doy, EVENT_NIGHT,
                                  nightRate[nightLevel] * roomNightRate[role],
                                  nightFrom, nightTo);
                } else {
                    hit = eventAt(lamp, m, _doy, EVENT_DAY,
                                  dayRate[dayLevel] * roomDayRate[role], 0, 0);
                }
                if (hit) {
                    _eventOn[word] |= bit;
                }
            }

            if (hit) {
                _active++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// What a flat looks like from outside, for the status API
// ---------------------------------------------------------------------------

const char *SceneEngine::flatState(uint8_t flat) const {
    if (flat >= _cfg.flatCount) {
        return "away";
    }
    if (_cfg.flats[flat].type == Household::Away) {
        return "away";
    }
    const FlatDay &d = _flatDay[flat];
    int t = (int)_sim;

    if (d.bed >= 0 && d.wake >= 0) {
        int to = d.wake + 1440;
        while (to <= d.bed) {
            to += 1440;
        }
        if (inWindow(t, d.bed, to)) {
            return "asleep";
        }
    }
    if (flatOut(d, t)) {
        return "out";
    }
    return "awake";
}

void SceneEngine::flatLitRooms(uint8_t flat, char *out, size_t len) const {
    static const char letters[(uint8_t)Room::COUNT] =
        { 'l', 'k', 'b', 't', 'h', 'o' };

    if (!out || len == 0) {
        return;
    }
    out[0] = 0;
    if (flat >= _cfg.flatCount) {
        return;
    }

    // One bit per role first, so a role that has two lit rooms still gives
    // one letter and the letters come out in role order however the flat
    // happens to list its rooms.
    const FlatConfig &F = _cfg.flats[flat];
    uint8_t seen = 0;
    for (uint8_t r = 0; r < F.roomCount; r++) {
        uint16_t lamp = F.rooms[r].lamp;
        if (_lampFlat[lamp] != flat) {
            continue;
        }
        uint8_t role = (uint8_t)F.rooms[r].role;
        if (role >= (uint8_t)Room::COUNT) {
            continue;
        }
        uint32_t word = lamp >> 5;
        uint32_t bit = 1u << (lamp & 31);
        bool lit;
        if (_eventOn[word] & bit) {
            lit = true;             // a short light counts as lit
        } else if (_eventOff[word] & bit) {
            lit = false;            // and a dip counts as dark
        } else {
            lit = (_flatLit[word] & bit) != 0;
        }
        if (lit) {
            seen |= (uint8_t)(1u << role);
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
        evaluateFlats();            // the rooms first: the events read them
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
        uint32_t word = lamp >> 5;
        uint32_t bit = 1u << (lamp & 31);

        bool flicker;
        uint16_t base;
        uint8_t level;              // the owner's brightness and fade
        uint16_t ownerFade;

        uint8_t f = _lampFlat[lamp];
        if (f != 0xFF) {
            // Owned by a flat: evaluateFlats() has already decided, so this
            // is two bit tests. Its group entry was cleared in rebuild().
            const FlatConfig &F = _cfg.flats[f];
            level = F.level;
            ownerFade = F.fadeMs;
            base = (_flatLit[word] & bit) ? (uint16_t)(F.level * 257) : 0;
            flicker = (_flatTv[word] & bit) != 0;
        } else {
            uint8_t g = _lampGroup[lamp];
            if (g == 0xFF) {
                continue;               // not in the scene: leave it alone
            }
            const GroupConfig &G = _cfg.groups[g];
            level = G.level;
            ownerFade = G.fadeMs;

            LampMoments moments;
            base = targetLevel(lamp, G, flicker, moments);
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
