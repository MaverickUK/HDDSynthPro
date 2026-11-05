// Host-side tests for card_retry: restarting a card that stopped answering,
// and device 1's reads, writes and syncs retried once after a restart, only
// on the same card with device 1's file the same size.
#include <string>
#include <vector>

#include "card_retry.h"
#include "check.h"

using media_watch::Identity;
using media_watch::State;
using media_watch::Watch;

namespace {

// A card that does what each test scripts, and records what was asked of it.
struct FakeCard {
    bool answers_ok = true, start_ok = true, reattach_ok = true;
    std::vector<bool> reopen_results;  // one per reopenDevice1(), then true
    std::vector<std::string> calls;
    uint32_t restarts = 0;

    uint32_t now() { return 1000; }
    bool answers() { calls.push_back("answers"); return answers_ok; }
    bool start() { calls.push_back("start"); return start_ok; }
    bool reattach(uint32_t) {
        calls.push_back("reattach");
        if (reattach_ok) ++restarts;  // as sd_card's reattach counts its own
        return reattach_ok;
    }
    void closeDevice1() { calls.push_back("close"); }
    bool reopenDevice1() {
        calls.push_back("reopen");
        if (reopen_results.empty()) return true;
        bool ok = reopen_results.front();
        reopen_results.erase(reopen_results.begin());
        return ok;
    }
    void countRestart() { ++restarts; }
    std::string log() const {
        std::string s;
        for (const auto& c : calls) s += (s.empty() ? "" : " ") + c;
        return s;
    }
};

// An operation (a read, write or sync) that gives each result in turn.
struct FakeOp {
    std::vector<bool> results;
    int calls = 0;
    bool operator()() { return results[size_t(calls++)]; }
};

Identity card() {
    Identity id;
    id.maker = 0x03;
    id.serial = 0x12345678;
    id.size = 64ull << 20;
    return id;
}

Watch serving() {  // device 0's image on the card
    Watch w;
    w.serve(card());
    return w;
}

// SdFat's FsFile, as card_retry::reopen uses it.
struct FakeFile {
    bool is_open = false, open_ok = true;
    uint64_t size = 4096;
    std::string opened_name;
    int opened_flags = -1, opens = 0, closes = 0;
    bool isOpen() const { return is_open; }
    bool open(const char* name, int flags) {
        ++opens;
        opened_name = name;
        opened_flags = flags;
        is_open = open_ok;
        return open_ok;
    }
    uint64_t fileSize() const { return size; }
    void close() { ++closes; is_open = false; }
};

}  // namespace

// --- device1Attempt -------------------------------------------------------

TEST(test_retry_success_needs_no_restart) {
    Watch w = serving();
    FakeCard c;
    FakeOp op{{true}};
    CHECK(card_retry::device1Attempt(w, c, op));
    CHECK_EQ(op.calls, 1);
    CHECK_STR(c.log(), "reopen");
    CHECK(w.state() == State::Present);
    CHECK_EQ(w.losses(), 0u);
}

TEST(test_retry_after_restart_succeeds) {
    Watch w = serving();
    FakeCard c;
    FakeOp op{{false, true}};
    CHECK(card_retry::device1Attempt(w, c, op));
    CHECK_EQ(op.calls, 2);
    CHECK_STR(c.log(), "reopen reattach reopen");
    CHECK_EQ(c.restarts, 1u);
    CHECK_EQ(w.losses(), 1u);  // the restart is a loss and return: a `CARD lost and back` line
}

TEST(test_retry_only_once) {
    Watch w = serving();
    FakeCard c;
    FakeOp op{{false, false}};
    CHECK(!card_retry::device1Attempt(w, c, op));
    CHECK_EQ(op.calls, 2);  // not a third: a dead card costs one restart per read, not a loop
}

TEST(test_retry_not_after_failed_restart) {
    Watch w = serving();
    FakeCard c;
    c.reattach_ok = false;  // a different card, or no card
    FakeOp op{{false}};
    CHECK(!card_retry::device1Attempt(w, c, op));
    CHECK_EQ(op.calls, 1);
    CHECK_STR(c.log(), "reopen reattach");
}

TEST(test_retry_not_when_file_cannot_be_reopened) {
    Watch w = serving();
    FakeCard c;
    c.reopen_results = {true, false};  // after the restart: gone, or a different size
    FakeOp op{{false}};
    CHECK(!card_retry::device1Attempt(w, c, op));
    CHECK_EQ(op.calls, 1);  // never read from whatever file has that name now
}

TEST(test_retry_reopens_before_the_first_try) {
    // A restart by device 0's reads closed device 1's file: its next read
    // reopens it first, and fails at once if it can't.
    Watch w = serving();
    FakeCard c;
    c.reopen_results = {false};
    FakeOp op{{true}};
    CHECK(!card_retry::device1Attempt(w, c, op));
    CHECK_EQ(op.calls, 0);
    CHECK_STR(c.log(), "reopen");
}

