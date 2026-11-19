#include "drive_params.h"

#include <math.h>
#include <string.h>

namespace {

struct NumParam {
    const char* group;  // the drive.json group, as the Drive Lab's cards
    const char* key;
    float DriveParams::*field;
    float min, max;
    bool integer;
};

// Ranges as the prototype's sliders. A key that would repeat its group's
// name is shorter in drive.json: platters.count, bearing.noise, and the
// mixer's layers (gSpindle is mixer.spindle).
const NumParam NUM_PARAMS[] = {
    {"spindle", "rpm", &DriveParams::rpm, 1800, 15000, true},
    {"spindle", "spinupS", &DriveParams::spinupS, 1, 20, false},
    {"spindle", "coastS", &DriveParams::coastS, 2, 30, false},
    {"spindle", "slots", &DriveParams::slots, 6, 18, true},
    {"spindle", "polePairs", &DriveParams::polePairs, 2, 8, true},
    {"spindle", "whine", &DriveParams::whine, 0, 1, false},
    {"spindle", "hum", &DriveParams::hum, 0, 1, false},
    {"spindle", "imbalance", &DriveParams::imbalance, 0, 1, false},
    {"platters", "count", &DriveParams::platters, 1, 10, true},
    {"platters", "windage", &DriveParams::windage, 0, 1, false},
    {"bearing", "wear", &DriveParams::wear, 0, 1, false},
    {"bearing", "noise", &DriveParams::bearingLvl, 0, 1, false},
    {"actuator", "cylinders", &DriveParams::cylinders, 300, 100000, true},
    {"actuator", "fullStrokeMs", &DriveParams::fullStrokeMs, 8, 60, false},
    {"actuator", "trackMs", &DriveParams::trackMs, 0.5f, 8, false},
    {"actuator", "settleDamp", &DriveParams::settleDamp, 0.1f, 1.2f, false},
    {"actuator", "buzz", &DriveParams::buzz, 0, 1, false},
    {"actuator", "stepMs", &DriveParams::stepMs, 0.2f, 6, false},
    {"actuator", "stepRamp", &DriveParams::stepRamp, 0, 1, false},
    {"actuator", "tick", &DriveParams::tick, 0, 1, false},
    {"resonance", "armHz", &DriveParams::armHz, 200, 4000, false},
    {"resonance", "armRingMs", &DriveParams::armRingMs, 5, 200, false},
    {"resonance", "caseHz", &DriveParams::caseHz, 80, 1200, false},
    {"resonance", "caseRingMs", &DriveParams::caseRingMs, 10, 400, false},
    {"resonance", "hardness", &DriveParams::hardness, 0, 1, false},
    {"heads", "calSeeks", &DriveParams::calSeeks, 0, 8, true},
    {"heads", "recalS", &DriveParams::recalS, 0, 120, false},
    {"heads", "wedges", &DriveParams::wedges, 0, 400, true},
    {"heads", "servo", &DriveParams::servo, 0, 1, false},
    {"enclosure", "bright", &DriveParams::bright, -1, 1, false},
    {"enclosure", "caseRes", &DriveParams::caseRes, 0, 1, false},
    {"mixer", "spindle", &DriveParams::gSpindle, 0, 1.5f, false},
    {"mixer", "bearing", &DriveParams::gBearing, 0, 1.5f, false},
    {"mixer", "air", &DriveParams::gWind, 0, 1.5f, false},
    {"mixer", "actuator", &DriveParams::gAct, 0, 1.5f, false},
    {"mixer", "servo", &DriveParams::gServo, 0, 1.5f, false},
    {"mixer", "master", &DriveParams::master, 0, 1.5f, false},
};

// A choice key: its names, in enum order.
template <typename E>
bool pick(const char* value, const char* const* names, size_t n, E& out) {
    for (size_t i = 0; i < n; ++i) {
        if (!strcmp(value, names[i])) {
            out = static_cast<E>(i);
            return true;
        }
    }
    return false;
}

const char* const SIZES[] = {"5.25", "3.5", "2.5"};
const char* const BEARINGS[] = {"ball", "fdb"};
const char* const ACTUATORS[] = {"voice", "stepper"};
const char* const PARKINGS[] = {"ramp", "zone", "none"};
const char* const MOUNTS[] = {"bare", "case", "sled"};

DriveParams stepper85() {
    DriveParams p;
    p.rpm = 3600; p.spinupS = 12; p.coastS = 16; p.slots = 12; p.polePairs = 4;
    p.whine = 0.6f; p.hum = 0.5f; p.imbalance = 0.3f;
    p.platters = 2; p.size = FormFactor::In525; p.windage = 0.7f; p.zoned = false;
    p.bearing = Bearing::Ball; p.wear = 0.55f; p.bearingLvl = 0.75f;
    p.actuator = Actuator::Stepper; p.cylinders = 615; p.fullStrokeMs = 40; p.trackMs = 3; p.settleDamp = 0.5f;
    p.buzz = 0; p.stepMs = 0.8f; p.stepRamp = 0.5f; p.tick = 0.6f;
    p.armHz = 900; p.armRingMs = 35; p.caseHz = 210; p.caseRingMs = 140; p.hardness = 0.6f;
    p.park = Parking::None; p.calSeeks = 2; p.recalS = 0; p.wedges = 0; p.servo = 0;
    p.mount = Mount::Case; p.bright = 0; p.caseRes = 0.6f;
    return p;
}

DriveParams voice91() {
    DriveParams p;
    p.rpm = 3600; p.spinupS = 6; p.coastS = 10; p.slots = 9; p.polePairs = 3;
    p.whine = 0.5f; p.hum = 0.4f; p.imbalance = 0.2f;
    p.platters = 2; p.size = FormFactor::In35; p.windage = 0.5f; p.zoned = false;
    p.bearing = Bearing::Ball; p.wear = 0.35f; p.bearingLvl = 0.6f;
    p.actuator = Actuator::VoiceCoil; p.cylinders = 1000; p.fullStrokeMs = 35; p.trackMs = 5; p.settleDamp = 0.35f;
    p.buzz = 0.4f; p.stepMs = 3; p.stepRamp = 0; p.tick = 0.4f;
    p.armHz = 650; p.armRingMs = 60; p.caseHz = 300; p.caseRingMs = 90; p.hardness = 0.5f;
    p.park = Parking::LandingZone; p.calSeeks = 4; p.recalS = 0; p.wedges = 0; p.servo = 0;
    p.mount = Mount::Case; p.bright = -0.2f; p.caseRes = 0.5f;
    return p;
}

DriveParams fdb05() {
    DriveParams p;
    p.rpm = 7200; p.spinupS = 6; p.coastS = 12; p.slots = 9; p.polePairs = 4;
    p.whine = 0.25f; p.hum = 0.2f; p.imbalance = 0.05f;
    p.platters = 2; p.size = FormFactor::In35; p.windage = 0.5f; p.zoned = true;
    p.bearing = Bearing::Fluid; p.wear = 0.1f; p.bearingLvl = 0.4f;
    p.actuator = Actuator::VoiceCoil; p.cylinders = 80000; p.fullStrokeMs = 14; p.trackMs = 0.8f; p.settleDamp = 0.6f;
    p.buzz = 0.15f; p.stepMs = 3; p.stepRamp = 0; p.tick = 0.25f;
    p.armHz = 1800; p.armRingMs = 20; p.caseHz = 450; p.caseRingMs = 50; p.hardness = 0.9f;
    p.park = Parking::Ramp; p.calSeeks = 3; p.recalS = 0; p.wedges = 200; p.servo = 0.2f;
    p.mount = Mount::Case; p.bright = 0.2f; p.caseRes = 0.3f;
    return p;
}

const char* const PRESET_IDS[] = {"stepper85", "voice91", "voice97", "fdb05"};

} // namespace

