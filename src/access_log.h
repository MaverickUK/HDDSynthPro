// A bench diagnostic, built in only with -DHDDPRO_ACCESS_LOG=1: what core 1
// was doing just before it answered a read late.
//
// SLOWREAD names a read IORDY held past ATA's 1,250 ns, but not why: core 0
// prints it up to half a millisecond later. With this built in, core 1 keeps
// a ring of its latest work (each access it took from the FIFO, each command
// it ran, each loop step that took over 0.5 us, the FIFO found empty), with
// its cycle counter. Answering a read the host is already strobing (DIOR#
// low) after more than 1.25 us busy, it freezes a copy, and core 0 prints it
// as ACCLOG lines (main.cpp, printAccessLog): times in ns before the late
// answer. One copy at a time; later late reads wait until it's printed.
//
// Off (the default) every call compiles to nothing, so the normal builds'
// core 1 code is unchanged (scripts/check_core1_layout.py). On, each access
// costs a few cycles, which moves core 1's timings a little: compare like
// with like.
#pragma once

#include <cstdint>

#ifndef HDDPRO_ACCESS_LOG
#define HDDPRO_ACCESS_LOG 0
#endif

#if defined(ARDUINO)
#include <hardware/gpio.h>
#include <hardware/structs/m33.h>

#include "config.h"
#endif

namespace access_log {

constexpr bool ON = HDDPRO_ACCESS_LOG != 0;

enum Kind : uint8_t {
    ACCESS,   // value: the bus sample
    EMPTY,    // the RX FIFO found empty
    COMMAND,  // value: 0 as runQueuedCommand starts, 1 as it returns
    STEP,     // value: the loop step, | its cycles << 8 (only steps over 100 cycles)
    LATE,     // value: cycles since the FIFO was last empty
};

struct Entry {
    uint32_t cycles;
    uint32_t value;
    uint8_t kind;
};

constexpr unsigned SIZE = 48;
constexpr uint32_t LATE_CYCLES = 250;  // 1.25 us at 200 MHz
constexpr uint32_t STEP_CYCLES = 100;  // 0.5 us

inline Entry g_ring[SIZE];
inline volatile uint32_t g_next = 0;
inline volatile uint32_t g_empty_at = 0;
inline volatile uint8_t g_last_kind = EMPTY;
inline Entry g_frozen[SIZE];          // oldest first, the LATE entry last
inline volatile bool g_frozen_ready = false;  // set by core 1, cleared by core 0 once printed

#if defined(ARDUINO)
__attribute__((always_inline)) inline uint32_t now() { return m33_hw->dwt_cyccnt; }

__attribute__((always_inline)) inline void note(Kind kind, uint32_t value) {
    if constexpr (ON) {
        uint32_t i = g_next;
        Entry& e = g_ring[i % SIZE];
        e.cycles = now();
        e.value = value;
        e.kind = kind;
        g_next = i + 1;
        g_last_kind = kind;
    }
}

// The FIFO found empty: the start of any backlog that follows.
__attribute__((always_inline)) inline void empty() {
    if constexpr (ON) {
        g_empty_at = now();
        if (g_last_kind != EMPTY) note(EMPTY, 0);
    }
}

// A loop step just done, started at `started`: logged only if it was long.
__attribute__((always_inline)) inline void step(unsigned which, uint32_t started) {
    if constexpr (ON) {
        uint32_t took = now() - started;
        if (took > STEP_CYCLES) note(STEP, which | took << 8);
    }
}

// Just after answering a read: freeze the ring if the host was kept waiting.
__attribute__((always_inline)) inline void answered() {
    if constexpr (ON) {
        if (g_frozen_ready) return;
        uint32_t busy = now() - g_empty_at;
        if (busy < LATE_CYCLES || gpio_get(cfg::PIN_DIOR_N)) return;  // in time, or not strobed yet
        note(LATE, busy);
        uint32_t end = g_next;
        for (unsigned k = 0; k < SIZE; ++k) g_frozen[k] = g_ring[(end + k) % SIZE];
        g_frozen_ready = true;
    }
}
#else
inline void note(Kind, uint32_t) {}
inline void empty() {}
inline void step(unsigned, uint32_t) {}
inline void answered() {}
#endif

}  // namespace access_log
