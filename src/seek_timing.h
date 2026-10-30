// Authentic seek timing (docs/hddsynth-head-position-brief.md): each media
// command is held for as long as a modelled drive would take (the seek, the
// wait for the sector to come round, the transfer), so the host runs at that
// drive's pace and the HDD Synth plays what the host experiences.
//
// The model is one of the synth's presets, or the one the synth says it is
// playing ($HDM, on the activity link's other direction). The timing itself
// is drive_timing.* (a copy of HDD Synth's, so both agree).
//
// Core 0 asks the Scheduler, for each sector it fetches or stores, when the
// result may go to core 1; until then the drive shows BSY, as a real one does
// during a seek. Nothing here waits. Pure C++, host-tested.
#pragma once

#include <cstddef>
#include <cstdint>

#include "drive_timing.h"

namespace seek_timing {

// ---- The setting (HDDSYNTH.INI seek_timing) ----------------------------------

enum class Source : uint8_t {
    Off,     // as fast as the card allows
    Synth,   // the model from the synth's latest $HDM
    Preset,  // one of the synth's presets, whatever the synth plays
};

constexpr size_t MAX_PRESET = 15;

struct Setting {
    Source source = Source::Off;
    char preset[MAX_PRESET + 1] = "";  // for Source::Preset: a drivePresetId()
};

// "off", "synth" or a preset id (stepper85, voice91, voice97, fdb05), in any
// case. False for anything else, leaving `out` alone.
bool parseSetting(const char* text, size_t len, Setting& out);

// What parseSetting() reads back: "off", "synth" or the preset id.
const char* settingName(const Setting& s);

// "voice" (voice coil) or "stepper", for printing a model.
inline const char* actuatorName(const SeekModel& m) { return m.actuator == Actuator::Stepper ? "stepper" : "voice"; }

// ---- The synth's model ($HDM) --------------------------------------------------

// The synth sends $HDM every 2 s; after this long without one, its model is
// forgotten and commands aren't held.
constexpr uint32_t MODEL_TIMEOUT_MS = 5000;

// What the synth last said it plays. Lines come from a LineAssembler (only
// whole lines starting with '$'); anything but a valid $HDM is counted as
// rejected and otherwise ignored.
class SynthModel {
public:
    // One received line, without its line ending. True for a valid $HDM.
    bool onLine(const char* line, size_t len, uint32_t now_ms);
    // A line cut off or too long (LineAssembler::Result::Dropped).
    void onDropped() { ++rejected_; }

    // The model the synth is playing; null if it's playing a recorded pack
    // ($HDM,2,-), or hasn't said anything for MODEL_TIMEOUT_MS.
    const SeekModel* model(uint32_t now_ms) const;
    // Has a valid $HDM arrived within MODEL_TIMEOUT_MS?
    bool heard(uint32_t now_ms) const { return heard_ && now_ms - last_ms_ < MODEL_TIMEOUT_MS; }
    uint32_t lastMs() const { return last_ms_; }
    uint32_t valid() const { return valid_; }
    uint32_t rejected() const { return rejected_; }

private:
    SeekModel model_;
    bool present_ = false;  // the last $HDM named a model
    bool heard_ = false;    // any valid $HDM yet
    uint32_t last_ms_ = 0;
    uint32_t valid_ = 0;
    uint32_t rejected_ = 0;
};

// ---- Holding each command ------------------------------------------------------

// No command is held longer than this, whatever the model says (the 1985
// stepper's full stroke is about 0.53 s, well inside host timeouts).
constexpr uint32_t MAX_HOLD_US = 1000000;

// When one sector may be released, and for a command's first sector, when
// its head starts to move (where $HDE goes out when timing is on).
struct Release {
    uint64_t ready_us;
    bool first;              // the command's first sector
    uint64_t seek_start_us;  // first only
};

// The modelled drive's head and platter, on one free-running microsecond
// clock. For each command:
//
//   x       = lbaToStroke(lba / total_sectors, zoned)         where the head goes
//   start   = max(now, head_free_at)                          the head must be free
//   arrive  = start + seekTimeUs(head_x, x)                   0 on the same track
//   wait    = rotationalWaitUs(arrive, lba, sectors_per_track)
//   ready_k = arrive + wait + transferUs(k + 1)               sector k under the head
//
// A command that carries on from the last one's next sector doesn't seek or
// wait for the platter: the head reads straight on from head_free_at, as a
// drive's read-ahead or write cache lets it, with at most a revolution read
// ahead if the host was slow to ask. Without that, a host that asks for the
// next command a little after the last one ended would wait nearly a whole
// turn each time, and sequential transfers would run at half the media rate.
//
// Everything is capped at MAX_HOLD_US after the command's first sector
// reached core 0.
class Scheduler {
public:
    // The model to follow; null: off (nothing is held, but the head is still
    // tracked, so switching on later starts from where it is).
    void setModel(const SeekModel* m);
    bool active() const { return has_model_; }
    const SeekModel& model() const { return model_; }

    void setGeometry(uint32_t total_sectors, uint32_t sectors_per_track);

    // One sector of a media command reached core 0 at `now_us`: `lba` is the
    // sector, `command_lba` the command's first. Sectors come in order; a
    // command is new when its first sector changes or `lba` doesn't move on.
    Release onSector(uint64_t now_us, uint32_t command_lba, uint32_t lba);

    // Counts, for the console, kept up to date sector by sector. A command's
    // time is from its first sector reaching core 0 to its latest being
    // ready (only while timing is on).
    uint32_t commands() const { return commands_; }  // timed
    uint32_t held() const { return held_; }          // with any sector held
    uint64_t totalUs() const { return total_us_; }
    uint32_t maxUs() const { return max_us_; }
    uint32_t capped() const { return capped_; }      // commands that hit MAX_HOLD_US
    void resetCounts() { commands_ = held_ = max_us_ = capped_ = 0, total_us_ = 0; }

private:
    SeekModel model_;
    bool has_model_ = false;
    uint32_t total_sectors_ = 1;
    uint32_t sectors_per_track_ = 63;

    float head_x_ = 0;
    uint64_t head_free_at_ = 0;

    bool in_command_ = false;
    bool command_held_ = false;
    bool command_capped_ = false;
    uint32_t command_lba_ = 0;
    uint32_t last_k_ = 0;
    uint32_t next_lba_ = 0;  // the sector after the last one seen
    uint64_t t0_ = 0;        // the command's first sector reached core 0
    uint64_t base_ = 0;      // sector 0 starts passing under the head
    uint32_t command_us_ = 0;  // the command's time so far

    uint32_t commands_ = 0, held_ = 0, max_us_ = 0, capped_ = 0;
    uint64_t total_us_ = 0;
};

}  // namespace seek_timing
