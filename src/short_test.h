// Solder-bridge test for the hand-wired bus: which pins are connected to
// which. Pure C++ (the analysis and the report); the hardware side, which
// pulls pins low one at a time, is in main.cpp. Host-tested.
//
// Method, with the IDE cable unplugged: every observed pin gets its pull-up,
// so all read high. Pins that read low anyway are "stuck low" (bridged to
// GND, or to the LED's pull-down). Then each bus pin in turn is driven low:
// any other pin that follows it is bridged to it.
//
// It can't see a bridge to a 3.3 V or 5 V rail (that would need pull-downs,
// which erratum E9 rules out), or a wire on the wrong header pin.
#pragma once

#include <cstddef>
#include <cstdint>

namespace short_test {

// GPIO 0-31: the bus pins (driven and observed) plus their header
// neighbours GPIO 0-3 and 31 (observed only). GPIO 25 (the LED, pulled down
// through it) is observed but expected low.
uint32_t drivenMask();
uint32_t observedMask();
constexpr uint32_t EXPECTED_LOW = 1u << 25;

// A short name for a GPIO in this rig, e.g. "GPIO9 (DD5)".
const char* signalName(unsigned gpio);

struct Result {
    uint32_t stuck_low = 0;     // read low with nothing driven
    uint32_t bridged[32] = {};  // bridged[a] has bit b set if a and b are connected
    uint32_t untested = 0;      // driven pins skipped because they were stuck low
    unsigned pairs = 0;         // distinct bridged pairs
    bool ok() const { return stuck_low == 0 && pairs == 0; }
};

// baseline_low: pins reading low before anything is driven.
// low_when_driven[p]: pins reading low while GPIO p is driven low (only
// for driven pins that were tested).
Result analyse(uint32_t baseline_low, const uint32_t low_when_driven[32]);

// Writes the report one line at a time to `emit`:
//   SHORTTEST OK <n> pins
//   STUCK <name> low
//   SHORT <name> <name>
//   SHORTTEST FAIL <stuck> stuck, <pairs> bridged
void report(const Result& r, void (*emit)(const char* line, void* ctx), void* ctx);

}  // namespace short_test
