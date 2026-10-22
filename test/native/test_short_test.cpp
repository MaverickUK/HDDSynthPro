// Tests for the solder-bridge analysis (short_test), against simulated
// boards with known faults.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "check.h"
#include "short_test.h"

using namespace short_test;

// A simulated board: groups of pins bridged together, and pins shorted to GND.
struct Board {
    std::vector<uint32_t> groups;  // each a mask of mutually bridged pins
    uint32_t to_gnd = 0;

    uint32_t groupOf(unsigned pin) const {
        for (uint32_t g : groups)
            if ((g >> pin) & 1) return g;
        return 1u << pin;
    }
    // Low with only pull-ups: GND shorts, the LED's pull-down, and anything bridged to them.
    uint32_t baseline() const {
        uint32_t low = 0;
        for (unsigned p = 0; p < 32; ++p)
            if (((to_gnd | EXPECTED_LOW) >> p) & 1) low |= groupOf(p);
        return low & observedMask();
    }
    void measure(uint32_t& base, uint32_t low_when_driven[32]) const {
        base = baseline();
        for (unsigned p = 0; p < 32; ++p)
            low_when_driven[p] = ((drivenMask() >> p) & 1) ? (base | groupOf(p)) & observedMask() : 0;
    }
};

static std::vector<std::string> run(const Board& b, Result* out = nullptr) {
    uint32_t base, low[32];
    b.measure(base, low);
    Result r = analyse(base, low);
    if (out) *out = r;
    std::vector<std::string> lines;
    report(r, [](const char* line, void* ctx) { static_cast<std::vector<std::string>*>(ctx)->push_back(line); },
           &lines);
    return lines;
}

static uint32_t pins(std::initializer_list<unsigned> list) {
    uint32_t m = 0;
    for (unsigned p : list) m |= 1u << p;
    return m;
}

TEST(test_short_masks) {
    CHECK_EQ(drivenMask(), 0x7DFFFFF0u);  // GPIO 4-30 except 25
    CHECK((observedMask() & 0xF) == 0xF);
    CHECK((observedMask() >> 31) & 1);
    CHECK_STR(signalName(26), "GPIO26 (DIOR#)");
}

TEST(test_short_clean_board) {
    Result r;
    auto lines = run(Board{}, &r);
    CHECK(r.ok());
    CHECK_EQ(lines.size(), 1u);
    CHECK_STR(lines[0], "SHORTTEST OK 26 pins");
}

TEST(test_short_one_bridge) {
    Result r;
    auto lines = run(Board{{pins({9, 10})}}, &r);
    CHECK_EQ(r.pairs, 1u);
    CHECK_EQ(lines.size(), 2u);
    CHECK_STR(lines[0], "SHORT GPIO9 (DD5) GPIO10 (DD6)");
    CHECK_STR(lines[1], "SHORTTEST FAIL 0 stuck, 1 bridged");
}

TEST(test_short_three_way_bridge) {
    Result r;
    run(Board{{pins({9, 10, 11})}}, &r);
    CHECK_EQ(r.pairs, 3u);
}

TEST(test_short_to_an_observed_neighbour) {
    // DD0 (GPIO4) bridged to GPIO3 next to it on the header.
    auto lines = run(Board{{pins({3, 4})}});
    CHECK_STR(lines[0], "SHORT GPIO3 (I2C SCL) GPIO4 (DD0)");
}

TEST(test_short_to_ground) {
    // CS1# (GPIO24) is next to a GND pin on EXT2.
    Result r;
    auto lines = run(Board{{}, pins({24})}, &r);
    CHECK_EQ(r.stuck_low, pins({24}));
    CHECK_EQ(r.untested, pins({24}));  // never driven
    CHECK_STR(lines[0], "STUCK GPIO24 (CS1#) low");
    CHECK_STR(lines.back(), "SHORTTEST FAIL 1 stuck, 0 bridged");
}

TEST(test_short_to_the_led) {
    // DIOR# (GPIO26) bridged to the LED pin next to it: pulled low by the LED.
    Result r;
    auto lines = run(Board{{pins({25, 26})}}, &r);
    CHECK_EQ(r.stuck_low, pins({26}));
    CHECK_STR(lines[0], "STUCK GPIO26 (DIOR#) low");
}

TEST(test_short_separate_faults) {
    Result r;
    run(Board{{pins({5, 6}), pins({20, 21})}, pins({30})}, &r);
    CHECK_EQ(r.pairs, 2u);
    CHECK_EQ(r.stuck_low, pins({30}));
    CHECK(!r.ok());
}

void runShortTestTests() {
    RUN(test_short_masks);
    RUN(test_short_clean_board);
    RUN(test_short_one_bridge);
    RUN(test_short_three_way_bridge);
    RUN(test_short_to_an_observed_neighbour);
    RUN(test_short_to_ground);
    RUN(test_short_to_the_led);
    RUN(test_short_separate_faults);
}
