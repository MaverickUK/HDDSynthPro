#include "seek_timing.h"

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

void Scheduler::setModel(const SeekModel* m) {
    has_model_ = m != nullptr;
    if (m) model_ = *m;
}

void Scheduler::setGeometry(uint32_t total_sectors, uint32_t sectors_per_track) {
    total_sectors_ = total_sectors ? total_sectors : 1;
    sectors_per_track_ = sectors_per_track ? sectors_per_track : 1;
}

Release Scheduler::onSector(uint64_t now_us, uint32_t command_lba, uint32_t lba) {
    uint32_t k = lba - command_lba;
    bool first = !in_command_ || command_lba != command_lba_ || lba < command_lba || k <= last_k_;
    Release r{now_us, first, now_us};

    if (first) {
        bool carries_on = command_lba == next_lba_ && head_free_at_ != 0;
        float x = lbaToStroke(float(command_lba) / float(total_sectors_), model_.zoned);
        in_command_ = true;
        command_held_ = command_capped_ = false;
        command_us_ = 0;
        command_lba_ = command_lba;
        t0_ = now_us;
        if (has_model_) {
            ++commands_;
            if (carries_on) {
                // Reading straight on: sector 0 passes the head as the last
                // command's last sector ends, or up to a turn before now.
                uint64_t rev = revolutionUs(model_);
                base_ = head_free_at_;
                if (now_us > rev && base_ < now_us - rev) base_ = now_us - rev;
                r.seek_start_us = now_us;
            } else {
                uint64_t start = now_us > head_free_at_ ? now_us : head_free_at_;
                uint64_t arrive = start + seekTimeUs(model_, head_x_, x);
                base_ = arrive + rotationalWaitUs(model_, arrive, command_lba, sectors_per_track_);
                r.seek_start_us = start;
            }
        }
        head_x_ = x;
    }
    last_k_ = k;
    next_lba_ = lba + 1;

    if (!has_model_) {
        head_free_at_ = now_us;
        return r;
    }
    uint64_t ready = base_ + transferUs(model_, k + 1, sectors_per_track_);
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
    if (ready > head_free_at_) head_free_at_ = ready;
    uint32_t us = ready > t0_ ? uint32_t(ready - t0_) : 0;
    if (us > command_us_) {
        total_us_ += us - command_us_;
        command_us_ = us;
        if (us > max_us_) max_us_ = us;
    }
    return r;
}

}  // namespace seek_timing
