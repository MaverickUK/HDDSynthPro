// Host-side tests for the activity link (activity_link.*): the sentence
// format and checksum, with the known answers from docs/activity-link.md
// (shared with host/tests/test_activity_link.py), and the send rules.
#include <cstdint>
#include <cstdio>
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

static bool parses(const std::string& line, Fields* out = nullptr) {
    Fields f;
    bool ok = activity_link::parseLine(line.data(), line.size(), f);
    if (ok && out) *out = f;
    return ok;
}

static std::string withChecksum(const std::string& body) {
    uint8_t checksum = 0;
    for (char c : body) checksum ^= uint8_t(c);
    char tail[4];
    std::snprintf(tail, sizeof tail, "*%02X", unsigned(checksum));
    return "$" + body + tail;
}

TEST(test_link_parse_known_answers) {
    // Every sentence the sender can make reads back as what it said.
    const State states[] = {State::Idle, State::Sequential, State::Random};
    const Op ops[] = {Op::None, Op::Read, Op::Write};
    for (int busy = 0; busy < 2; ++busy)
        for (State s : states)
            for (Op op : ops) {
                std::string line = sentence(busy != 0, s, op);
                Fields got;
                CHECK(parses(line.substr(0, line.size() - 2), &got));  // without \r\n
                CHECK(got.busy == (busy != 0));
                CHECK(got.state == s);
                CHECK(got.op == op);
            }
}

TEST(test_link_parse_rejects) {
    CHECK(!parses("$HDA,1,1,S,R*4D"));      // wrong checksum
    CHECK(!parses("$HDA,1,1,S,R"));         // no checksum
    CHECK(!parses("$HDA,1,1,S,R*4"));       // one digit
    CHECK(!parses("$HDA,1,1,S,R*4CC"));     // three
    CHECK(!parses("$HDA,1,1,S,R*G1"));      // not hex
    CHECK(!parses("$GPGGA,123519*47"));     // another sentence
    CHECK(!parses("HDA,1,1,S,R*4C"));       // no $
    CHECK(!parses(withChecksum("HDA,1,2,S,R")));   // busy not 0/1
    CHECK(!parses(withChecksum("HDA,1,1,X,R")));   // unknown class
    CHECK(!parses(withChecksum("HDA,1,1,S,X")));   // unknown op
    CHECK(!parses(withChecksum("HDA,1,11,S,R")));  // busy too long
    CHECK(!parses(withChecksum("HDA,1,1")));       // too few fields
    CHECK(!parses(withChecksum("HDA,,1,S,R")));    // no version
    CHECK(!parses(withChecksum("HDA,v1,1,S,R")));  // version not a number
    CHECK(!parses(withChecksum("HDA,1,1,S,R," + std::string(80, 'x'))));  // over 80 bytes
    CHECK(!parses(withChecksum("HDA,1,1,S,R,\xff")));  // not ASCII
}

TEST(test_link_parse_accepts_later_versions) {
    Fields got;
    CHECK(parses(withChecksum("HDA,2,1,S,W,123,future"), &got));  // extra fields ignored
    CHECK(got.busy && got.state == State::Sequential && got.op == Op::Write);
    CHECK(parses("$HDA,1,1,S,R*4c"));  // lower-case hex, as the Python reference
}

static std::string feedAll(activity_link::Receiver& r, const std::string& bytes) {
    // The sentences it accepted, re-formatted, one after another.
    std::string out;
    Fields f;
    for (char c : bytes)
        if (r.feed(c, f)) out += sentence(f.busy, f.state, f.op);
    return out;
}

TEST(test_link_receiver_assembles_lines) {
    activity_link::Receiver r;
    std::string a = sentence(true, State::Sequential, Op::Read);
    std::string b = sentence(false, State::Idle, Op::None);
    CHECK_STR(feedAll(r, a.substr(0, 7)), "");  // split across feeds
    CHECK_STR(feedAll(r, a.substr(7)), a);
    CHECK_STR(feedAll(r, a + b), a + b);  // two in one feed
    std::string no_cr = b.substr(0, b.size() - 2) + "\n";
    CHECK_STR(feedAll(r, no_cr), b);      // \n without \r
    CHECK_STR(feedAll(r, "\r\n\n"), "");  // empty lines: skipped, not rejected
    CHECK_EQ(r.valid(), 4u);
    CHECK_EQ(r.rejected(), 0u);
}

