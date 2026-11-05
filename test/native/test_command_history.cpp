// Host-side tests for CommandHistory: which command a separately reported bus
// event (a NOIORDY read) fell in or followed.
#include "check.h"
#include "command_history.h"

static TraceEvent start(uint32_t t, uint8_t command, uint8_t unit = 0, uint8_t packet = 0, uint16_t count = 1) {
    return commandStartEvent(t, command, 0, count, packet, unit);
}
static TraceEvent end(uint32_t t, uint8_t command) { return commandEndEvent(t, command, 0x50, 0, 0); }

TEST(test_history_empty) {
    CommandHistory h;
    CHECK(h.at(1000) == nullptr);
}

TEST(test_history_finds_the_running_command) {
    CommandHistory h;
    h.note(start(100, 0x20));
    h.note(end(150, 0x20));
    h.note(start(200, 0xA0, 1, 0x28, 4));  // READ(10) on device 1
    const CommandHistory::Entry* e = h.at(260);
    CHECK(e != nullptr);
    CHECK_EQ(e->command, 0xA0);
    CHECK_EQ(e->packet, 0x28);
    CHECK_EQ(e->unit, 1);
    CHECK_EQ(e->count, 4);
    CHECK(!e->ended);
    CHECK_EQ(e->start_us, 200u);
}

TEST(test_history_finds_a_finished_command) {
    CommandHistory h;
    h.note(start(100, 0x20));
    h.note(end(150, 0x20));
    h.note(start(200, 0xC8));
    // An event at 170, learnt of after the next command started: the first one, ended.
    const CommandHistory::Entry* e = h.at(170);
    CHECK(e != nullptr);
    CHECK_EQ(e->command, 0x20);
    CHECK(e->ended);
    CHECK_EQ(e->end_us, 150u);
    CHECK(h.at(99) == nullptr);  // before anything kept
}

TEST(test_history_ignores_other_events) {
    CommandHistory h;
    TraceEvent bus{500, 0, TraceKind::Bus, 0, 0};
    h.note(bus);
    h.note(end(600, 0x20));  // an end with no start
    CHECK(h.at(700) == nullptr);
}

TEST(test_history_keeps_the_last_few) {
    CommandHistory h;
    for (uint32_t i = 0; i < 40; ++i) {
        h.note(start(1000 + i * 10, uint8_t(i)));
        h.note(end(1000 + i * 10 + 5, uint8_t(i)));
    }
    CHECK_EQ(h.at(1000 + 39 * 10 + 7)->command, 39);
    CHECK_EQ(h.at(1000 + 30 * 10 + 1)->command, 30);
    CHECK(h.at(1000 + 20 * 10 + 1) == nullptr);  // dropped: older than the last 16
}

TEST(test_history_across_the_wrap) {
    CommandHistory h;
    h.note(start(0xFFFFFF00u, 0x20));
    h.note(start(0x00000010u, 0x30));  // time_us_32() wrapped between them
    CHECK_EQ(h.at(0xFFFFFF80u)->command, 0x20);
    CHECK_EQ(h.at(0x00000020u)->command, 0x30);
}

void runCommandHistoryTests() {
    RUN(test_history_empty);
    RUN(test_history_finds_the_running_command);
    RUN(test_history_finds_a_finished_command);
    RUN(test_history_ignores_other_events);
    RUN(test_history_keeps_the_last_few);
    RUN(test_history_across_the_wrap);
}
