// Host-side tests for media_watch: a card pulled and put back is served
// again only if it's the same card with the same image, unchanged.
#include <string>

#include "check.h"
#include "media_watch.h"

using media_watch::Identity;
using media_watch::State;
using media_watch::Watch;

static Identity card() {
    Identity id;
    id.maker = 0x03;
    id.serial = 0x12345678;
    id.size = 64ull << 20;
    id.first_sector = 32768;
    id.date = 0x5B5C;
    id.time = 0x6000;
    return id;
}

TEST(test_media_none_until_served) {
    Watch w;
    CHECK(w.state() == State::None);
    CHECK(w.usable());
    CHECK(!w.failed(10));  // the generated disk never fails over to "lost"
    CHECK(!w.due(100000));
    CHECK_STR(std::string(media_watch::stateName(w.state())), "none");
}

TEST(test_media_same_card_comes_back) {
    Watch w;
    w.serve(card());
    CHECK(w.usable());
    CHECK(w.failed(1000));
    CHECK(!w.failed(1001));  // reported once
    CHECK(w.state() == State::Lost);
    CHECK(!w.usable());
    CHECK_EQ(w.losses(), 1u);
    CHECK(w.cameBack(card(), 1500) == State::Present);
    CHECK(w.usable());
    CHECK_EQ(w.returns(), 1u);
}

TEST(test_media_probes_at_its_interval) {
    Watch w;
    w.serve(card());
    CHECK(!w.due(5000));  // present: no probing, no SPI traffic while serving
    w.failed(1000);
    CHECK(!w.due(1000 + media_watch::PROBE_MS - 1));
    CHECK(w.due(1000 + media_watch::PROBE_MS));
    CHECK(!w.due(1000 + media_watch::PROBE_MS + 1));
    CHECK(w.due(1000 + 2 * media_watch::PROBE_MS));
}

TEST(test_media_probes_across_millis_wrap) {
    Watch w;
    w.serve(card());
    w.failed(0xFFFFFF00u);
    CHECK(!w.due(0xFFFFFF10u));
    CHECK(w.due(0xFFFFFF00u + media_watch::PROBE_MS));  // wrapped past 0
}

TEST(test_media_another_card_is_not_served) {
    Watch w;
    w.serve(card());
    w.failed(0);
    Identity other = card();
    other.serial ^= 1;
    CHECK(w.cameBack(other, 300) == State::Changed);
    CHECK(!w.usable());
    CHECK_STR(std::string(w.why()), "another card");
    CHECK_EQ(w.returns(), 0u);
    // Checked less often while changed, then taken out: lost again, and the
    // right card is served when it comes back.
    CHECK(!w.due(300 + media_watch::CHANGED_CHECK_MS - 1));
    CHECK(w.due(300 + media_watch::CHANGED_CHECK_MS));
    w.gone(1400);
    CHECK(w.state() == State::Lost);
    CHECK(w.cameBack(card(), 1700) == State::Present);
    CHECK_EQ(w.returns(), 1u);
}

TEST(test_media_changed_image_is_not_served) {
    Identity served = card();
    Identity edited = card();
    edited.time += 1;
    CHECK_STR(std::string(media_watch::difference(served, edited)), "image modified");
    Identity copied = card();
    copied.first_sector += 64;  // the same name, written again elsewhere
    CHECK_STR(std::string(media_watch::difference(served, copied)), "image replaced");
    Identity resized = card();
    resized.size += 512;
    CHECK_STR(std::string(media_watch::difference(served, resized)), "image replaced");
    Identity maker = card();
    maker.maker = 0x1B;
    CHECK_STR(std::string(media_watch::difference(served, maker)), "another card");
    CHECK(media_watch::difference(served, card()) == nullptr);
}

TEST(test_media_card_without_the_image) {
    Watch w;
    w.serve(card());
    w.failed(0);
    w.cameBackWithout("not found", 250);
    CHECK(w.state() == State::Changed);
    CHECK_STR(std::string(w.why()), "not found");
    // The right card later: served again (cameBack works from Changed too).
    CHECK(w.cameBack(card(), 2000) == State::Present);
}

TEST(test_media_events_out_of_state_are_ignored) {
    Watch w;
    w.serve(card());
    CHECK(w.cameBack(card(), 0) == State::Present);  // never lost
    CHECK_EQ(w.returns(), 0u);
    w.gone(0);  // only meaningful while changed
    CHECK(w.state() == State::Present);
    w.cameBackWithout("not found", 0);
    CHECK(w.state() == State::Present);
}

void runMediaWatchTests() {
    RUN(test_media_none_until_served);
    RUN(test_media_same_card_comes_back);
    RUN(test_media_probes_at_its_interval);
    RUN(test_media_probes_across_millis_wrap);
    RUN(test_media_another_card_is_not_served);
    RUN(test_media_changed_image_is_not_served);
    RUN(test_media_card_without_the_image);
    RUN(test_media_events_out_of_state_are_ignored);
}
