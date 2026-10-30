// The modelled drive's parameters: what a pack's drive.json sets, the four
// built-in presets, and a key table to set them by name. Pure C++ (no
// Arduino), host-tested.
//
// The parameters, their names, ranges and presets match the browser
// prototype (tools/drive-lab), so a drive.json exported from it works here.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class FormFactor : uint8_t { In525, In35, In25 };
enum class Bearing : uint8_t { Ball, Fluid };
enum class Actuator : uint8_t { VoiceCoil, Stepper };
enum class Parking : uint8_t { Ramp, LandingZone, None };
enum class Mount : uint8_t { Bare, Case, Sled };

// Every number is a float, even the counts (slots, platters, cylinders...),
// so one table can set them all; the engine rounds the counts.
struct DriveParams {
    // Spindle motor
    float rpm = 5400, spinupS = 5, coastS = 9;
    float slots = 12, polePairs = 4;
    float whine = 0.45f, hum = 0.35f, imbalance = 0.15f;
    // Platters and air
    float platters = 3;
    FormFactor size = FormFactor::In35;
    float windage = 0.6f;
    bool zoned = true;
    // Bearing
    Bearing bearing = Bearing::Ball;
    float wear = 0.3f, bearingLvl = 0.5f;
    // Actuator
    Actuator actuator = Actuator::VoiceCoil;
    float cylinders = 6000;
    float fullStrokeMs = 19, trackMs = 2, settleDamp = 0.45f, buzz = 0.3f;  // voice coil
    float stepMs = 3, stepRamp = 0;                                         // stepper
    float tick = 0.35f;
    // Arm and case resonance
    float armHz = 1100, armRingMs = 35, caseHz = 380, caseRingMs = 70, hardness = 0.75f;
    // Head events
    Parking park = Parking::LandingZone;
    float calSeeks = 5, recalS = 45, wedges = 72, servo = 0.3f;
    // Enclosure
    Mount mount = Mount::Case;
    float bright = 0, caseRes = 0.4f;
    // Mixer: one gain per layer, then the master
    float gSpindle = 1, gBearing = 1, gWind = 1, gAct = 1, gServo = 1, master = 0.8f;
};

// The built-in presets, by id: "stepper85", "voice91", "voice97" (the
// defaults above) and "fdb05". Returns false for an unknown id.
bool drivePreset(const char* id, DriveParams& out);
size_t drivePresetCount();
const char* drivePresetId(size_t i);

enum class ParamResult : uint8_t { Ok, UnknownKey, BadValue };

// Set one parameter by its drive.json key. Numbers are clamped to the
// parameter's range; a bool key (zoned) takes 0 or 1. setDriveChoice() sets
// the choice keys (size, bearing, actuator, park, mount), e.g. "stepper".
ParamResult setDriveParam(DriveParams& p, const char* key, float value);
ParamResult setDriveChoice(DriveParams& p, const char* key, const char* value);
