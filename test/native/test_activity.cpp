// Tests for the activity classifier (activity.h) against synthetic
// workloads, with time moving as it does on the drive.
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "activity.h"
#include "check.h"

using namespace activity;

// Drives a classifier through a workload: commands at given times, each
// lasting `duration_us`, with update() called every millisecond (as core 0
// would). Records how long each state was held.
struct Sim {
    Classifier c;
    uint32_t now = 1000;
    uint32_t time_in[3] = {};  // Idle, Sequential, Random
    uint32_t changes = 0;
    std::vector<std::pair<uint32_t, State>> timeline;

    explicit Sim(const Config& cfg = Config()) : c(cfg) {}

    void advanceTo(uint32_t t) {
        while (now < t) {
            now += 1000;
            if (c.update(now)) {
                ++changes;
                timeline.push_back({now, c.state()});
            }
            time_in[int(c.state())] += 1000;
        }
    }
    Classified command(uint32_t lba, uint32_t count, uint32_t duration_us) {
        Classified k = c.onCommand(now, lba, count);
        advanceTo(now + duration_us);
        c.onCommandEnd(now);
        return k;
    }
    void gap(uint32_t us) { advanceTo(now + us); }
    void resetCounters() {
        std::memset(time_in, 0, sizeof time_in);
        changes = 0;
        timeline.clear();
    }
    uint32_t pct(State s) const {
        uint32_t total = time_in[0] + time_in[1] + time_in[2];
        return total ? time_in[int(s)] * 100 / total : 0;
    }
};

// A big sequential read: 128-sector commands, as a USB bridge would issue.
static void sequential(Sim& s, uint32_t& lba, int commands, uint32_t gap_us = 2000) {
    for (int i = 0; i < commands; ++i) {
        s.command(lba, 128, 30000);
        lba += 128;
        s.gap(gap_us);
    }
}

// Small reads scattered over the disk.
static void random(Sim& s, std::mt19937& rng, int commands) {
    std::uniform_int_distribution<uint32_t> where(0, 130000);
    for (int i = 0; i < commands; ++i) {
        s.command(where(rng), 8, 3000);
        s.gap(2000);
    }
}

TEST(test_activity_media_commands) {
    CHECK(isMediaCommand(0x20));
    CHECK(isMediaCommand(0xC8));
    CHECK(isMediaCommand(0x30));
    CHECK(isMediaCommand(0x70));
    CHECK(isMediaCommand(0x10));
    CHECK(!isMediaCommand(0xEC));  // IDENTIFY
    CHECK(!isMediaCommand(0xEF));  // SET FEATURES
    CHECK(!isMediaCommand(0xE5));  // CHECK POWER MODE
}

TEST(test_activity_starts_idle) {
    Classifier c;
    CHECK(c.state() == State::Idle);
    CHECK(!c.update(5000000));
    CHECK(c.state() == State::Idle);
}

TEST(test_activity_sequential_stream) {
    Sim s;
    uint32_t lba = 1000;
    sequential(s, lba, 60);  // ~2 s
    CHECK(s.c.state() == State::Sequential);
    CHECK_EQ(s.time_in[int(State::Random)], 0u);  // the opening seek never flips it to Random
    CHECK(s.pct(State::Sequential) >= 95);
    CHECK_EQ(s.c.randomCommands(), 1u);  // only the first command (from nowhere)
}

TEST(test_activity_random_reads) {
    Sim s;
    std::mt19937 rng(1);
    random(s, rng, 200);  // ~1 s
    CHECK(s.c.state() == State::Random);
    CHECK(s.pct(State::Random) >= 95);
    CHECK(s.c.averageSeek() > 10000);  // scattered over 130k sectors
}

TEST(test_activity_goes_idle_and_wakes) {
    Sim s;
    uint32_t lba = 0;
    sequential(s, lba, 10);
    CHECK(s.c.state() == State::Sequential);
    s.resetCounters();
    s.gap(145000);  // 147 ms since the last command ended (sequential() adds 2 ms)
    CHECK(s.c.state() == State::Sequential);  // not yet
    s.gap(5000);    // 152 ms: past idle_us
    CHECK(s.c.state() == State::Idle);
    // Waking is immediate: the first command sets the state.
    s.c.onCommand(s.now, 500000 % 131072, 8);
    CHECK(s.c.update(s.now + 1));
    CHECK(s.c.state() != State::Idle);
}

TEST(test_activity_long_command_is_not_idle) {
    Sim s;
    s.command(0, 256, 400000);  // one slow 256-sector read: 400 ms
    CHECK(s.time_in[int(State::Idle)] <= 1000);  // at most the first tick
    s.gap(200000);
    CHECK(s.c.state() == State::Idle);
}

TEST(test_activity_interleaved_streams_stay_sequential) {
    // Two files read in alternate chunks: each stream is sequential.
    Sim s;
    uint32_t a = 1000, b = 90000;
    for (int i = 0; i < 40; ++i) {
        s.command(a, 64, 15000);
        a += 64;
        s.command(b, 64, 15000);
        b += 64;
    }
    CHECK(s.c.state() == State::Sequential);
    CHECK_EQ(s.c.randomCommands(), 2u);  // the first command of each stream
    CHECK_EQ(s.time_in[int(State::Random)], 0u);
}

