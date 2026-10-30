// The activity link to an HDD Synth (docs/activity-link.md): the drive's
// activity as one-line sentences on a one-way UART, so the synth can play a
// different sound for sequential transfers than for random access.
//
//   $HDA,1,<busy>,<class>,<op>*<checksum>\r\n
//
// busy is 0 or 1; class is I, S or R (idle, sequential, random); op is R, W
// or - (the last media command read, wrote, or neither yet); the checksum is
// two upper-case hex digits, the XOR of every byte between $ and *, as in
// NMEA. A sentence goes out on every change, and every 200 ms regardless.
//
// Version 2 adds $HDE, where each media command takes the head, for the
// synth's modelled drives (docs/hddsynth-head-position-brief.md):
//
//   $HDE,2,<pos>,<op>,<sectors>*<checksum>\r\n
//
// pos is the command's first LBA as a fraction of the disk, 0-65535; op as
// $HDA's; sectors the command's sector count. At most one every 2 ms, the
// newest; $HDA goes first when one is due.
//
// Pure C++, host-tested; main.cpp writes the sentences to the UART.
#pragma once

#include <cstddef>
#include <cstdint>

#include "activity.h"

namespace activity_link {

enum class Op : uint8_t { None, Read, Write };

// Read or write, for a media command (activity::isMediaCommand); None for
// the rest (verifies and seeks move no data).
Op opOf(uint8_t command);

struct Fields {
    bool busy = false;
    activity::State state = activity::State::Idle;
    Op op = Op::None;
    bool operator==(const Fields& o) const { return busy == o.busy && state == o.state && op == o.op; }
    bool operator!=(const Fields& o) const { return !(*this == o); }
};

// The longest sentence, with \r\n and a terminating NUL.
constexpr size_t SENTENCE_MAX = 24;

// Writes the sentence for `f` into `buf` (NUL-terminated). Returns its
// length without the NUL, or 0 if `size` is too small.
size_t formatSentence(const Fields& f, char* buf, size_t size);

// The longest line a receiver accepts, without its line ending. (Not
// LINE_MAX: macOS's <limits.h> defines that as a macro.)
constexpr size_t MAX_LINE = 80;

// Validates one received line, without its line ending, as a receiver must
// (docs/activity-link.md; the reference is host/hddproto/activity_link.py):
// it starts with $HDA, its checksum matches (either case of hex), it has a
// version number, and busy, class and op are valid; fields after op are
// ignored, for later versions. Writes the fields to `out` if so.
bool parseLine(const char* line, size_t len, Fields& out);

// Assembles bytes into lines and validates each with parseLine(), counting
// the valid and the rejected ones. Lines end with \n, with or without \r;
// empty lines are skipped, and a line over MAX_LINE is rejected whole. The
// console's loopback test (`U`) uses it, and the HDD Synth's receiver can
// be a copy.
class Receiver {
public:
    // Feeds one byte. Returns true when it completes a valid sentence, whose
    // fields are written to `out`.
    bool feed(char c, Fields& out);
    // Forgets any partial line and zeroes the counts.
    void reset() { *this = Receiver(); }
    uint32_t valid() const { return valid_; }
    uint32_t rejected() const { return rejected_; }

private:
    char line_[MAX_LINE + 1];  // room for a trailing \r
    size_t len_ = 0;
    bool overlong_ = false;
    uint32_t valid_ = 0;
    uint32_t rejected_ = 0;
};

// Decides when to send: on any change of the fields, or once `heartbeat_ms`
// has passed since the last sentence that went out.
class Sender {
public:
    explicit Sender(uint32_t heartbeat_ms) : heartbeat_ms_(heartbeat_ms) {}

    // Is a sentence for `f` due at `now_ms`?
    bool due(const Fields& f, uint32_t now_ms) const {
        return !sent_any_ || f != last_ || now_ms - last_ms_ >= heartbeat_ms_;
    }
    // A sentence for `f` went out at `now_ms`. If one couldn't be written
    // (the UART was full), don't call this: it stays due, and the next try
    // sends whatever is current by then.
    void sent(const Fields& f, uint32_t now_ms) {
        last_ = f;
        last_ms_ = now_ms;
        sent_any_ = true;
        ++count_;
    }
    uint32_t count() const { return count_; }
    const Fields& last() const { return last_; }

private:
    uint32_t heartbeat_ms_;
    bool sent_any_ = false;
    Fields last_;
    uint32_t last_ms_ = 0;
    uint32_t count_ = 0;
};

// ---- Version 2: $HDE, head targets -------------------------------------------

struct HeadTarget {
    uint16_t pos = 0;  // 0 (LBA 0) to 65535 (the last LBA)
    Op op = Op::None;
    uint32_t sectors = 0;
    bool operator==(const HeadTarget& o) const { return pos == o.pos && op == o.op && sectors == o.sectors; }
};

// `lba` as a fraction of a disk of `total_sectors`: lba * 65535 / (total - 1).
uint16_t headPosition(uint32_t lba, uint32_t total_sectors);

// The longest $HDE ("$HDE,2,65535,R,256*hh"), with \r\n and a NUL.
constexpr size_t HEAD_SENTENCE_MAX = 32;

// As formatSentence(), for a head target.
size_t formatHeadSentence(const HeadTarget& t, char* buf, size_t size);

// Validates a received $HDE line, without its line ending, as parseLine()
// does a $HDA (the loopback test reads back both).
bool parseHeadLine(const char* line, size_t len, HeadTarget& out);

// Decides when a head target goes out: the newest one posted, once its time
// has come (when timing holds a command, the seek starts later than the
// command arrives) and at least `min_gap_us` after the last one sent. A
// target posted before the last is sent replaces it: a real head heads for
// the newest, and the synth's does too.
class HeadPacer {
public:
    explicit HeadPacer(uint32_t min_gap_us) : min_gap_us_(min_gap_us) {}

    void post(const HeadTarget& t, uint64_t not_before_us) {
        if (pending_) ++replaced_;
        target_ = t;
        not_before_us_ = not_before_us;
        pending_ = true;
    }
    bool due(uint64_t now_us) const {
        return pending_ && now_us >= not_before_us_ && (!sent_any_ || now_us - last_us_ >= min_gap_us_);
    }
    const HeadTarget& target() const { return target_; }
    // The target went out at `now_us`. As with Sender, call this only if it
    // was written; otherwise it stays due.
    void sent(uint64_t now_us) {
        pending_ = false;
        sent_any_ = true;
        last_us_ = now_us;
        last_ = target_;
        ++count_;
    }
    uint32_t count() const { return count_; }
    uint32_t replaced() const { return replaced_; }  // posted, then overtaken before going out
    const HeadTarget& last() const { return last_; }

private:
    uint32_t min_gap_us_;
    HeadTarget target_;
    HeadTarget last_;
    uint64_t not_before_us_ = 0;
    uint64_t last_us_ = 0;
    bool pending_ = false;
    bool sent_any_ = false;
    uint32_t count_ = 0;
    uint32_t replaced_ = 0;
};

// Which sentence goes out next on the shared UART: a due $HDA first (a
// change, or the heartbeat that keeps the link alive), then a due $HDE.
enum class Next : uint8_t { Nothing, Activity, Head };
inline Next nextSentence(const Sender& activity, const Fields& f, uint32_t now_ms, const HeadPacer& head,
                         uint64_t now_us) {
    if (activity.due(f, now_ms)) return Next::Activity;
    if (head.due(now_us)) return Next::Head;
    return Next::Nothing;
}

}  // namespace activity_link
