// Host-side tests for authentic seek timing: the setting, the synth's model
// from $HDM, and the Scheduler that holds each command (seek_timing.*), fed
// a fake clock. Also the files copied from HDD Synth (sentence.*,
// drive_params.*, drive_timing.*), with the tests they have there, less the
// one that holds drive_timing to the synth's engine (not copied here).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "drive_params.h"
#include "drive_timing.h"
#include "seek_timing.h"
#include "sentence.h"

using seek_timing::Release;
using seek_timing::Scheduler;
using seek_timing::Setting;
using seek_timing::Source;
using seek_timing::SynthModel;

static std::string sentenceLine(const char* body) {
    char buf[96];
    size_t n = formatSentence(buf, sizeof buf, body);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

static SeekModel preset(const char* id) {
    DriveParams p;
    drivePreset(id, p);
    return seekModelOf(p);
}

// ---- Copied from HDD Synth's tests ----------------------------------------------

TEST(test_sentence_format_and_checksum) {
    char buf[64];
    CHECK_EQ(formatSentence(buf, sizeof(buf), "HDA,1,1,S,R"), 17u);
    CHECK(strcmp(buf, "$HDA,1,1,S,R*4C\r\n") == 0);
    CHECK_EQ(formatSentence(buf, 17, "HDA,1,1,S,R"), 0u);  // one byte short of room for the NUL
    CHECK_EQ(formatSentence(buf, 18, "HDA,1,1,S,R"), 17u);
    const char* body;
    size_t n;
    CHECK(sentenceBody("$HDA,1,0,I,-*28", 15, body, n));
    CHECK_EQ(n, 11u);
    CHECK(!sentenceBody("$HDA,1,0,I,-*27", 15, body, n));
}

TEST(test_sentence_fields) {
    Field f[3];
    const char* b = "HDC,1,VOL,55";
    CHECK_EQ(splitFields(b, strlen(b), f, 3), 4u);  // counted past `max`
    CHECK(f[0].is("HDC") && f[1].is("1") && f[2].is("VOL"));
    CHECK(!f[2].is("VO") && !f[2].is("VOLUME"));
    uint32_t v = 0;
    Field num{"0042", 4};
    CHECK(num.toUint(v) && v == 42);
    Field bad{"4x", 2}, empty{"", 0}, neg{"-1", 2};
    CHECK(!bad.toUint(v) && !empty.toUint(v) && !neg.toUint(v));
}

TEST(test_sentence_line_assembler) {
    LineAssembler a;
    std::vector<std::string> lines;
    int dropped = 0;
    auto feed = [&](const char* t) {
        for (const char* c = t; *c; ++c) {
            const LineAssembler::Result r = a.feed(*c);
            if (r == LineAssembler::Result::Line) lines.push_back(a.line());
            if (r == LineAssembler::Result::Dropped) ++dropped;
        }
    };
    feed("h$HDA,1,0,I,-*28\r\n");
    CHECK_EQ(lines.size(), 1u);
    CHECK(lines[0] == "$HDA,1,0,I,-*28");
    feed("$HDQ,1$HDQ,1,PACKS*26\n");  // cut off by a '$'
    CHECK_EQ(dropped, 1);
    CHECK(lines.size() == 2 && lines[1] == "$HDQ,1,PACKS*26");
    std::string longLine(81, 'x');
    feed(("$" + longLine + "\n").c_str());
    CHECK_EQ(dropped, 2);
    CHECK_EQ(lines.size(), 2u);
}

TEST(test_drive_presets) {
    CHECK_EQ(drivePresetCount(), size_t(4));
    DriveParams p;
    for (size_t i = 0; i < drivePresetCount(); ++i) CHECK(drivePreset(drivePresetId(i), p));
    CHECK(!drivePreset("nonsense", p));
    CHECK(drivePreset("stepper85", p));
    CHECK(p.actuator == Actuator::Stepper);
    CHECK_EQ(int(p.cylinders), 615);
}

TEST(test_drive_model_sentence_round_trip) {
    DriveParams p;
    for (size_t i = 0; i < drivePresetCount(); ++i) {
        drivePreset(drivePresetId(i), p);
        const SeekModel m = seekModelOf(p);
        char body[80];
        CHECK(formatModelBody(&m, body, sizeof(body)) > 0);
        const std::string l = sentenceLine(body);
        SeekModel back;
        bool present = false;
        CHECK(parseModelSentence(l.data(), l.size(), back, present));
        CHECK(present);
        CHECK(back.actuator == m.actuator);
        CHECK_EQ(int(back.rpm), int(m.rpm));
        CHECK_EQ(int(back.cylinders), int(m.cylinders));
        CHECK(std::fabs(back.fullStrokeMs - m.fullStrokeMs) < 0.051f);
        CHECK(std::fabs(back.trackMs - m.trackMs) < 0.051f);
        CHECK(std::fabs(back.settleDamp - m.settleDamp) < 0.0051f);
        CHECK(std::fabs(back.stepMs - m.stepMs) < 0.051f);
        CHECK(std::fabs(back.stepRamp - m.stepRamp) < 0.0051f);
        CHECK(back.zoned == m.zoned);
        CHECK(l.size() <= SENTENCE_MAX_LINE);
    }
}

TEST(test_drive_timing_rotation_and_transfer) {
    SeekModel m;
    m.rpm = 6000;  // 10 ms per revolution
    CHECK_EQ(int(revolutionUs(m)), 10000);
    CHECK_EQ(int(rotationalWaitUs(m, 0, 25, 100)), 2500);
    CHECK_EQ(int(rotationalWaitUs(m, 2500, 25, 100)), 0);
    CHECK_EQ(int(rotationalWaitUs(m, 3000, 25, 100)), 9500);
    CHECK_EQ(int(rotationalWaitUs(m, 1000000, 125, 100)), 2500);
    CHECK_EQ(int(transferUs(m, 50, 100)), 5000);
    CHECK(lbaToStroke(0.5f, true) < 0.42f);
    CHECK(lbaToStroke(0.5f, true) > 0.40f);
    CHECK(std::fabs(lbaToStroke(0.5f, false) - 0.5f) < 1e-6f);
    CHECK(lbaToStroke(0, true) < 1e-6f);
    CHECK_EQ(int(seekTimeUs(m, 0.5f, 0.5f)), 0);  // the same track
}

// ---- The setting -------------------------------------------------------------------

static bool parse(const char* text, Setting& s) { return seek_timing::parseSetting(text, std::strlen(text), s); }

TEST(test_seek_timing_setting) {
    Setting s;
    CHECK(parse("off", s) && s.source == Source::Off);
    CHECK(parse("Synth", s) && s.source == Source::Synth);
    CHECK_STR(seek_timing::settingName(s), "synth");
    CHECK(parse("VOICE97", s) && s.source == Source::Preset);
    CHECK_STR(s.preset, "voice97");  // the preset's own spelling
    CHECK_STR(seek_timing::settingName(s), "voice97");
    for (size_t i = 0; i < drivePresetCount(); ++i) CHECK(parse(drivePresetId(i), s));
    CHECK(!parse("on", s));
    CHECK(!parse("voice9", s));
    CHECK(!parse("", s));
    CHECK_STR(seek_timing::settingName(s), "fdb05");  // a refusal leaves it alone
}

// ---- $HDM ----------------------------------------------------------------------------

TEST(test_hdm_example_is_voice97) {
    SynthModel sm;
    const char* line = "$HDM,2,V,5400,6000,190,20,45,30,0,1*1B";
    CHECK(sm.onLine(line, std::strlen(line), 1000));
    const SeekModel* m = sm.model(1000);
    CHECK(m != nullptr);
    if (!m) return;
    SeekModel want = preset("voice97");
    CHECK(m->actuator == want.actuator);
    CHECK_EQ(int(m->rpm), int(want.rpm));
    CHECK_EQ(int(m->cylinders), int(want.cylinders));
    CHECK(std::fabs(m->fullStrokeMs - want.fullStrokeMs) < 0.01f);
    CHECK(std::fabs(m->trackMs - want.trackMs) < 0.01f);
    CHECK(std::fabs(m->settleDamp - want.settleDamp) < 0.001f);
    CHECK(m->zoned == want.zoned);
    // So it times as the preset does.
    CHECK_EQ(seekTimeUs(*m, 0, 1), seekTimeUs(want, 0, 1));
}

TEST(test_hdm_none_and_rejects) {
    SynthModel sm;
    std::string none = sentenceLine("HDM,2,-");
    CHECK(sm.onLine(none.data(), none.size(), 0));
    CHECK(sm.heard(0));
    CHECK(sm.model(0) == nullptr);  // a recorded pack: no model

    const char* bad_sum = "$HDM,2,V,5400,6000,190,20,45,30,0,1*1C";
    CHECK(!sm.onLine(bad_sum, std::strlen(bad_sum), 0));
    std::string few = sentenceLine("HDM,2,V,5400,6000");
    CHECK(!sm.onLine(few.data(), few.size(), 0));
    std::string other = sentenceLine("HDA,1,0,I,-");
    CHECK(!sm.onLine(other.data(), other.size(), 0));
    sm.onDropped();
    CHECK_EQ(sm.valid(), 1u);
    CHECK_EQ(sm.rejected(), 4u);

    // A later version, with a field added: still read.
    std::string later = sentenceLine("HDM,3,S,3600,615,0,0,0,30,40,0,99");
    CHECK(sm.onLine(later.data(), later.size(), 0));
    CHECK(sm.model(0) != nullptr && sm.model(0)->actuator == Actuator::Stepper);
}

TEST(test_hdm_times_out) {
    SynthModel sm;
    CHECK(sm.model(0) == nullptr);  // nothing heard yet
    CHECK(!sm.heard(0));
    const char* line = "$HDM,2,V,5400,6000,190,20,45,30,0,1*1B";
    CHECK(sm.onLine(line, std::strlen(line), 10000));
    CHECK(sm.model(10000 + seek_timing::MODEL_TIMEOUT_MS - 1) != nullptr);
    CHECK(sm.model(10000 + seek_timing::MODEL_TIMEOUT_MS) == nullptr);
    CHECK(!sm.heard(10000 + seek_timing::MODEL_TIMEOUT_MS));
    CHECK(sm.onLine(line, std::strlen(line), 20000));  // back
    CHECK(sm.model(20000) != nullptr);
}

// ---- The scheduler -------------------------------------------------------------------

constexpr uint32_t TOTAL = 131072;  // the generated disk
constexpr uint32_t SPT = 63;

static Scheduler scheduler(const SeekModel& m) {
    Scheduler s;
    s.setGeometry(TOTAL, SPT);
    s.setModel(&m);
    return s;
}

// A command of `n` sectors at `lba`, every sector reaching core 0 at `now`.
// Returns the last sector's release.
static Release command(Scheduler& s, uint64_t now, uint32_t lba, uint32_t n, Release* first = nullptr) {
    Release r{};
    for (uint32_t k = 0; k < n; ++k) {
        r = s.onSector(now, lba, lba + k);
        if (k == 0 && first) *first = r;
    }
    return r;
}

TEST(test_hold_same_track_no_seek) {
    SeekModel m = preset("voice97");
    Scheduler s = scheduler(m);
    uint64_t now = 1000000;
    Release r = command(s, now, 50000, 1);
    now = r.ready_us + 5000;
    // Another sector a few LBAs on (same track), not straight on: no seek,
    // only the platter.
    Release first;
    command(s, now, 50010, 1, &first);
    CHECK(first.first);
    CHECK_EQ(first.seek_start_us, now);
    CHECK_EQ(first.ready_us, now + rotationalWaitUs(m, now, 50010, SPT) + transferUs(m, 1, SPT));
}

TEST(test_hold_full_stroke) {
    SeekModel m = preset("voice97");
    Scheduler s = scheduler(m);
    uint64_t now = 2000000;
    Release r = command(s, now, 0, 1);  // the head to the outer edge
    now = r.ready_us + 1000;
    Release last = command(s, now, TOTAL - 1, 1);
    uint32_t seek = seekTimeUs(m, 0, lbaToStroke(float(TOTAL - 1) / TOTAL, m.zoned));
    CHECK_EQ(seek, seekTimeUs(m, 0, 1));  // the last LBA is on the innermost track
    uint64_t arrive = now + seek;
    CHECK_EQ(last.ready_us, arrive + rotationalWaitUs(m, arrive, TOTAL - 1, SPT) + transferUs(m, 1, SPT));
}

TEST(test_hold_sequential_one_sector_per_sector) {
    SeekModel m = preset("voice97");
    Scheduler s = scheduler(m);
    uint64_t now = 3000000;
    uint32_t sector_us = transferUs(m, 1, SPT);
    uint64_t prev = 0;
    for (uint32_t k = 0; k < 128; ++k) {
        Release r = s.onSector(now, 1000, 1000 + k);
        CHECK_EQ(r.first, k == 0);
        if (k > 0) CHECK(r.ready_us - prev + 1 >= sector_us && r.ready_us - prev <= sector_us + 1);
        prev = r.ready_us;
    }
    // The next command carries straight on, though it reached core 0 a
    // little after the last ended: no seek and no turn of the platter.
    Release first;
    command(s, prev + 200, 1128, 128, &first);
    CHECK(first.first);
    CHECK(first.ready_us - prev <= sector_us + 1);

    // A long pause first: a revolution was read ahead at most.
    uint64_t later = prev + 1000000;
    Release r = command(s, later, 1256, 128);  // straight on from 1128 + 128
    uint64_t rev = revolutionUs(m);
    CHECK(r.ready_us >= later);  // 128 sectors are two turns: the second can't be read yet
    CHECK(r.ready_us <= later + 2 * rev);
}

TEST(test_hold_random_reads_seek_plus_half_a_turn) {
    SeekModel m = preset("voice97");
    Scheduler s = scheduler(m);
    std::mt19937 rng(12345);
    std::uniform_int_distribution<uint32_t> pick(0, TOTAL - 9);
    uint64_t now = 5000000;
    float head = 0;
    double modelled = 0, expected = 0;
    const int reads = 300;
    command(s, now, 0, 1);
    now += 100000;
    for (int i = 0; i < reads; ++i) {
        uint32_t lba = pick(rng);
        float x = lbaToStroke(float(lba) / TOTAL, m.zoned);
        expected += seekTimeUs(m, head, x) + revolutionUs(m) / 2.0 + transferUs(m, 8, SPT);
        head = x;
        Release r = command(s, now, lba, 8);
        modelled += double(r.ready_us - now);
        now = r.ready_us + 300;  // the host asks again at once
    }
    double ratio = modelled / expected;
    std::printf("  random 4 KB: %.2f ms each, expected %.2f\n", modelled / reads / 1000, expected / reads / 1000);
    CHECK(ratio > 0.9 && ratio < 1.1);
    CHECK_EQ(s.commands(), uint32_t(reads + 1));  // and the first, to LBA 0
    CHECK_EQ(s.held(), uint32_t(reads + 1));
    CHECK(std::fabs(double(s.totalUs()) - modelled) < 0.01 * modelled + 2e6);  // the first's time too
}

TEST(test_hold_queues_behind_the_head) {
    SeekModel m = preset("voice97");
    Scheduler s = scheduler(m);
    uint64_t now = 7000000;
    Release a = command(s, now, 100, 8);
    // The next command reaches core 0 at the same instant: its seek waits for
    // the head to be free.
    Release first;
    command(s, now, 90000, 8, &first);
    CHECK_EQ(first.seek_start_us, a.ready_us);
    CHECK(first.ready_us > a.ready_us);
}

TEST(test_hold_capped_at_a_second) {
    SeekModel m = preset("stepper85");
    m.stepMs = 20;  // 615 cylinders at 20 ms a step: far over a second
    Scheduler s = scheduler(m);
    uint64_t now = 9000000;
    command(s, now, 0, 1);
    now += 2000000;
    Release first;
    Release last = command(s, now, TOTAL - 1, 1, &first);
    CHECK_EQ(last.ready_us, now + seek_timing::MAX_HOLD_US);
    CHECK_EQ(s.capped(), 1u);
    CHECK_EQ(s.maxUs(), seek_timing::MAX_HOLD_US);
}

TEST(test_hold_stepper_full_stroke_under_the_cap) {
    SeekModel m = preset("stepper85");
    uint32_t full = seekTimeUs(m, 0, 1);
    std::printf("  stepper85 full stroke: %u ms\n", unsigned(full / 1000));
    CHECK(full + revolutionUs(m) < seek_timing::MAX_HOLD_US);
}

TEST(test_hold_off_passes_through) {
    Scheduler s;
    s.setGeometry(TOTAL, SPT);
    Release r = s.onSector(100, 500, 500);
    CHECK(r.first);
    CHECK_EQ(r.ready_us, uint64_t(100));
    CHECK_EQ(r.seek_start_us, uint64_t(100));
    r = s.onSector(150, 500, 501);
    CHECK(!r.first);
    CHECK_EQ(r.ready_us, uint64_t(150));
    // The same single sector read twice: two commands.
    r = s.onSector(200, 7, 7);
    CHECK(r.first);
    r = s.onSector(300, 7, 7);
    CHECK(r.first);
    CHECK_EQ(s.commands(), 0u);  // not timed
}

void runSeekTimingTests() {
    RUN(test_sentence_format_and_checksum);
    RUN(test_sentence_fields);
    RUN(test_sentence_line_assembler);
    RUN(test_drive_presets);
    RUN(test_drive_model_sentence_round_trip);
    RUN(test_drive_timing_rotation_and_transfer);
    RUN(test_seek_timing_setting);
    RUN(test_hdm_example_is_voice97);
    RUN(test_hdm_none_and_rejects);
    RUN(test_hdm_times_out);
    RUN(test_hold_same_track_no_seek);
    RUN(test_hold_full_stroke);
    RUN(test_hold_sequential_one_sector_per_sector);
    RUN(test_hold_random_reads_seek_plus_half_a_turn);
    RUN(test_hold_queues_behind_the_head);
    RUN(test_hold_capped_at_a_second);
    RUN(test_hold_stepper_full_stroke_under_the_cap);
    RUN(test_hold_off_passes_through);
}
