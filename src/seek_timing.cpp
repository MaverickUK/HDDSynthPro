#include "seek_timing.h"

#include "core1_ram.h"  // CORE0_RAM

#include <cstdio>
#include <cstring>

namespace seek_timing {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool equalsNoCase(const char* text, size_t len, const char* word) {
    if (std::strlen(word) != len) return false;
    for (size_t i = 0; i < len; ++i)
        if (lower(text[i]) != word[i]) return false;
    return true;
}

}  // namespace

bool parseSetting(const char* text, size_t len, Setting& out) {
    if (equalsNoCase(text, len, "off")) {
        out = Setting{};
        return true;
    }
    if (equalsNoCase(text, len, "synth")) {
        out = Setting{};
        out.source = Source::Synth;
        return true;
    }
    for (size_t i = 0; i < drivePresetCount(); ++i) {
        const char* id = drivePresetId(i);
        if (!equalsNoCase(text, len, id)) continue;
        out = Setting{};
        out.source = Source::Preset;
        std::snprintf(out.preset, sizeof out.preset, "%s", id);
        return true;
    }
    return false;
}

const char* settingName(const Setting& s) {
    switch (s.source) {
        case Source::Synth: return "synth";
        case Source::Preset: return s.preset;
        default: return "off";
    }
}

bool SynthModel::onLine(const char* line, size_t len, uint32_t now_ms) {
    SeekModel m;
    bool present = false;
    if (!parseModelSentence(line, len, m, present)) {
        ++rejected_;
        return false;
    }
    if (present) model_ = m;
    present_ = present;
    heard_ = true;
    last_ms_ = now_ms;
    ++valid_;
    return true;
}

const SeekModel* SynthModel::model(uint32_t now_ms) const {
    return heard(now_ms) && present_ ? &model_ : nullptr;
}

uint32_t cacheSectorsOf(const SeekModel& m) {
    if (m.actuator == Actuator::Stepper) return 0;
    if (m.rpm < 5400) return 256;    // 128 KB
    if (m.rpm <= 5400) return 1024;  // 512 KB
    return 4096;                     // 2 MB
}

uint32_t trackSectorsOf(const SeekModel& m) {
    if (m.cylinders < 800) return 17;
    if (m.cylinders < 2000) return 40;
    if (m.cylinders < 20000) return 200;
    return 900;
}

void Scheduler::setModel(const SeekModel* m) {
    has_model_ = m != nullptr;
    if (m) {
        model_ = *m;
        cache_sectors_ = cacheSectorsOf(*m);
        sectors_per_track_ = trackSectorsOf(*m);
    } else {
        run_count_ = 0;  // timing off: nothing waits to be written
        pend_sectors_ = 0;
        seg_valid_ = false;
    }
}

void Scheduler::setGeometry(uint32_t total_sectors, uint32_t sectors_per_track) {
    total_sectors_ = total_sectors ? total_sectors : 1;
    sectors_per_track_ = sectors_per_track ? sectors_per_track : 1;
}

CORE0_RAM uint64_t Scheduler::readAheadAt(uint32_t sector) const {
    return seg_base_ + transferUs(model_, sector - seg_lba_ + 1, sectors_per_track_);
}

// The oldest run of cached writes, written from `start` (the head free): it
// leaves the read-ahead there, seeks, waits for the platter, writes.
CORE0_RAM uint64_t Scheduler::flushRun(uint64_t start, Release& r) {
    const Run run = runs_[run_first_];
    run_first_ = (run_first_ + 1) % WRITE_RUNS;
    --run_count_;
    pend_sectors_ -= run.sectors;
    if (seg_valid_ && seg_end_ > start) seg_end_ = start;
    float x = lbaToStroke(float(run.lba) / float(total_sectors_), model_.zoned);
    uint64_t arrive = start + seekTimeUs(model_, head_x_, x);
    uint64_t done = arrive + rotationalWaitUs(model_, arrive, run.lba, sectors_per_track_) +
                    transferUs(model_, run.sectors, sectors_per_track_);
    head_x_ = x;
    head_free_at_ = done;
    next_lba_ = 0xFFFFFFFFu;  // the head isn't where the last command left it
    ++flushes_;
    r.flushed = true;
    r.flush_lba = run.lba;
    r.flush_sectors = run.sectors;
    r.flush_at_us = start;
    return done;
}

// Runs written out while the drive sat idle before `now` (IDLE_FLUSH_US
// after the head was last free), each one starting only if it could start
// before `now`.
CORE0_RAM void Scheduler::flushIdle(uint64_t now, Release& r) {
    while (run_count_ && head_free_at_ + IDLE_FLUSH_US <= now) flushRun(head_free_at_ + IDLE_FLUSH_US, r);
}

CORE0_RAM uint64_t Scheduler::flushAll(uint64_t now_us, Release* out) {
    Release r{now_us, false, now_us};
    uint64_t done = now_us > head_free_at_ ? now_us : head_free_at_;
    if (has_model_) {
        flushIdle(now_us, r);
        while (run_count_) done = flushRun(now_us > head_free_at_ ? now_us : head_free_at_, r);
        if (head_free_at_ > done) done = head_free_at_;
    }
    if (done > now_us + MAX_HOLD_US) done = now_us + MAX_HOLD_US;
    if (out) *out = r;
    return done;
}

