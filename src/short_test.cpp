#include "short_test.h"

#include <cstdio>

#include "config.h"

namespace short_test {

uint32_t drivenMask() {
    uint32_t m = 0;
    for (unsigned p = cfg::BUS_FIRST_PIN; p <= cfg::BUS_LAST_PIN; ++p)
        if (p != cfg::PIN_BOARD_LED) m |= 1u << p;
    return m;
}

uint32_t observedMask() { return drivenMask() | 0xFu | (1u << 31) | EXPECTED_LOW; }

const char* signalName(unsigned gpio) {
    static const char* const names[32] = {
        "GPIO0",          "GPIO1",          "GPIO2 (I2C SDA)", "GPIO3 (I2C SCL)",
        "GPIO4 (DD0)",    "GPIO5 (DD1)",    "GPIO6 (DD2)",     "GPIO7 (DD3)",
        "GPIO8 (DD4)",    "GPIO9 (DD5)",    "GPIO10 (DD6)",    "GPIO11 (DD7)",
        "GPIO12 (DD8)",   "GPIO13 (DD9)",   "GPIO14 (DD10)",   "GPIO15 (DD11)",
        "GPIO16 (DD12)",  "GPIO17 (DD13)",  "GPIO18 (DD14)",   "GPIO19 (DD15)",
        "GPIO20 (DA0)",   "GPIO21 (DA1)",   "GPIO22 (DA2)",    "GPIO23 (CS0#)",
        "GPIO24 (CS1#)",  "GPIO25 (LED)",   "GPIO26 (DIOR#)",  "GPIO27 (DIOW#)",
        "GPIO28 (IORDY)", "GPIO29 (INTRQ)", "GPIO30 (RESET#)", "GPIO31",
    };
    return gpio < 32 ? names[gpio] : "?";
}

Result analyse(uint32_t baseline_low, const uint32_t low_when_driven[32]) {
    Result r;
    uint32_t observed = observedMask();
    r.stuck_low = baseline_low & observed & ~EXPECTED_LOW;
    r.untested = drivenMask() & baseline_low;
    for (unsigned a = 0; a < 32; ++a) {
        if (!((drivenMask() >> a) & 1) || ((r.untested >> a) & 1)) continue;
        // Pins that went low because a was driven, beyond those already low.
        uint32_t followers = low_when_driven[a] & observed & ~baseline_low & ~(1u << a);
        for (unsigned b = 0; b < 32; ++b) {
            if ((followers >> b) & 1) {
                r.bridged[a] |= 1u << b;
                r.bridged[b] |= 1u << a;
            }
        }
    }
    for (unsigned a = 0; a < 32; ++a)
        for (unsigned b = a + 1; b < 32; ++b)
            if ((r.bridged[a] >> b) & 1) ++r.pairs;
    return r;
}

void report(const Result& r, void (*emit)(const char*, void*), void* ctx) {
    char line[96];
    if (r.ok()) {
        unsigned n = 0;
        for (uint32_t m = drivenMask(); m; m &= m - 1) ++n;
        std::snprintf(line, sizeof line, "SHORTTEST OK %u pins", n);
        emit(line, ctx);
        return;
    }
    for (unsigned g = 0; g < 32; ++g) {
        if ((r.stuck_low >> g) & 1) {
            std::snprintf(line, sizeof line, "STUCK %s low", signalName(g));
            emit(line, ctx);
        }
    }
    for (unsigned a = 0; a < 32; ++a) {
        for (unsigned b = a + 1; b < 32; ++b) {
            if ((r.bridged[a] >> b) & 1) {
                std::snprintf(line, sizeof line, "SHORT %s %s", signalName(a), signalName(b));
                emit(line, ctx);
            }
        }
    }
    unsigned stuck = 0;
    for (uint32_t m = r.stuck_low; m; m &= m - 1) ++stuck;
    std::snprintf(line, sizeof line, "SHORTTEST FAIL %u stuck, %u bridged", stuck, r.pairs);
    emit(line, ctx);
}

}  // namespace short_test