bool drivePreset(const char* id, DriveParams& out) {
    if (!strcmp(id, "stepper85")) out = stepper85();
    else if (!strcmp(id, "voice91")) out = voice91();
    else if (!strcmp(id, "voice97")) out = DriveParams{};
    else if (!strcmp(id, "fdb05")) out = fdb05();
    else return false;
    return true;
}

size_t drivePresetCount() { return sizeof(PRESET_IDS) / sizeof(PRESET_IDS[0]); }
const char* drivePresetId(size_t i) { return i < drivePresetCount() ? PRESET_IDS[i] : nullptr; }

ParamResult setDriveParam(DriveParams& p, const char* group, const char* key, float value) {
    if (!isfinite(value)) return ParamResult::BadValue;
    if (!strcmp(group, "platters") && !strcmp(key, "zoned")) {
        p.zoned = value != 0;
        return ParamResult::Ok;
    }
    for (const NumParam& n : NUM_PARAMS) {
        if (strcmp(group, n.group) || strcmp(key, n.key)) continue;
        float v = value < n.min ? n.min : (value > n.max ? n.max : value);
        if (n.integer) v = roundf(v);
        p.*n.field = v;
        return ParamResult::Ok;
    }
    return ParamResult::UnknownKey;
}

ParamResult setDriveChoice(DriveParams& p, const char* group, const char* key, const char* value) {
    auto is = [&](const char* g, const char* k) { return !strcmp(group, g) && !strcmp(key, k); };
    bool ok;
    if (is("platters", "size")) ok = pick(value, SIZES, 3, p.size);
    else if (is("bearing", "type")) ok = pick(value, BEARINGS, 2, p.bearing);
    else if (is("actuator", "type")) ok = pick(value, ACTUATORS, 2, p.actuator);
    else if (is("heads", "parking")) ok = pick(value, PARKINGS, 3, p.park);
    else if (is("enclosure", "mount")) ok = pick(value, MOUNTS, 3, p.mount);
    else return ParamResult::UnknownKey;
    return ok ? ParamResult::Ok : ParamResult::BadValue;
}
