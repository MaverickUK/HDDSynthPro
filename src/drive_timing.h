// How long a modelled drive takes to do what it's asked: the seek, the wait
// for the sector to come round, and the transfer. Pure C++ (no Arduino),
// host-tested against the drive engine's own head (drive_engine.*), so the
// timing matches what the synth plays.
//
// For HDDSynthPro's authentic timing: it copies this file and
// drive_params.* (also pure), learns the synth's model from $HDM (below) or
// a preset, and holds each command for seek + rotation + transfer.
// HDDSynthPro's docs/hddsynth-head-position-brief.md describes it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "drive_params.h"

// The parameters that decide timing (a subset of DriveParams).
struct SeekModel {
    Actuator actuator = Actuator::VoiceCoil;
    float rpm = 5400;
    float cylinders = 6000;
    float fullStrokeMs = 19, trackMs = 2, settleDamp = 0.45f;  // voice coil
    float stepMs = 3, stepRamp = 0;                            // stepper
    bool zoned = true;
};

SeekModel seekModelOf(const DriveParams& p);

constexpr float STROKE_INNER_RADIUS = 0.45f;  // the data zone's inner radius, as a fraction of the outer

// Where on the stroke (0 = outer edge, 1 = inner) a fraction of the disk's
// LBAs lies. Zoned recording packs more sectors into the outer tracks, at
// constant areal density, so the first half of the LBAs covers well under
// half of the stroke; without it, tracks are evenly filled.
inline float lbaToStroke(float f, bool zoned) {
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    if (!zoned) return f;
    const float ri2 = STROKE_INNER_RADIUS * STROKE_INNER_RADIUS;
    float r2 = 1 - f * (1 - ri2);
    // sqrt by Newton's method keeps this header free of <math.h> differences.
    float r = r2 > 0.5f ? r2 : 0.5f;
    for (int i = 0; i < 6; ++i) r = 0.5f * (r + r2 / r);
    return (1 - r) / (1 - STROKE_INNER_RADIUS);
}

// The time for the head to move from `fromX` to `toX` (stroke positions)
// and settle on the track, in microseconds; 0 if it's the same track.
uint32_t seekTimeUs(const SeekModel& m, float fromX, float toX);

// One revolution, in microseconds.
uint32_t revolutionUs(const SeekModel& m);

// After the seek, the wait for `sector` to reach the head. The platter's
// angle is taken from a free-running clock (`nowUs`, when the head
// arrives); sector s of a track lies at angle (s mod sectorsPerTrack) /
// sectorsPerTrack, so reading on from the last sector costs nothing.
uint32_t rotationalWaitUs(const SeekModel& m, uint64_t nowUs, uint32_t sector, uint32_t sectorsPerTrack);

// Reading or writing `sectors` sectors passing under the head.
uint32_t transferUs(const SeekModel& m, uint32_t sectors, uint32_t sectorsPerTrack);

// $HDM: the synth tells HDDSynthPro (on the link's other direction, J6 pin 4) which model
// is playing, every 2 s and when the pack changes:
//
//   $HDM,2,<V|S>,<rpm>,<cylinders>,<fullStroke ms x10>,<track ms x10>,<settleDamp x100>,<step ms x10>,<stepRamp x100>,<zoned 0|1>
//   $HDM,2,-        no modelled drive (a recorded pack)
//
// formatModelBody() writes the body (between '$' and '*'); `m` null means
// none. parseModelSentence() reads a whole line: true if valid, with
// `present` false for "none".
size_t formatModelBody(const SeekModel* m, char* out, size_t outLen);
bool parseModelSentence(const char* line, size_t len, SeekModel& out, bool& present);
