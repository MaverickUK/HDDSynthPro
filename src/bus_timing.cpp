#include "bus_timing.h"

#include <Arduino.h>
#include <hardware/clocks.h>
#include <hardware/pio.h>

#include "config.h"
#include "bus_timing.pio.h"

namespace bus_timing {

static PIO s_pio;
static uint s_sm;
static bool s_running = false;

static uint32_t s_count = 0;
static uint32_t s_t1_min = UINT32_MAX, s_t1_max = 0, s_strobe_min = UINT32_MAX, s_strobe_max = 0;
static uint32_t s_t1_ns[PRINT_LIMIT], s_strobe_ns[PRINT_LIMIT];
static unsigned s_kept = 0, s_printed = 0;

// Passes of the program's loops to ns: a t1 pass is 6 cycles, a strobe pass 2.
static constexpr uint32_t T1_LATE_NS = 20, STROBE_LATE_NS = 35;
static uint32_t cyclesToNs(uint32_t cycles) {
    return uint32_t(uint64_t(cycles) * 1000000000ull / clock_get_hz(clk_sys));
}

bool begin() {
    uint offset;
    s_running = pio_claim_free_sm_and_add_program_for_gpio_range(
        &bus_timing_program, &s_pio, &s_sm, &offset, cfg::PIN_DD0, cfg::PIN_RESET_N - cfg::PIN_DD0 + 1, true);
    if (!s_running) return false;
    pio_sm_config c = bus_timing_program_get_default_config(offset);
    sm_config_set_in_pins(&c, cfg::PIN_DD0);
    sm_config_set_jmp_pin(&c, cfg::PIN_DIOR_N);
    sm_config_set_in_shift(&c, false, false, 32);  // shift left: `in x, 16` puts t1 under the strobe
    sm_config_set_out_shift(&c, true, false, 32);
    pio_sm_init(s_pio, s_sm, offset, &c);
    pio_sm_set_enabled(s_pio, s_sm, true);
    return true;
}

bool running() { return s_running; }

void poll() {
    if (!s_running) return;
    while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm)) {
        uint32_t w = pio_sm_get(s_pio, s_sm);
        // Plus what the loops take to notice the edges, on average (t1 reads
        // 10-30 ns short, the strobe 20-50 ns: test_bus_timing_sim.py).
        uint32_t t1 = cyclesToNs((w & 0xFFFF) * 6) + T1_LATE_NS;
        uint32_t strobe = cyclesToNs((w >> 16) * 2) + STROBE_LATE_NS;
        ++s_count;
        if (t1 < s_t1_min) s_t1_min = t1;
        if (t1 > s_t1_max) s_t1_max = t1;
        if (strobe < s_strobe_min) s_strobe_min = strobe;
        if (strobe > s_strobe_max) s_strobe_max = strobe;
        if (s_kept < PRINT_LIMIT) {
            s_t1_ns[s_kept] = t1;
            s_strobe_ns[s_kept] = strobe;
            ++s_kept;
        }
    }
}

void printPending(unsigned max) {
    while (max-- && s_printed < s_kept && Serial.availableForWrite() > 48) {
        Serial.printf("RDTIME %u t1_ns=%lu strobe_ns=%lu\n", s_printed, (unsigned long)s_t1_ns[s_printed],
                      (unsigned long)s_strobe_ns[s_printed]);
        ++s_printed;
    }
}

uint32_t count() { return s_count; }
uint32_t t1MinNs() { return s_count ? s_t1_min : 0; }
uint32_t t1MaxNs() { return s_t1_max; }
uint32_t strobeMinNs() { return s_count ? s_strobe_min : 0; }
uint32_t strobeMaxNs() { return s_strobe_max; }

void reset() {
    s_count = 0;
    s_t1_min = s_strobe_min = UINT32_MAX;
    s_t1_max = s_strobe_max = 0;
    s_kept = s_printed = 0;
}

}  // namespace bus_timing