TEST(test_retry_refused_while_card_lost_or_changed) {
    Watch lost = serving();
    lost.failed(500);
    FakeCard c;
    FakeOp op{{true}};
    CHECK(!card_retry::device1Attempt(lost, c, op));
    CHECK_EQ(op.calls, 0);
    CHECK(c.calls.empty());  // no SPI traffic: serviceMedia() looks for the card

    Watch changed = serving();
    changed.failed(500);
    Identity other = card();
    other.serial ^= 1;
    CHECK(changed.cameBack(other, 600) == State::Changed);
    CHECK(!card_retry::device1Attempt(changed, c, op));
    CHECK_EQ(op.calls, 0);
    CHECK(c.calls.empty());
}

TEST(test_retry_with_no_image_on_device_0) {
    // device0 = none (the generated disk) with device 1 on the card: nothing
    // to identify the card by, so any card that answers is restarted.
    Watch w;
    FakeCard c;
    FakeOp op{{false, true}};
    CHECK(card_retry::device1Attempt(w, c, op));
    CHECK_STR(c.log(), "reopen answers close start reopen");
    CHECK_EQ(c.restarts, 1u);
    CHECK(w.state() == State::None);  // nothing to watch still
}

// --- restart --------------------------------------------------------------

TEST(test_restart_present_reattaches) {
    Watch w = serving();
    FakeCard c;
    CHECK(card_retry::restart(w, c));
    CHECK_STR(c.log(), "reattach");  // reattach closes device 1's file itself
    CHECK(w.state() == State::Lost);  // the fake doesn't report back: sd_card's reattach does (cameBack)
    CHECK_EQ(w.losses(), 1u);
}

TEST(test_restart_none_without_a_card) {
    Watch w;
    FakeCard c;
    c.answers_ok = false;
    CHECK(!card_retry::restart(w, c));
    CHECK_STR(c.log(), "answers");  // no 2 s mount attempt, and device 1's file left alone
    CHECK_EQ(c.restarts, 0u);
}

TEST(test_restart_none_mount_fails) {
    Watch w;
    FakeCard c;
    c.start_ok = false;
    CHECK(!card_retry::restart(w, c));
    CHECK_STR(c.log(), "answers close start");  // closed before the mount, which drops its volume
    CHECK_EQ(c.restarts, 0u);
}

TEST(test_restart_refused_while_lost_or_changed) {
    Watch w = serving();
    w.failed(500);
    FakeCard c;
    CHECK(!card_retry::restart(w, c));
    CHECK(c.calls.empty());
    CHECK_EQ(w.losses(), 1u);
}

// --- reopen ---------------------------------------------------------------

TEST(test_reopen_open_file_untouched) {
    FakeFile f;
    f.is_open = true;
    CHECK(card_retry::reopen(f, "DOOM.ISO", 0, 4096));
    CHECK_EQ(f.opens, 0);
}

TEST(test_reopen_no_name) {
    FakeFile f;
    CHECK(!card_retry::reopen(f, "", 0, 4096));
    CHECK_EQ(f.opens, 0);
}

TEST(test_reopen_same_size) {
    FakeFile f;
    CHECK(card_retry::reopen(f, "DOOM.ISO", 2, 4096));
    CHECK_STR(f.opened_name, "DOOM.ISO");
    CHECK_EQ(f.opened_flags, 2);  // as first opened: read-only stays read-only
    CHECK(f.isOpen());
}

TEST(test_reopen_different_size_closed) {
    FakeFile f;
    f.size = 8192;  // another card's file of that name
    CHECK(!card_retry::reopen(f, "DOOM.ISO", 0, 4096));
    CHECK(!f.isOpen());
    CHECK_EQ(f.closes, 1);
}

TEST(test_reopen_missing) {
    FakeFile f;
    f.open_ok = false;
    CHECK(!card_retry::reopen(f, "DOOM.ISO", 0, 4096));
    CHECK(!f.isOpen());
}

void runCardRetryTests() {
    RUN(test_retry_success_needs_no_restart);
    RUN(test_retry_after_restart_succeeds);
    RUN(test_retry_only_once);
    RUN(test_retry_not_after_failed_restart);
    RUN(test_retry_not_when_file_cannot_be_reopened);
    RUN(test_retry_reopens_before_the_first_try);
    RUN(test_retry_refused_while_card_lost_or_changed);
    RUN(test_retry_with_no_image_on_device_0);
    RUN(test_restart_present_reattaches);
    RUN(test_restart_none_without_a_card);
    RUN(test_restart_none_mount_fails);
    RUN(test_restart_refused_while_lost_or_changed);
    RUN(test_reopen_open_file_untouched);
    RUN(test_reopen_no_name);
    RUN(test_reopen_same_size);
    RUN(test_reopen_different_size_closed);
    RUN(test_reopen_missing);
}
