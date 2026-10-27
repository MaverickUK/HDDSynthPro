// Host-side tests for the activity link (activity_link.*): the sentence
// format and checksum, with the known answers from docs/activity-link.md
// (shared with host/tests/test_activity_link.py), and the send rules.
#include <cstdint>
#include <cstring>
#include <string>

#include "activity_link.h"
#include "check.h"

using activity::State;
using activity_link::Fields;
using activity_link::Op;

static std::string sentence(bool busy, State state, Op op) {
    char buf[activity_link::SENTENCE_MAX];
    Fields f;
    f.busy = busy;
    f.state = state;
    f.op = op;
    size_t n = activity_link::formatSentence(f, buf, sizeof buf);
    return std::string(buf, n);
}

TEST(test_link_known_answers) {
    CHECK_STR(sentence(false, State::Idle, Op::None), "$HDA,1,0,I,-*28\r\n");
    CHECK_STR(sentence(true, State::Sequential, Op::Read), "$HDA,1,1,S,R*4C\r\n");
    CHECK_STR(sentence(true, State::Random, Op::Write), "$HDA,1,1,R,W*48\r\n");
    CHECK_STR(sentence(true, State::Sequential, Op::Write), "$HDA,1,1,S,W*49\r\n");
    CHECK_STR(sentence(false, State::Random, Op::Read), "$HDA,1,0,R,R*4C\r\n");
}

TEST(test_link_format_needs_room) {
    char buf[activity_link::SENTENCE_MAX];
    Fields f;
    CHECK_EQ(activity_link::formatSentence(f, buf, 10), size_t(0));  // too small: nothing
    size_t n = activity_link::formatSentence(f, buf, sizeof buf);
    CHECK_EQ(n, std::strlen("$HDA,1,0,I,-*28\r\n"));
    CHECK(n < activity_link::SENTENCE_MAX);
    CHECK_EQ(std::strlen(buf), n);
}

TEST(test_link_op_of_commands) {
    CHECK(activity_link::opOf(0x20) == Op::Read);   // READ SECTORS
    CHECK(activity_link::opOf(0xC8) == Op::Read);   // READ DMA
    CHECK(activity_link::opOf(0x30) == Op::Write);  // WRITE SECTORS
    CHECK(activity_link::opOf(0xCA) == Op::Write);  // WRITE DMA
    CHECK(activity_link::opOf(0x40) == Op::None);   // READ VERIFY moves no data
    CHECK(activity_link::opOf(0x70) == Op::None);   // SEEK
    CHECK(activity_link::opOf(0xEC) == Op::None);   // IDENTIFY
}

TEST(test_link_sends_on_change_and_heartbeat) {
    activity_link::Sender s(200);
    Fields idle;
    CHECK(s.due(idle, 0));  // the first one always
    s.sent(idle, 0);
    CHECK(!s.due(idle, 1));
    CHECK(!s.due(idle, 199));
    CHECK(s.due(idle, 200));  // heartbeat
    s.sent(idle, 200);

    Fields busy = idle;
    busy.busy = true;
    CHECK(s.due(busy, 201));  // any change, at once
    s.sent(busy, 201);
    Fields seq = busy;
    seq.state = State::Sequential;
    CHECK(s.due(seq, 202));
    Fields wrote = busy;
    wrote.op = Op::Write;
    CHECK(s.due(wrote, 202));
    CHECK_EQ(s.count(), 3u);
    CHECK(s.last() == busy);
}

TEST(test_link_unsent_stays_due) {
    // The UART was full: sent() isn't called, so the sentence stays due and
    // the next try sends whatever is current then.
    activity_link::Sender s(200);
    Fields f;
    s.sent(f, 0);
    f.busy = true;
    CHECK(s.due(f, 5));   // couldn't write it
    f.busy = false;       // back as it was before anything went out
    CHECK(!s.due(f, 6));  // nothing to say any more
    CHECK(s.due(f, 200)); // until the heartbeat
}

TEST(test_link_heartbeat_across_wrap) {
    activity_link::Sender s(200);
    Fields f;
    s.sent(f, 0xFFFFFF00u);  // millis() wraps after 49 days
    CHECK(!s.due(f, 0xFFFFFFC7u));
    CHECK(s.due(f, 0x00000008u));  // 264 ms later, across the wrap
}

void runActivityLinkTests() {
    RUN(test_link_known_answers);
    RUN(test_link_format_needs_room);
    RUN(test_link_op_of_commands);
    RUN(test_link_sends_on_change_and_heartbeat);
    RUN(test_link_unsent_stays_due);
    RUN(test_link_heartbeat_across_wrap);
}