TEST(test_activity_too_many_streams_read_as_random) {
    // Six interleaved streams outnumber the four tracked: that's head
    // thrashing, and Random is the right sound for it.
    Sim s;
    uint32_t next[6] = {0, 20000, 40000, 60000, 80000, 100000};
    for (int i = 0; i < 30; ++i)
        for (uint32_t& n : next) {
            s.command(n, 16, 4000);
            n += 16;
        }
    CHECK(s.c.state() == State::Random);
}

TEST(test_activity_small_gaps_and_rereads) {
    Classifier c;
    c.onCommand(0, 1000, 8);              // stream ends at 1008
    CHECK(c.onCommand(10, 1008 + 64, 8).sequential);   // a 64-sector skip: still sequential
    CHECK(!c.onCommand(20, 1080 + 65, 8).sequential);  // 65: a seek
    Classified back = c.onCommand(30, 1153 - 8, 8);    // re-reading the last 8
    CHECK(back.sequential);
}

TEST(test_activity_seek_distance) {
    Classifier c;
    Classified first = c.onCommand(0, 5000, 10);
    CHECK(!first.sequential);
    CHECK_EQ(first.seek, 5000u);  // from nowhere: treat as from LBA 0
    Classified far = c.onCommand(10, 100000, 10);
    CHECK_EQ(far.seek, 100000u - 5010u);
    Classified back = c.onCommand(20, 20, 10);
    CHECK_EQ(back.seek, 100010u - 20u);
}

TEST(test_activity_metadata_reads_dont_flip_sequential) {
    // A file copy with an occasional jump (file-system metadata): ~1 seek in
    // 8 commands stays Sequential.
    Sim s;
    uint32_t lba = 10000;
    std::mt19937 rng(2);
    for (int i = 0; i < 80; ++i) {
        if (i % 8 == 7) {
            s.command(rng() % 1000, 8, 3000);  // a FAT read near the start
        } else {
            s.command(lba, 128, 30000);
            lba += 128;
        }
    }
    CHECK(s.c.state() == State::Sequential);
    CHECK_EQ(s.time_in[int(State::Random)], 0u);
}

TEST(test_activity_switches_after_dwell) {
    Sim s;
    uint32_t lba = 0;
    sequential(s, lba, 30);
    CHECK(s.c.state() == State::Sequential);
    s.resetCounters();
    uint32_t switch_start = s.now;
    std::mt19937 rng(3);
    random(s, rng, 150);
    CHECK(s.c.state() == State::Random);
    CHECK_EQ(s.changes, 1u);  // one clean switch, no flicker
    uint32_t switched_after = s.timeline[0].first - switch_start;
    // Not before the dwell; not later than the window filling plus the dwell.
    CHECK(switched_after >= 100000);
    CHECK(switched_after <= 250000 + 100000 + 10000);
}

TEST(test_activity_alternating_phases) {
    // Sequential, random, idle, sequential: each phase is mostly its own state.
    Sim s;
    uint32_t lba = 0;
    std::mt19937 rng(4);
    sequential(s, lba, 40);
    s.resetCounters();
    random(s, rng, 300);
    CHECK(s.pct(State::Random) >= 80);
    s.resetCounters();
    s.gap(1000000);
    CHECK(s.pct(State::Idle) >= 80);
    s.resetCounters();
    sequential(s, lba, 40);
    CHECK(s.pct(State::Sequential) >= 90);
}

TEST(test_activity_random_burst_after_idle_is_quick) {
    // Waking into a random burst: Sequential for the first few commands at
    // most, then Random, and it stays there.
    Sim s;
    std::mt19937 rng(6);
    random(s, rng, 100);
    CHECK(s.timeline.size() == 2u);  // Idle -> Sequential (waking), -> Random
    CHECK(s.timeline.back().second == State::Random);
    CHECK(s.timeline.back().first - s.timeline.front().first <= 25000);  // within ~5 commands
    CHECK(s.pct(State::Random) >= 95);
}

TEST(test_activity_format) {
    Sim s;
    std::mt19937 rng(5);
    random(s, rng, 50);
    char buf[64];
    size_t n = formatActivity(123456, s.c, buf, sizeof buf);
    std::string line(buf, n);
    CHECK(line.rfind("ACT 123456 RAND ", 0) == 0);
    CHECK(line.back() == '\n');
    Classifier idle;
    n = formatActivity(7, idle, buf, sizeof buf);
    CHECK_STR(std::string(buf, n), "ACT 7 IDLE 0 0\n");
}

void runActivityTests() {
    RUN(test_activity_media_commands);
    RUN(test_activity_starts_idle);
    RUN(test_activity_sequential_stream);
    RUN(test_activity_random_reads);
    RUN(test_activity_goes_idle_and_wakes);
    RUN(test_activity_long_command_is_not_idle);
    RUN(test_activity_interleaved_streams_stay_sequential);
    RUN(test_activity_too_many_streams_read_as_random);
    RUN(test_activity_small_gaps_and_rereads);
    RUN(test_activity_seek_distance);
    RUN(test_activity_metadata_reads_dont_flip_sequential);
    RUN(test_activity_switches_after_dwell);
    RUN(test_activity_alternating_phases);
    RUN(test_activity_random_burst_after_idle_is_quick);
    RUN(test_activity_format);
}