TEST(test_link_receiver_rejects_and_recovers) {
    activity_link::Receiver r;
    std::string good = sentence(true, State::Random, Op::Write);
    CHECK_STR(feedAll(r, std::string(200, 'x') + "\r\n" + good), good);  // over 80, then a good one
    CHECK_STR(feedAll(r, "$HDA,1,1,S,R*4D\r\n" + good), good);         // a bad checksum, then good
    CHECK_STR(feedAll(r, "R,W*48\r\n"), "");  // the tail of a sentence, as when listening starts mid-line
    CHECK_EQ(r.valid(), 2u);
    CHECK_EQ(r.rejected(), 3u);
    r.reset();
    CHECK_EQ(r.valid(), 0u);
    CHECK_EQ(r.rejected(), 0u);
}

TEST(test_link_receiver_line_length_limit) {
    // 80 bytes is the limit, with \r\n not counted.
    std::string body = "HDA,1,1,S,R,";
    std::string at_limit = withChecksum(body + std::string(80 - 4 - body.size(), 'x'));
    CHECK_EQ(at_limit.size(), activity_link::MAX_LINE);
    activity_link::Receiver r;
    CHECK_STR(feedAll(r, at_limit + "\r\n"), sentence(true, State::Sequential, Op::Read));
    std::string over = withChecksum(body + std::string(81 - 4 - body.size(), 'x'));
    CHECK_STR(feedAll(r, over + "\r\n"), "");
    CHECK_EQ(r.rejected(), 1u);
}

// ---- Version 2: $HDE --------------------------------------------------------

using activity_link::HeadPacer;
using activity_link::HeadTarget;

static std::string head(uint16_t pos, Op op, uint32_t sectors) {
    char buf[activity_link::HEAD_SENTENCE_MAX];
    HeadTarget t;
    t.pos = pos;
    t.op = op;
    t.sectors = sectors;
    size_t n = activity_link::formatHeadSentence(t, buf, sizeof buf);
    return std::string(buf, n);
}

TEST(test_head_known_answers) {
    // docs/activity-link.md, and HDD Synth's receiver.
    CHECK_STR(head(0, Op::Read, 8), "$HDE,2,0,R,8*21\r\n");
    CHECK_STR(head(32768, Op::Write, 128), "$HDE,2,32768,W,128*2F\r\n");
    CHECK_STR(head(65535, Op::Read, 1), "$HDE,2,65535,R,1*28\r\n");
    CHECK(head(65535, Op::Write, 256).size() < activity_link::HEAD_SENTENCE_MAX);
    char small[10];
    CHECK_EQ(activity_link::formatHeadSentence(HeadTarget{}, small, sizeof small), size_t(0));
}

TEST(test_head_position) {
    const uint32_t total = 131072;  // the generated disk
    CHECK_EQ(activity_link::headPosition(0, total), 0);
    CHECK_EQ(activity_link::headPosition(total - 1, total), 65535);
    CHECK_EQ(activity_link::headPosition((total - 1) / 2, total), 32767);
    CHECK_EQ(activity_link::headPosition(total, total), 65535);  // past the end: clamped
    // A large disk: the maths is 64-bit.
    CHECK_EQ(activity_link::headPosition(0x0FFFFFFE, 0x0FFFFFFF), 65535);
    CHECK_EQ(activity_link::headPosition(0x07FFFFFF, 0x0FFFFFFF), 32767);
    CHECK_EQ(activity_link::headPosition(5, 1), 0);
}

