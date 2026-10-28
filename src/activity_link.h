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

}  // namespace activity_link
