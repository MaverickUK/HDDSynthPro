// Host-side tests for the CD-ROM drive's sounds: the model of a real drive
// (optical_drive.*) and its sentence, $HDO (activity_link.*), whose known
// answers the HDD Synth's parser shares.
#include <cstdint>
#include <cstring>
#include <string>

#include "activity_link.h"
#include "check.h"
#include "optical_drive.h"

using activity::State;
using activity_link::OpticalFields;

static std::string opticalSentence(bool busy, State state, bool motor, bool tray_open) {
    char buf[activity_link::OPTICAL_SENTENCE_MAX];
    OpticalFields f;
    f.busy = busy;
    f.state = state;
    f.motor = motor;
    f.tray_open = tray_open;
    size_t n = activity_link::formatOpticalSentence(f, buf, sizeof buf);
    return std::string(buf, n);
}

TEST(test_optical_known_answers) {
    CHECK_STR(opticalSentence(false, State::Idle, false, false), "$HDO,3,0,I,0,C*56\r\n");
    CHECK_STR(opticalSentence(true, State::Sequential, true, false), "$HDO,3,1,S,1,C*4C\r\n");
    CHECK_STR(opticalSentence(false, State::Idle, false, true), "$HDO,3,0,I,0,O*5A\r\n");
    std::string longest = opticalSentence(true, State::Random, true, true);
    CHECK(longest.size() < activity_link::OPTICAL_SENTENCE_MAX);
}

TEST(test_optical_parse) {
    for (bool busy : {false, true})
        for (State st : {State::Idle, State::Sequential, State::Random})
            for (bool motor : {false, true})
                for (bool tray : {false, true}) {
                    std::string s = opticalSentence(busy, st, motor, tray);
                    OpticalFields f;
                    CHECK(activity_link::parseOpticalLine(s.data(), s.size() - 2, f));
                    CHECK(f.busy == busy && f.state == st && f.motor == motor && f.tray_open == tray);
                }
    OpticalFields f;
    const char* bad[] = {"$HDO,3,1,S,1,X*57", "$HDO,2,0,I,0,C*57", "$HDA,1,0,I,-*28", "$HDO,3,0,I,0,C*30"};
    for (const char* line : bad) CHECK(!activity_link::parseOpticalLine(line, std::strlen(line), f));
}

TEST(test_optical_goes_between_activity_and_head) {
    activity_link::Sender a(200);
    activity_link::OpticalSender o(200);
    activity_link::HeadPacer h(2000);
    activity_link::Fields idle;
    OpticalFields spinning;
    spinning.motor = true;
    activity_link::HeadTarget t;
    h.post(t, 0);
    CHECK(activity_link::nextSentence(a, idle, 0, h, 0, &o, spinning) == activity_link::Next::Activity);
    a.sent(idle, 0);
    CHECK(activity_link::nextSentence(a, idle, 0, h, 0, &o, spinning) == activity_link::Next::Optical);
    o.sent(spinning, 0);
    CHECK(activity_link::nextSentence(a, idle, 0, h, 0, &o, spinning) == activity_link::Next::Head);
    h.sent(0);
    CHECK(activity_link::nextSentence(a, idle, 10, h, 10000, &o, spinning) == activity_link::Next::Nothing);
    spinning.busy = true;  // a change goes at once
    CHECK(activity_link::nextSentence(a, idle, 10, h, 10000, &o, spinning) == activity_link::Next::Optical);
    // Without a CD-ROM drive, none.
    CHECK(activity_link::nextSentence(a, idle, 10, h, 10000) == activity_link::Next::Nothing);
}

