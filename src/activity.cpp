#include "activity.h"

#include <cstdio>

namespace activity {

const char* stateName(State s) {
    switch (s) {
        case State::Sequential: return "SEQ";
        case State::Random: return "RAND";
        default: return "IDLE";
    }
}

bool isMediaCommand(uint8_t c) {
    switch (c) {
        case 0x20: case 0x21:  // READ SECTORS
        case 0x30: case 0x31:  // WRITE SECTORS
        case 0x40: case 0x41:  // READ VERIFY
        case 0xC4: case 0xC5:  // READ / WRITE MULTIPLE
        case 0xC8: case 0xC9:  // READ DMA
        case 0xCA: case 0xCB:  // WRITE DMA
            return true;
        default:
            return (c >= 0x10 && c <= 0x1F) || (c >= 0x70 && c <= 0x7F);  // RECALIBRATE, SEEK
    }
}

static uint32_t distance(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

Classified Classifier::onCommand(uint32_t t_us, uint32_t lba, uint32_t count) {
    // The first command after idle: the seek that starts a burst.
    bool neutral = !busy_ && (history_count_ == 0 || t_us - last_command_us_ > cfg_.idle_us);

    // Forget streams nobody has continued for a while.
    for (Stream& s : streams_)
        if (s.used && t_us - s.last_us > cfg_.stream_timeout_us) s.used = false;

    // Sequential if it carries on one of the streams.
    Stream* match = nullptr;
    for (Stream& s : streams_) {
        if (!s.used) continue;
        bool ahead = lba >= s.next && lba - s.next <= cfg_.gap_forward;
        bool behind = lba < s.next && s.next - lba <= cfg_.gap_backward;
        if (ahead || behind) {
            match = &s;
            break;
        }
    }

    Classified result{match != nullptr, 0};
    if (!match) {
        // A seek: the head moves from where the last command left it.
        result.seek = have_head_ ? distance(lba, head_) : lba;
        // Start a new stream in a free slot, or the least recently used one.
        match = &streams_[0];
        for (Stream& s : streams_) {
            if (!s.used) {
                match = &s;
                break;
            }
            if (t_us - s.last_us > t_us - match->last_us) match = &s;
        }
        match->used = true;
        ++total_rand_;
    } else {
        ++total_seq_;
    }
    match->next = lba + count;
    match->last_us = t_us;
    head_ = lba + count;
    have_head_ = true;
    last_command_us_ = t_us;
    busy_ = true;

    history_[history_next_] = Record{t_us, result.sequential, neutral, result.seek};
    history_next_ = (history_next_ + 1) % HISTORY;
    if (history_count_ < HISTORY) ++history_count_;
    return result;
}

void Classifier::onCommandEnd(uint32_t t_us) {
    if (!busy_) return;
    busy_ = false;
    last_command_us_ = t_us;
}

bool Classifier::update(uint32_t now_us) {
    // Window statistics.
    uint32_t seq = 0, rand = 0;
    uint64_t seek_sum = 0;
    for (size_t i = 0; i < history_count_; ++i) {
        const Record& r = history_[i];
        if (r.neutral || now_us - r.t_us > cfg_.window_us) continue;
        if (r.sequential) {
            ++seq;
        } else {
            ++rand;
            seek_sum += r.seek;
        }
    }
    uint32_t total = seq + rand;
    seek_pct_ = total ? uint8_t(rand * 100 / total) : 0;
    avg_seek_ = rand ? uint32_t(seek_sum / rand) : 0;

    // What the state should be now.
    State want;
    if (history_count_ == 0 || (!busy_ && now_us - last_command_us_ > cfg_.idle_us)) {
        want = State::Idle;
    } else if (total < cfg_.min_commands) {
        // Too few to judge (just woken, or one long command): stay as we are,
        // and wake as Sequential.
        want = state_ != State::Idle ? state_ : State::Sequential;
    } else if (seek_pct_ <= cfg_.enter_sequential_pct) {
        want = State::Sequential;
    } else if (seek_pct_ >= cfg_.enter_random_pct) {
        want = State::Random;
    } else {
        // In between: keep what we have, or pick the nearer side when waking.
        want = state_ != State::Idle ? state_ : (seek_pct_ < 50 ? State::Sequential : State::Random);
    }

    if (want == state_) {
        pending_ = state_;
        return false;
    }
    // Going idle or waking up is immediate: the sound must start and stop
    // with the activity. Switching between Sequential and Random waits for
    // the new state to hold.
    if (want == State::Idle || state_ == State::Idle) {
        if (state_ == State::Idle) woke_us_ = now_us;
        state_ = pending_ = want;
        return true;
    }
    // Just woken: follow the share at once, while the window fills.
    if (now_us - woke_us_ < cfg_.window_us) {
        state_ = pending_ = want;
        return true;
    }
    if (pending_ != want) {
        pending_ = want;
        pending_since_us_ = now_us;
        return false;
    }
    if (now_us - pending_since_us_ >= cfg_.dwell_us) {
        state_ = want;
        return true;
    }
    return false;
}

size_t formatActivity(uint32_t t_us, const Classifier& c, char* buf, size_t size) {
    int n = std::snprintf(buf, size, "ACT %lu %s %u %lu\n", (unsigned long)t_us, stateName(c.state()),
                          (unsigned)c.seekPercent(), (unsigned long)c.averageSeek());
    if (n < 0) return 0;
    return (size_t)n < size ? (size_t)n : size - 1;
}

}  // namespace activity