TEST(test_head_parse) {
    HeadTarget t;
    std::string line = head(32768, Op::Write, 128);
    CHECK(activity_link::parseHeadLine(line.data(), line.size() - 2, t));
    CHECK(t.pos == 32768 && t.op == Op::Write && t.sectors == 128);
    const char* lower = "$HDE,2,0,R,8*21";
    CHECK(activity_link::parseHeadLine(lower, std::strlen(lower), t));
    std::string later = withChecksum("HDE,3,10,-,0,extra");
    CHECK(activity_link::parseHeadLine(later.data(), later.size(), t));
    CHECK(t.pos == 10 && t.op == Op::None);
    for (const char* body : {"HDE,1,0,R,8", "HDE,2,65536,R,8", "HDE,2,0,X,8", "HDE,2,0,R", "HDE,2,,R,8",
                             "HDA,2,0,R,8"}) {
        std::string bad = withChecksum(body);
        CHECK(!activity_link::parseHeadLine(bad.data(), bad.size(), t));
    }
    const char* bad_sum = "$HDE,2,0,R,8*22";
    CHECK(!activity_link::parseHeadLine(bad_sum, std::strlen(bad_sum), t));
    Fields f;
    CHECK(!activity_link::parseLine(lower, std::strlen(lower), f));  // not an $HDA
}

static HeadTarget target(uint16_t pos) {
    HeadTarget t;
    t.pos = pos;
    t.op = Op::Read;
    t.sectors = 8;
    return t;
}

TEST(test_head_pacer_keeps_the_latest) {
    HeadPacer p(2000);
    CHECK(!p.due(0));
    p.post(target(1), 1000);
    CHECK(p.due(1000));
    p.sent(1000);
    CHECK_EQ(p.count(), 1u);
    // Three more within 2 ms: only the last goes, 2 ms after the first.
    p.post(target(2), 1200);
    p.post(target(3), 1500);
    p.post(target(4), 1800);
    CHECK(!p.due(2999));
    CHECK(p.due(3000));
    CHECK_EQ(p.target().pos, 4);
    p.sent(3000);
    CHECK_EQ(p.replaced(), 2u);
    CHECK_EQ(p.last().pos, 4);
    CHECK(!p.due(10000));  // nothing new
}

TEST(test_head_pacer_waits_for_the_seek) {
    HeadPacer p(2000);
    p.post(target(7), 50000);  // the head starts moving at 50 ms
    CHECK(!p.due(49999));
    CHECK(p.due(50000));
}

TEST(test_head_waits_for_a_due_activity_sentence) {
    activity_link::Sender a(200);
    HeadPacer h(2000);
    Fields idle;
    // Nothing sent yet: $HDA is due, and goes first.
    h.post(target(9), 0);
    CHECK(activity_link::nextSentence(a, idle, 0, h, 0) == activity_link::Next::Activity);
    a.sent(idle, 0);
    CHECK(activity_link::nextSentence(a, idle, 1, h, 1000) == activity_link::Next::Head);
    h.sent(1000);
    CHECK(activity_link::nextSentence(a, idle, 2, h, 2000) == activity_link::Next::Nothing);
    // A change and a target together: the change first, the target in the gap after.
    Fields busy = idle;
    busy.busy = true;
    h.post(target(10), 3000);
    CHECK(activity_link::nextSentence(a, busy, 5, h, 5000) == activity_link::Next::Activity);
    a.sent(busy, 5);
    CHECK(activity_link::nextSentence(a, busy, 7, h, 7000) == activity_link::Next::Head);
    // The heartbeat still goes on time during a stream of targets.
    CHECK(activity_link::nextSentence(a, busy, 205, h, 205000) == activity_link::Next::Activity);
}

void runActivityLinkTests() {
    RUN(test_head_known_answers);
    RUN(test_head_position);
    RUN(test_head_parse);
    RUN(test_head_pacer_keeps_the_latest);
    RUN(test_head_pacer_waits_for_the_seek);
    RUN(test_head_waits_for_a_due_activity_sentence);
    RUN(test_link_parse_known_answers);
    RUN(test_link_parse_rejects);
    RUN(test_link_parse_accepts_later_versions);
    RUN(test_link_receiver_assembles_lines);
    RUN(test_link_receiver_rejects_and_recovers);
    RUN(test_link_receiver_line_length_limit);
    RUN(test_link_known_answers);
    RUN(test_link_format_needs_room);
    RUN(test_link_op_of_commands);
    RUN(test_link_sends_on_change_and_heartbeat);
    RUN(test_link_unsent_stays_due);
    RUN(test_link_heartbeat_across_wrap);
}
