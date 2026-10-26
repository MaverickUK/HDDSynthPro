#include "iordy_timer.h"

#include <atomic>

#include <hardware/clocks.h>
#include <hardware/pio.h>
#include <hardware/structs/busctrl.h>
#include <pico/time.h>

#include "bus_sample.h"
#include "config.h"
#include "iordy_timer.pio.h"

namespace iordy_timer {

static PIO s_pio;
static uint s_sm;
static uint32_t s_threshold_iterations;
static uint32_t s_slow = 0;
static uint32_t s_max_iterations = 0;
static const char* s_max_register = "-";
static uint32_t s_max_at_us = 0;
static uint32_t s_early = 0;
static const char* s_early_register = "-";
// Core 1 starts the program while core 0 may already be polling.
static std::atomic<bool> s_started{false};
static std::atomic<bool> s_reset_requested{false};
// Reads past the limit, waiting for takeOverLimit(); core 0 only.
static OverLimit s_over[8];
static unsigned s_over_head = 0, s_over_count = 0;

// One loop iteration is 2 PIO cycles; the wait and the three instructions
// before the loop add 4.
static uint32_t iterationsToNs(uint32_t iterations) {
    return uint32_t((2ull * iterations + 4) * 1000000000ull / clock_get_hz(clk_sys));
}

void begin(uint32_t threshold_ns) {
    uint offset;
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &iordy_timer_program, &s_pio, &s_sm, &offset, cfg::PIN_DD0,
        cfg::PIN_IORDY - cfg::PIN_DD0 + 1, true);
    hard_assert(ok);

    pio_sm_config c = iordy_timer_program_get_default_config(offset);
    sm_config_set_in_pins(&c, cfg::PIN_DD0);
    sm_config_set_in_shift(&c, true, false, 32);  // `in` shifts right: new bits on top
    sm_config_set_jmp_pin(&c, cfg::PIN_IORDY);
    pio_sm_init(s_pio, s_sm, offset, &c);

    uint64_t cycles = uint64_t(threshold_ns) * clock_get_hz(clk_sys) / 1000000000ull;
    s_threshold_iterations = uint32_t(cycles / 2);
    pio_sm_put(s_pio, s_sm, s_threshold_iterations);
    pio_sm_set_enabled(s_pio, s_sm, true);

    busctrl_hw->counter[0].sel = BUSCTRL_PERFSEL0_VALUE_FASTPERI_ACCESS_CONTESTED;
    busctrl_hw->counter[1].sel = BUSCTRL_PERFSEL0_VALUE_FASTPERI_STALL_UPSTREAM;
    busctrl_hw->perfctr_en = 1;
    s_started.store(true, std::memory_order_release);
}

void poll() {
    if (!s_started.load(std::memory_order_acquire)) return;
    while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm)) {
        uint32_t report = pio_sm_get(s_pio, s_sm);
        uint32_t sample = report << 16;  // back to the sample layout
        if (bus::high(sample, cfg::PIN_IORDY)) {  // started before IORDY was low
            ++s_early;
            s_early_register = bus::registerName(bus::block(sample), bus::address(sample), false);
            continue;
        }
        uint32_t iterations = s_threshold_iterations + (0xFFFFu - (report >> 16));
        const char* reg = bus::registerName(bus::block(sample), bus::address(sample), false);
        ++s_slow;
        if (iterations > s_max_iterations) {
            s_max_iterations = iterations;
            s_max_register = reg;
            s_max_at_us = time_us_32();
        }
        uint32_t ns = iterationsToNs(iterations);
        if (ns > cfg::IORDY_LIMIT_NS) {
            s_over[(s_over_head + s_over_count) % 8] = OverLimit{ns, reg, time_us_32()};
            if (s_over_count < 8)
                ++s_over_count;
            else
                s_over_head = (s_over_head + 1) % 8;  // full: drop the oldest
        }
    }
    if (s_reset_requested.exchange(false, std::memory_order_relaxed)) reset();
}

uint32_t slowReads() { return s_slow; }
uint32_t maxNs() { return s_max_iterations ? iterationsToNs(s_max_iterations) : 0; }
const char* maxRegister() { return s_max_register; }
uint32_t maxAtUs() { return s_max_at_us; }
uint32_t earlyReads() { return s_early; }
const char* earlyRegister() { return s_early_register; }
bool takeOverLimit(OverLimit& read) {
    if (s_over_count == 0) return false;
    read = s_over[s_over_head];
    s_over_head = (s_over_head + 1) % 8;
    --s_over_count;
    return true;
}

uint32_t contestedAccesses() { return busctrl_hw->counter[0].value; }
uint32_t stallCycles() { return busctrl_hw->counter[1].value; }

void requestReset() { s_reset_requested.store(true, std::memory_order_relaxed); }

void reset() {
    s_slow = 0;
    s_max_iterations = 0;
    s_max_register = "-";
    s_max_at_us = 0;
    s_early = 0;
    s_early_register = "-";
    busctrl_hw->counter[0].value = 0;  // any write clears a counter
    busctrl_hw->counter[1].value = 0;
}

}  // namespace iordy_timer
