// The last few commands, from their CMD and DONE events, so core 0 can say
// which command a bus event it learnt of separately (a read the host didn't
// wait for IORDY on) fell in or followed. Times are core 1's time_us_32(),
// as the trace events carry them; comparisons allow for its wrap.
//
// Pure C++, host-tested (test_command_history.cpp).
#pragma once

#include <cstdint>

#include "trace_format.h"

class CommandHistory {
public:
    struct Entry {
        uint32_t start_us = 0, end_us = 0;
        bool ended = false;
        uint8_t command = 0, packet = 0, unit = 0;
        uint16_t count = 0;
    };

    // Feed every trace event; only command starts and ends are kept.
    void note(const TraceEvent& e) {
        if (e.kind == TraceKind::CommandStart) {
            head_ = (head_ + 1) % SIZE;
            if (size_ < SIZE) ++size_;
            Entry& n = entries_[head_];
            n = Entry{};
            n.start_us = e.time_us;
            n.command = uint8_t(e.extra);
            n.packet = uint8_t(e.extra >> 24);
            n.unit = e.unit;
            n.count = commandCount(e);
        } else if (e.kind == TraceKind::CommandEnd && size_ && !entries_[head_].ended) {
            entries_[head_].ended = true;  // commands run one at a time: the latest one ended
            entries_[head_].end_us = e.time_us;
        }
    }

    // The latest command that started at or before `at_us`, or nullptr if
    // none kept did (or the event came before all of them).
    const Entry* at(uint32_t at_us) const {
        for (unsigned i = 0; i < size_; ++i) {
            const Entry& e = entries_[(head_ + SIZE - i) % SIZE];
            if (int32_t(at_us - e.start_us) >= 0) return &e;
        }
        return nullptr;
    }

    void clear() { size_ = 0; }

    static constexpr unsigned SIZE = 16;

private:
    Entry entries_[SIZE];
    unsigned head_ = 0, size_ = 0;
};
