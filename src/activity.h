// Classifies the drive's command stream as Idle, Sequential or Random:
// what an HDD Synth needs to pick a sound. Pure C++, host-tested.
//
// The existing HDD Synth only sees the host's activity LED (busy or not).
// The emulated drive sees every command's start LBA and length, so it can
// tell a head streaming along a track from one jumping about, and how far
// each jump goes (the seek distance, which decides how a real seek sounds).
//
// How it works:
//   1. Each media command is sequential if it starts at (or just after) where
//      one of a few recent streams ended. Tracking several streams keeps two
//      interleaved sequential reads (two files copied at once) sequential.
//      Anything else is a seek; its distance is from where the head last was.
//   2. Over a sliding window, the share of seeks among recent commands
//      decides the state, with hysteresis (separate enter thresholds) and a
//      minimum dwell before switching between Sequential and Random, so the
//      sound doesn't flicker. Going idle, or waking from idle, is immediate.
//   3. Every burst of activity starts with a seek to wherever the data is.
//      That first command is neutral: it doesn't count towards the seek
//      share (its seek distance is still reported). Waking, the state is
//      Sequential until at least `min_commands` others are in the window,
//      and for one window after waking it follows the share without the
//      dwell, so a random burst is recognised within a few commands.
//
// PATA has no command queueing, so the order commands arrive in is the
// order a real drive's head would move.
#pragma once

#include <cstddef>
#include <cstdint>

namespace activity {

enum class State : uint8_t { Idle, Sequential, Random };

const char* stateName(State s);

struct Config {
    uint32_t window_us = 250000;         // commands considered for the state
    uint32_t idle_us = 150000;           // no commands this long: Idle
    uint32_t dwell_us = 100000;          // a new Sequential/Random state must hold this long
    uint8_t enter_sequential_pct = 25;   // seeks at or below this share: Sequential
    uint8_t enter_random_pct = 50;       // seeks at or above this share: Random
    uint8_t min_commands = 3;            // fewer counted commands than this: no Random yet
    uint32_t gap_forward = 64;           // a start up to this many sectors past a stream's end is sequential
    uint32_t gap_backward = 8;           // ...or this many before it (re-reads)
    uint32_t stream_timeout_us = 2000000;  // a stream unused this long is forgotten
};

// What one command was.
struct Classified {
    bool sequential;
    uint32_t seek;  // sectors from the head's last position (0 if sequential)
};

// Is this ATA command a media access worth classifying (reads, writes,
// verifies, seeks)? IDENTIFY, SET FEATURES and the like are not.
bool isMediaCommand(uint8_t command);

class Classifier {
public:
    explicit Classifier(const Config& config = Config()) : cfg_(config) {}

    // A media command started at `t_us`, covering `count` sectors from `lba`.
    // A seek-only command (SEEK, RECALIBRATE) passes count 0.
    Classified onCommand(uint32_t t_us, uint32_t lba, uint32_t count);

    // The media command in progress ended. Until then the drive counts as
    // busy, so a long transfer never reads as Idle.
    void onCommandEnd(uint32_t t_us);

    // Re-evaluates the state at `now_us`. Returns true if it changed.
    bool update(uint32_t now_us);

    State state() const { return state_; }
    uint8_t seekPercent() const { return seek_pct_; }     // in the window, at the last update
    uint32_t averageSeek() const { return avg_seek_; }    // sectors, over the window's seeks
    uint32_t sequentialCommands() const { return total_seq_; }
    uint32_t randomCommands() const { return total_rand_; }

private:
    static constexpr size_t STREAMS = 4;
    static constexpr size_t HISTORY = 64;  // must hold a window's worth of commands

    struct Stream {
        bool used = false;
        uint32_t next = 0;     // where it would continue
        uint32_t last_us = 0;
    };
    struct Record {
        uint32_t t_us;
        bool sequential;
        bool neutral;  // the first command after idle: not counted
        uint32_t seek;
    };

    Config cfg_;
    Stream streams_[STREAMS];
    Record history_[HISTORY] = {};
    size_t history_count_ = 0;  // valid records (up to HISTORY)
    size_t history_next_ = 0;   // where the next record goes
    bool have_head_ = false;
    uint32_t head_ = 0;          // the head's position: where the last command ended
    uint32_t last_command_us_ = 0;  // last start or end of a media command
    bool busy_ = false;             // a media command has started and not ended

    State state_ = State::Idle;
    State pending_ = State::Idle;
    uint32_t pending_since_us_ = 0;
    uint32_t woke_us_ = 0;  // when the state last left Idle
    uint8_t seek_pct_ = 0;
    uint32_t avg_seek_ = 0;
    uint32_t total_seq_ = 0;
    uint32_t total_rand_ = 0;
};

// One trace line for the current state, with a trailing newline:
//   ACT <t_us> <IDLE|SEQ|RAND> <seek_pct> <avg_seek_sectors>
// seek_pct is the share of seeks among the window's commands; avg_seek is
// their mean distance in sectors (0 when there were none).
// Returns the length (truncated to size - 1 if needed).
size_t formatActivity(uint32_t t_us, const Classifier& c, char* buf, size_t size);

}  // namespace activity
