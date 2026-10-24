#include "iordy_timer.h"

#include <atomic>

#include <hardware/clocks.h>
#include <hardware/pio.h>

#include "config.h"
#include "iordy_timer.pio.h"

namespace iordy_timer {

static PIO s_pio;
static uint s_sm;
static uint32_t s_threshold_iterations;
static uint32_t s_slow = 0;
static uint32_t s_max_iterations = 0;
// Core 1 starts the program while core 0 may already be polling.
static std::atomic<bool> s_started{false};
static std::atomic<bool> s_reset_requested{false};

// One loop iteration is 2 PIO cycles; the wait and mov before it add 2.
static uint32_t iterationsToNs(uint32_t iterations) {
    return uint32_t((2ull * iterations + 2) * 1000000000ull / clock_get_hz(clk_sys));
}

void begin(uint32_t threshold_ns) {
    uint offset;
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &iordy_timer_program, &s_pio, &s_sm, &offset, cfg::PIN_DIOR_N,
        cfg::PIN_IORDY - cfg::PIN_DIOR_N + 1, true);
    hard_assert(ok);

    pio_sm_config c = iordy_timer_program_get_default_config(offset);
    sm_config_set_jmp_pin(&c, cfg::PIN_IORDY);
    pio_sm_init(s_pio, s_sm, offset, &c);

    uint64_t cycles = uint64_t(threshold_ns) * clock_get_hz(clk_sys) / 1000000000ull;
    s_threshold_iterations = uint32_t(cycles / 2);
    pio_sm_put(s_pio, s_sm, s_threshold_iterations);
    pio_sm_set_enabled(s_pio, s_sm, true);
    s_started.store(true, std::memory_order_release);
}

void poll() {
    if (!s_started.load(std::memory_order_acquire)) return;
    while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm)) {
        uint32_t iterations = s_threshold_iterations + pio_sm_get(s_pio, s_sm);
        ++s_slow;
        if (iterations > s_max_iterations) s_max_iterations = iterations;
    }
    if (s_reset_requested.exchange(false, std::memory_order_relaxed)) reset();
}

uint32_t slowReads() { return s_slow; }
uint32_t maxNs() { return s_max_iterations ? iterationsToNs(s_max_iterations) : 0; }

void requestReset() { s_reset_requested.store(true, std::memory_order_relaxed); }

void reset() {
    s_slow = 0;
    s_max_iterations = 0;
}

}  // namespace iordy_timer