TEST(test_optical_spins_up_and_down) {
    optical::Config cfg;
    optical::Drive d(cfg);
    d.begin(0, true);  // a disc in at power-on: it spins up
    CHECK(d.state(0).motor);
    CHECK_EQ(d.spinUps(), uint32_t(1));
    d.update(cfg.spin_down_ms - 1, 0);
    CHECK(d.state(cfg.spin_down_ms - 1).motor);
    d.update(cfg.spin_down_ms, 0);
    CHECK(!d.state(cfg.spin_down_ms).motor);  // nothing read: it stops

    // Windows' polls don't wake it; a read does, and lights it.
    uint32_t t = 40000;
    d.onCommand(t, t * 1000, 0x4A, 0, 0);  // GET EVENT STATUS NOTIFICATION
    d.onCommandEnd(t, t * 1000);
    d.onCommand(t, t * 1000, 0x00, 0, 0);  // TEST UNIT READY
    d.onCommandEnd(t, t * 1000);
    CHECK(!d.state(t).motor && !d.state(t).busy);
    d.onCommand(t, t * 1000, 0x28, 100, 16);  // READ(10)
    CHECK(d.state(t).motor && d.state(t).busy);
    CHECK_EQ(d.spinUps(), uint32_t(2));
    d.update(t + 100, (t + 100) * 1000);
    CHECK(d.state(t + 100).busy);  // a long read is busy throughout
    d.onCommandEnd(t + 100, (t + 100) * 1000);
    CHECK(d.state(t + 100 + cfg.busy_min_ms).busy == false);
    // The spin-down counts from the read's end.
    d.update(t + 100 + cfg.spin_down_ms - 1, 0);
    CHECK(d.state(t + 100 + cfg.spin_down_ms - 1).motor);
    d.update(t + 100 + cfg.spin_down_ms, 0);
    CHECK(!d.state(t + 100 + cfg.spin_down_ms).motor);
}

TEST(test_optical_reads_are_classified) {
    optical::Drive d;
    d.begin(0, true);
    uint32_t t_us = 1000000;
    for (int i = 0; i < 20; ++i, t_us += 5000) {  // a file read straight through: sequential
        d.onCommand(t_us / 1000, t_us, 0x28, 1000 + uint32_t(i) * 16, 16);
        d.onCommandEnd(t_us / 1000, t_us + 2000);
        d.update(t_us / 1000, t_us + 2000);
    }
    CHECK(d.state(t_us / 1000).reads == State::Sequential);
    uint32_t lba = 7;
    for (int i = 0; i < 40; ++i, t_us += 5000) {  // jumping about: random
        lba = (lba * 7919 + 104729) % 300000;
        d.onCommand(t_us / 1000, t_us, 0x28, lba, 1);
        d.onCommandEnd(t_us / 1000, t_us + 2000);
        d.update(t_us / 1000, t_us + 2000);
    }
    CHECK(d.state(t_us / 1000).reads == State::Random);
}

TEST(test_optical_disc_change) {
    optical::Config cfg;
    optical::Drive d(cfg);
    d.begin(0, true);
    // Console M: the disc comes out at once (the tray opens, the disc stops)...
    d.eject(1000);
    CHECK(d.state(1000).tray_open && !d.state(1000).motor);
    // ...a read meanwhile doesn't spin anything...
    d.onCommand(1100, 1100000, 0x28, 0, 16);
    d.onCommandEnd(1100, 1100000);
    CHECK(!d.state(1100).motor && !d.state(1100).busy);
    // ...the next disc is in within milliseconds, but the tray stays open a while...
    d.load(1010, true);
    d.update(1010 + cfg.tray_min_open_ms - 1 - 10, 0);
    CHECK(d.state(1000 + cfg.tray_min_open_ms - 1).tray_open);
    // ...then closes, and the new disc spins up.
    d.update(1000 + cfg.tray_min_open_ms, 0);
    CHECK(!d.state(1000 + cfg.tray_min_open_ms).tray_open);
    CHECK(d.state(1000 + cfg.tray_min_open_ms).motor);
    // An empty drive: the tray closes on nothing, and nothing spins.
    d.eject(10000);
    d.load(10000, false);
    d.update(10000 + cfg.tray_min_open_ms, 0);
    CHECK(!d.state(10000 + cfg.tray_min_open_ms).tray_open && !d.state(10000 + cfg.tray_min_open_ms).motor);
    d.onCommand(20000, 0, 0x28, 0, 16);  // no disc: a read fails at once, nothing spins
    CHECK(!d.state(20000).motor);
}

void runOpticalDriveTests() {
    RUN(test_optical_known_answers);
    RUN(test_optical_parse);
    RUN(test_optical_goes_between_activity_and_head);
    RUN(test_optical_spins_up_and_down);
    RUN(test_optical_reads_are_classified);
    RUN(test_optical_disc_change);
}
