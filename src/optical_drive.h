// The CD-ROM drive as a real one would sound: whether its disc spins, whether
// it's reading (sequentially or jumping about), and its tray, for the HDD
// Synth's CD-ROM pack ($HDO, activity_link.h; docs/activity-link.md). The
// emulated drive answers at once and never spins anything, so this models
// what a real drive would be doing:
//
//   - the disc spins up when a disc goes in, at power-on with one in, and
//     when the host reads it while stopped; it spins down after
//     `spin_down_ms` without a read, as drives of the 1990s and 2000s did;
//   - it's busy from each read's start to its end, and at least `busy_min_ms`
//     (the activity light's LED_MIN_MS);
//   - its reads are classified as the hard disk's are (activity::Classifier),
//     in 2,048-byte frames;
//   - the tray opens on an eject (console `M`, which swaps the disc), with
//     the disc braking first, and closes once the new disc is in, but not
//     before `tray_min_open_ms`: a person takes that long, and the synth's
//     tray_open.wav needs to finish.
//
// Fed from the command events on core 0 (only the CD-ROM's, by unit). Pure
// C++, host-tested.
#pragma once

#include <cstdint>

#include "activity.h"

namespace optical {

struct Config {
    uint32_t spin_down_ms = 30000;     // no read this long: the disc stops
    uint32_t busy_min_ms = 25;         // a read lights the activity this long at least
    uint32_t tray_min_open_ms = 3000;  // an eject's tray stays open this long at least
};

// Does this MMC command read the disc (or move to a place on it)? These spin
// it up and count as activity. The rest (TEST UNIT READY, GET EVENT STATUS
// NOTIFICATION, which Windows sends every second, INQUIRY, MODE SENSE and the
// like) answer from what the drive already knows, as a real drive's do.
bool readsDisc(uint8_t packet);

// What the synth is told.
struct State {
    bool busy = false;
    activity::State reads = activity::State::Idle;  // the reads' class
    bool motor = false;     // the disc spins
    bool tray_open = false;
};

class Drive {
public:
    explicit Drive(const Config& config = Config()) : cfg_(config), reads_(readsConfig()) {}

    // At start-up: a disc in the drive spins up, as one does at power-on.
    void begin(uint32_t now_ms, bool disc);
    // A command for the CD-ROM started (`packet`: PACKET's opcode, else 0;
    // `lba` and `frames`: a read's, in frames) or ended.
    void onCommand(uint32_t now_ms, uint32_t t_us, uint8_t packet, uint32_t lba, uint32_t frames);
    void onCommandEnd(uint32_t now_ms, uint32_t t_us);
    // Console `M`: the disc comes out (the tray opens), then `load`s the next
    // (`disc` false for none) once it's ready; the tray closes after both.
    void eject(uint32_t now_ms);
    void load(uint32_t now_ms, bool disc);
    // The timers and the classifier, each loop pass (or once a millisecond).
    void update(uint32_t now_ms, uint32_t now_us);

    State state(uint32_t now_ms) const;
    uint32_t spinUps() const { return spin_ups_; }
    uint32_t reads() const { return reads_count_; }

private:
    static activity::Config readsConfig() {
        activity::Config c;
        c.gap_forward = 16;  // in frames: 32 KB, as the disk's 64 sectors
        c.gap_backward = 2;
        return c;
    }
    void spin(uint32_t now_ms);

    Config cfg_;
    activity::Classifier reads_;
    bool disc_ = false;
    bool motor_ = false;
    bool tray_open_ = false;
    bool closing_ = false;      // the next disc is in: close once the tray has been open long enough
    bool active_ = false;       // a read started and hasn't ended
    bool last_was_read_ = false;  // the command in progress is a read (ends don't say whose they are)
    uint32_t opened_ms_ = 0;
    uint32_t last_read_ms_ = 0;  // a read's start or end
    uint32_t busy_until_ms_ = 0;
    uint32_t spin_ups_ = 0;
    uint32_t reads_count_ = 0;
};

}  // namespace optical