CORE0_RAM Release Scheduler::onSector(uint64_t now_us, uint32_t command_lba, uint32_t lba, bool write,
                                      uint32_t command_sectors) {
    uint32_t k = lba - command_lba;
    bool first = !in_command_ || command_lba != command_lba_ || lba < command_lba || k <= last_k_;
    Release r{now_us, first, now_us};

    if (first) {
        bool carries_on = command_lba == next_lba_ && head_free_at_ != 0;
        float x = lbaToStroke(float(command_lba) / float(total_sectors_), model_.zoned);
        in_command_ = true;
        command_held_ = command_capped_ = false;
        command_cached_write_ = false;
        command_us_ = 0;
        command_lba_ = command_lba;
        t0_ = now_us;
        if (!has_model_) {
            head_x_ = x;
        } else {
            ++commands_;
            flushIdle(now_us, r);
            const uint32_t n = command_sectors ? command_sectors : 1;
            if (write && cache_sectors_ && n <= cache_sectors_) {
                // Into the write cache: room made first if it's full (the
                // host waits for that), then released at once.
                uint64_t ready = now_us;
                while (run_count_ && (pend_sectors_ + n > cache_sectors_ || run_count_ == WRITE_RUNS)) {
                    uint64_t start = now_us > head_free_at_ ? now_us : head_free_at_;
                    ready = flushRun(start, r);
                }
                const Run* last = run_count_ ? &runs_[(run_first_ + run_count_ - 1) % WRITE_RUNS] : nullptr;
                if (last && last->lba + last->sectors == command_lba) {
                    runs_[(run_first_ + run_count_ - 1) % WRITE_RUNS].sectors += n;  // straight on: one run
                } else {
                    runs_[(run_first_ + run_count_) % WRITE_RUNS] = Run{command_lba, n};
                    ++run_count_;
                }
                pend_sectors_ += n;
                ++cached_writes_;
                command_cached_write_ = true;
                command_ready_ = ready;
                r.moves = false;
            } else if (!write && cache_sectors_ && seg_valid_ && command_lba >= seg_lba_ &&
                       readAheadAt(command_lba) <= now_us + revolutionUs(model_) &&
                       readAheadAt(command_lba + n - 1) <= seg_end_) {
                // From the read-ahead: passed already, or about to be. It
                // runs at most cacheSectors ahead of the host: a slow host
                // finds that many ready, the rest still to come.
                const uint64_t keep = transferUs(model_, cache_sectors_, sectors_per_track_);
                base_ = readAheadAt(command_lba) - transferUs(model_, 1, sectors_per_track_);
                if (now_us > keep && base_ < now_us - keep) base_ = now_us - keep;
                ++hits_;
                r.moves = false;
                r.seek_start_us = now_us;
            } else if (carries_on && !write) {
                // Straight on, after a write-through or the read-ahead lost:
                // as the head reads on from where it was.
                uint64_t rev = revolutionUs(model_);
                base_ = head_free_at_;
                if (now_us > rev && base_ < now_us - rev) base_ = now_us - rev;
                r.seek_start_us = now_us;
            } else {
                uint64_t start = now_us > head_free_at_ ? now_us : head_free_at_;
                if (carries_on) {
                    base_ = start;  // a write straight on from the last
                } else {
                    uint64_t arrive = start + seekTimeUs(model_, head_x_, x);
                    base_ = arrive + rotationalWaitUs(model_, arrive, command_lba, sectors_per_track_);
                }
                r.seek_start_us = start;
                head_x_ = x;
                if (!write) {  // the read-ahead starts here
                    seg_valid_ = true;
                    seg_lba_ = command_lba;
                    seg_base_ = base_;
                    seg_end_ = ~uint64_t(0);
                } else if (seg_valid_ && seg_end_ > start) {
                    seg_end_ = start;  // the head left the read-ahead
                }
            }
        }
    }
    last_k_ = k;
    if (!command_cached_write_) next_lba_ = lba + 1;

    if (!has_model_) {
        head_free_at_ = now_us;
        return r;
    }
    uint64_t ready = command_cached_write_ ? command_ready_ : base_ + transferUs(model_, k + 1, sectors_per_track_);
    if (ready > t0_ + MAX_HOLD_US) {
        ready = t0_ + MAX_HOLD_US;
        if (!command_capped_) ++capped_;
        command_capped_ = true;
    }
    if (ready > now_us && !command_held_) {
        command_held_ = true;
        ++held_;
    }
    if (r.seek_start_us > t0_ + MAX_HOLD_US) r.seek_start_us = t0_ + MAX_HOLD_US;
    r.ready_us = ready;
    if (!command_cached_write_ && ready > head_free_at_) head_free_at_ = ready;
    uint32_t us = ready > t0_ ? uint32_t(ready - t0_) : 0;
    if (us > command_us_) {
        total_us_ += us - command_us_;
        command_us_ = us;
        if (us > max_us_) max_us_ = us;
    }
    return r;
}

}  // namespace seek_timing
