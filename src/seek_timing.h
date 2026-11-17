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
    // First only: the head moves for this command (a read the cache can't
    // serve, a write the drive writes through), so its head target goes to
    // the synth at seek_start_us. Not for a cache hit or a cached write.
    bool moves = true;
    // Cached writes written out to the media during this call (when idle,
    // when the cache was full, or for FLUSH CACHE): the last run, for its
    // head target (its first LBA, sectors and when the head set off).
    bool flushed = false;
    uint32_t flush_lba = 0, flush_sectors = 0;
    uint64_t flush_at_us = 0;
};

// The modelled drive's cache, in sectors, until $HDM carries it: none for a
// stepper drive (writes go straight through; reads go faster only straight
// on, a revolution at most, as before the cache was modelled), and
// for a voice coil by speed, as drives of each era had: 128 KB below 5,400
// rpm, 512 KB at 5,400, 2 MB above. Writes are cached when it's non-zero.
uint32_t cacheSectorsOf(const SeekModel& m);

// Sectors on one of the modelled drive's physical tracks, which sets its
// media rate (sectors x 512 bytes x revolutions a second), until $HDM carries
// it: by cylinder count, as a stand-in for the era's recording density. 17
// (MFM) below 800 cylinders, 40 below 2,000, 200 below 20,000, 900 above:
// 0.5 MB/s for the 1985 stepper, 1.2 for 1991's, 9.2 for 1997's, 55 for
// 2005's. Up to 0.35.0-dev.8 the host's CHS geometry (63) stood in for it,
// which held a 2005 drive to 3.9 MB/s.
uint32_t trackSectorsOf(const SeekModel& m);

// The cache's write side writes runs out once the drive has been idle this
// long (or when it's full, or for FLUSH CACHE).
constexpr uint32_t IDLE_FLUSH_US = 10000;
// Separate runs of cached writes it keeps (neighbouring writes merge).
constexpr unsigned WRITE_RUNS = 16;

// The modelled drive's head and platter, on one free-running microsecond
// clock. For each command:
//
//   x       = lbaToStroke(lba / total_sectors, zoned)         where the head goes
//   start   = max(now, head_free_at)                          the head must be free
//   arrive  = start + seekTimeUs(head_x, x)                   0 on the same track
//   wait    = rotationalWaitUs(arrive, lba, sectors_per_track)
//   ready_k = arrive + wait + transferUs(k + 1)               sector k under the head
//
// The cache (cacheSectorsOf), as drives from the late 1980s on hid most of
// their seeks behind one:
//
//   read-ahead  after a read the head reads on into the cache until it's
//               needed elsewhere. A read of sectors it has passed (and not
//               yet pushed out: the cache holds the last cacheSectors), or
//               will pass within a revolution, is served from there with no
//               seek. Without a cache (a stepper) only a read straight on
//               gains, by a revolution at most. Without this, a host
//               that asks for the next command a little after the last one
//               ended would wait nearly a whole turn each time.
//   write-back  a write is taken into the cache and released at once; the
//               head stays where it is, so a copy's reads keep hitting.
//               Neighbouring writes merge into runs, written out (a seek,
//               the platter, the transfer) once the drive has been idle for
//               IDLE_FLUSH_US, when a write wouldn't fit (the host waits for
//               room), or for FLUSH CACHE (flushAll).
//
// Not modelled: the host turning the write cache off (SET FEATURES 82h),
// reads served from cached writes, and segmented caches.
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

    // The disk's size; `sectors_per_track` is used only until a model is set
    // (which brings its own, trackSectorsOf).
    void setGeometry(uint32_t total_sectors, uint32_t sectors_per_track);
    // Sectors per physical track (set by setModel); for the tests of the
    // mechanics, which use the host's geometry.
    void setTrackSectors(uint32_t sectors) { sectors_per_track_ = sectors ? sectors : 1; }
    uint32_t trackSectors() const { return sectors_per_track_; }

    // One sector of a media command reached core 0 at `now_us`: `lba` is the
    // sector, `command_lba` the command's first. Sectors come in order; a
    // command is new when its first sector changes or `lba` doesn't move on.
    // `write`: a write command; `command_sectors`: its length (for the
    // write cache, which takes a write whole).
    Release onSector(uint64_t now_us, uint32_t command_lba, uint32_t lba, bool write = false,
                     uint32_t command_sectors = 1);

    // FLUSH CACHE: every cached write written out; when it's done (now_us if
    // there was nothing to write). `out`, if given, gets the last run's head
    // target.
    uint64_t flushAll(uint64_t now_us, Release* out = nullptr);

    // The cache in sectors (cacheSectorsOf the model, set by setModel); 0
    // turns it off, for the tests of the mechanics alone.
    void setCacheSectors(uint32_t sectors) { cache_sectors_ = sectors; }
    uint32_t cacheSectors() const { return cache_sectors_; }

    // Counts, for the console, kept up to date sector by sector. A command's
    // time is from its first sector reaching core 0 to its latest being
    // ready (only while timing is on).
    uint32_t commands() const { return commands_; }  // timed
    uint32_t held() const { return held_; }          // with any sector held
    uint64_t totalUs() const { return total_us_; }
    uint32_t maxUs() const { return max_us_; }
    uint32_t capped() const { return capped_; }      // commands that hit MAX_HOLD_US
    uint32_t hits() const { return hits_; }          // reads served from the read-ahead
    uint32_t cachedWrites() const { return cached_writes_; }  // writes taken into the cache
    uint32_t flushes() const { return flushes_; }    // runs written out
    uint32_t pendingSectors() const { return pend_sectors_; }  // cached, not yet written
    void resetCounts() { commands_ = held_ = max_us_ = capped_ = hits_ = cached_writes_ = flushes_ = 0, total_us_ = 0; }

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

    // The cache
    uint32_t cache_sectors_ = 0;
    bool command_cached_write_ = false;  // this command went into the write cache
    uint64_t command_ready_ = 0;         // and when its sectors are released
    // The read-ahead: sector seg_lba_ passes under the head at seg_base_,
    // the ones after it in turn, until seg_end_ (the head moved away).
    bool seg_valid_ = false;
    uint32_t seg_lba_ = 0;
    uint64_t seg_base_ = 0, seg_end_ = 0;
    // Cached writes waiting to be written: runs of sectors, oldest first.
    struct Run {
        uint32_t lba, sectors;
    };
    Run runs_[WRITE_RUNS] = {};
    unsigned run_first_ = 0, run_count_ = 0;
    uint32_t pend_sectors_ = 0;
    uint32_t hits_ = 0, cached_writes_ = 0, flushes_ = 0;

    uint64_t readAheadAt(uint32_t sector) const;  // when `sector` passes the head (the read-ahead)
    uint64_t flushRun(uint64_t start, Release& r);  // writes the oldest run from `start`; when done
    void flushIdle(uint64_t now, Release& r);       // writes runs out while the drive was idle
};

}  // namespace seek_timing
