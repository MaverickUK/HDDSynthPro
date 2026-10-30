#include "drive_timing.h"

#include <math.h>
#include <stdio.h>

#include "sentence.h"

namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The engine's constants (drive_engine.cpp): keep them in step.
constexpr float SETTLE_WIN = 0.003f;
constexpr float SETTLED_X = 0.0006f, SETTLED_V = 0.08f;
constexpr float STEPPER_SETTLE_US = 12000;

} // namespace

SeekModel seekModelOf(const DriveParams& p) {
    SeekModel m;
    m.actuator = p.actuator;
    m.rpm = p.rpm;
    m.cylinders = p.cylinders;
    m.fullStrokeMs = p.fullStrokeMs;
    m.trackMs = p.trackMs;
    m.settleDamp = p.settleDamp;
    m.stepMs = p.stepMs;
    m.stepRamp = p.stepRamp;
    m.zoned = p.zoned;
    return m;
}

uint32_t seekTimeUs(const SeekModel& m, float fromX, float toX) {
    const int cyl = m.cylinders < 2 ? 2 : static_cast<int>(m.cylinders);
    const int from = static_cast<int>(lroundf(clampf(fromX, 0, 1) * (cyl - 1)));
    const int to = static_cast<int>(lroundf(clampf(toX, 0, 1) * (cyl - 1)));
    if (from == to) return 0;

    if (m.actuator == Actuator::Stepper) {
        // One step per cylinder; a buffered seek ramps the step rate up and
        // down (as the engine does), then the head settles.
        const int n = from < to ? to - from : from - to;
        const float tau = 3 + 12 * m.stepRamp;
        float ms = 0;
        for (int i = 1; i < n; ++i) {
            const int k = i < n - i + 1 ? i : n - i + 1;
            ms += m.stepMs * (1 + 4 * m.stepRamp * expf(-k / tau));
        }
        return static_cast<uint32_t>(ms * 1000 + STEPPER_SETTLE_US);
    }

    // Voice coil: run the engine's head (bang-bang seek, then the servo
    // loop) without the sound, until it has settled on the track.
    const float T = fmaxf(0.004f, m.fullStrokeMs / 1000);
    const float A = 4.6f / (T * T);
    const float zeta = clampf(m.settleDamp, 0.05f, 1.5f);
    const float w = 4 / (zeta * fmaxf(0.0003f, m.trackMs / 1000));
    constexpr float dt = 1.0f / 32000;
    float x = fromX, v = 0;
    const float xt = toX;
    bool seeking = fabsf(xt - x) > SETTLE_WIN;
    for (int i = 1; i < 32000 * 2; ++i) {
        const float d = xt - x;
        if (seeking && fabsf(d) < SETTLE_WIN) seeking = false;
        float a;
        if (seeking) a = clampf(((d > 0 ? 1 : -1) * sqrtf(2 * A * 0.8f * fabsf(d)) - v) * 4000, -A, A);
        else a = clampf(w * w * d - 2 * zeta * w * v, -A, A);
        v += a * dt;
        x += v * dt;
        // Settled, checked once a millisecond as the engine's control tick
        // does: an underdamped head swings through the track, and looks
        // still for an instant at the end of each swing.
        if (i % 32 == 0 && !seeking && fabsf(x - xt) < SETTLED_X && fabsf(v) < SETTLED_V) {
            return static_cast<uint32_t>(i * 1e6f * dt);
        }
    }
    return 2000000;
}

uint32_t revolutionUs(const SeekModel& m) { return static_cast<uint32_t>(60e6f / fmaxf(1, m.rpm)); }

uint32_t rotationalWaitUs(const SeekModel& m, uint64_t nowUs, uint32_t sector, uint32_t sectorsPerTrack) {
    if (!sectorsPerTrack) return 0;
    const uint32_t rev = revolutionUs(m);
    const uint32_t at = static_cast<uint32_t>(nowUs % rev);  // where the platter is now
    const uint32_t want = static_cast<uint32_t>(uint64_t(sector % sectorsPerTrack) * rev / sectorsPerTrack);
    return want >= at ? want - at : rev - at + want;
}

uint32_t transferUs(const SeekModel& m, uint32_t sectors, uint32_t sectorsPerTrack) {
    if (!sectorsPerTrack) return 0;
    return static_cast<uint32_t>(uint64_t(sectors) * revolutionUs(m) / sectorsPerTrack);
}

size_t formatModelBody(const SeekModel* m, char* out, size_t outLen) {
    int n;
    if (!m) {
        n = snprintf(out, outLen, "HDM,2,-");
    } else {
        // Integers only: no float printf on the RP2350 (newlib-nano).
        n = snprintf(out, outLen, "HDM,2,%c,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d",
                     m->actuator == Actuator::Stepper ? 'S' : 'V', lroundf(m->rpm), lroundf(m->cylinders),
                     lroundf(m->fullStrokeMs * 10), lroundf(m->trackMs * 10), lroundf(m->settleDamp * 100),
                     lroundf(m->stepMs * 10), lroundf(m->stepRamp * 100), m->zoned ? 1 : 0);
    }
    return n > 0 && static_cast<size_t>(n) < outLen ? static_cast<size_t>(n) : 0;
}

bool parseModelSentence(const char* line, size_t len, SeekModel& out, bool& present) {
    const char* body;
    size_t bodyLen;
    if (!sentenceBody(line, len, body, bodyLen)) return false;
    Field f[12];
    const size_t n = splitFields(body, bodyLen, f, 12);
    uint32_t ver;
    if (n < 3 || !f[0].is("HDM") || !f[1].toUint(ver) || ver < 2) return false;
    if (f[2].is("-")) {
        present = false;
        return true;
    }
    if (n < 11 || !(f[2].is("V") || f[2].is("S"))) return false;
    uint32_t v[8];
    for (int i = 0; i < 8; ++i) {
        if (!f[3 + i].toUint(v[i])) return false;
    }
    if (v[7] > 1 || v[0] == 0 || v[1] < 2) return false;
    SeekModel m;
    m.actuator = f[2].is("S") ? Actuator::Stepper : Actuator::VoiceCoil;
    m.rpm = static_cast<float>(v[0]);
    m.cylinders = static_cast<float>(v[1]);
    m.fullStrokeMs = v[2] / 10.0f;
    m.trackMs = v[3] / 10.0f;
    m.settleDamp = v[4] / 100.0f;
    m.stepMs = v[5] / 10.0f;
    m.stepRamp = v[6] / 100.0f;
    m.zoned = v[7] == 1;
    out = m;
    present = true;
    return true;
}
