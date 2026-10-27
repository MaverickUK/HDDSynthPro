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
