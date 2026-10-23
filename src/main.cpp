// HDDSynthPro IDE drive emulator prototype.
//
// Two builds (FIRMWARE_SLICE, set in platformio.ini):
//   bb48        slice 0, listen only: core 1 drains the PIO listener and
//               records every bus access. Every pin stays an input.
//   bb48_drive  slices 1-3, the drive: core 1 serves the bus (ide_bus) and
//               runs the ATA device; core 0 fetches sectors from the
//               generated disk for it. Commands are traced; raw accesses
//               on request (console `t`).
// In both, core 1 records events into the trace ring and core 0 owns USB
// serial: it prints the trace and runs the console.
#include <Arduino.h>

#include <hardware/gpio.h>

#include <atomic>
#include <cstdio>

#include "bus_sample.h"
#include "config.h"
#include "core1_ram.h"
#include "short_test.h"
#include "spsc_ring.h"
#include "trace_format.h"
#include "version.h"

#if FIRMWARE_SLICE == 0
#include "ide_listen.h"
#else
#include <hardware/clocks.h>
#include <hardware/structs/m33.h>

#include "activity.h"
#include "ata_device.h"
#include "block_source.h"
#include "fetch_mailbox.h"
#include "ide_bus.h"
#endif

static SpscRing<TraceEvent, cfg::TRACE_RING_EVENTS> g_trace;
static std::atomic<uint32_t> g_events{0};   // recorded by core 1
static std::atomic<uint32_t> g_dropped{0};  // lost because the ring was full
static std::atomic<uint32_t> g_stalls{0};   // PIO FIFO filled: accesses missed

// Pins in sample layout (bit n = GPIO PIN_DD0 + n), for non-bus events.
CORE1_RAM static uint32_t pinsAsSample() { return gpio_get_all() >> cfg::PIN_DD0; }

CORE1_RAM static void record(const TraceEvent& e) {
    g_events.fetch_add(1, std::memory_order_relaxed);
    if (!g_trace.push(e)) g_dropped.fetch_add(1, std::memory_order_relaxed);
}

CORE1_RAM static void record(TraceKind kind, uint32_t pins) { record(TraceEvent{time_us_32(), pins, kind}); }

// Arduino-Pico runs a periodic SysTick interrupt on core 1 unless told not
// to; each one delays the bus loop by about 1 us. Core 1 needs no ticks.
bool core1_disable_systick = true;

// ---- Core 1: slice 0, listen only ---------------------------------------------
#if FIRMWARE_SLICE == 0

void setup1() { listen::begin(); }

CORE1_RAM void loop1() {
    // Never returns: one tight loop, so nothing slow ever sits between a
    // FIFO word arriving and it being timestamped.
    bool reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
    if (reset_low) record(TraceKind::ResetAssert, pinsAsSample());
    for (;;) {
        uint32_t sample;
        if (listen::pop(sample)) record(TraceKind::Bus, sample);

        uint32_t pins = pinsAsSample();
        bool low = !bus::high(pins, cfg::PIN_RESET_N);
        if (low != reset_low) {
            reset_low = low;
            record(low ? TraceKind::ResetAssert : TraceKind::ResetRelease, pins);
        }

        if (listen::takeStall()) {
            g_stalls.fetch_add(1, std::memory_order_relaxed);
            record(TraceKind::Stall, pins);
        }
    }
}

// ---- Core 1: the drive ----------------------------------------------------------
#else

static FetchMailbox g_mailbox;
static std::atomic<bool> g_trace_bus{false};       // console `t`
static std::atomic<uint32_t> g_read_max_cycles{0}; // longest a read waited for its answer (console `s`)
static std::atomic<bool> g_read_max_reset{false};

static const IdentifyInfo DRIVE_INFO{"HDDSYNTH PROTO", "HSP-000001", FIRMWARE_VERSION,
                                     gen_disk::DEFAULT_SECTORS};

void setup1() {
    ide_bus::begin();
    // The M33's cycle counter, to time reads: core 1 must answer one within
    // about 1 us (host/tests/test_pio_bus_sim.py).
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
}

// Serves one bus access if the PIO has one waiting. loop1() calls this
// between each of its steps, so a read never waits for a whole pass.
//
// It also times reads. `last_empty` is the last moment the FIFO was seen
// empty. Any access in the FIFO arrived after it, so the time from then until
// a read's answer has gone back to the PIO is an upper bound on how long the
// host waited. (While a read waits, the PIO pushes nothing behind it.)
CORE1_RAM static void serveBus(ata::Device& dev, uint32_t& last_empty) {
    uint32_t now = m33_hw->dwt_cyccnt;
    uint32_t sample;
    if (!ide_bus::serviceOnce(dev, sample)) {
        last_empty = now;
        return;
    }
    uint32_t done = m33_hw->dwt_cyccnt;
    if (bus::op(sample) != bus::Op::Write) {  // serviceOnce answered it
        uint32_t wait = done - last_empty;
        if (wait > g_read_max_cycles.load(std::memory_order_relaxed))
            g_read_max_cycles.store(wait, std::memory_order_relaxed);
    }
    if (ide_bus::idle()) last_empty = done;
    if (g_trace_bus.load(std::memory_order_relaxed)) record(TraceKind::Bus, sample);
}

CORE1_RAM void loop1() {
    // Never returns. Each pass does a little bookkeeping, with a bus check
    // (serveBus) before every step.
    static ata::Device dev(DRIVE_INFO);
    uint32_t started = dev.commandsStarted();
    uint32_t completed = dev.commandsCompleted();
    uint32_t command_start_us = 0;
    uint32_t posted_fetch = 0;
    uint32_t wake_start_us = 0;  // 0: not timing a wake
    bool reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
    dev.hardReset(reset_low);
    uint32_t last_empty = m33_hw->dwt_cyccnt;

    for (;;) {
        if (g_read_max_reset.exchange(false, std::memory_order_relaxed))
            g_read_max_cycles.store(0, std::memory_order_relaxed);

        serveBus(dev, last_empty);
        if (dev.commandsStarted() != started) {
            started = dev.commandsStarted();
            command_start_us = time_us_32();
            const ata::CommandInfo& c = dev.lastCommand();
            record(commandStartEvent(command_start_us, c.command, c.lba, c.count));
        }
        if (dev.commandsCompleted() != completed) {
            completed = dev.commandsCompleted();
            uint32_t t = time_us_32();
            record(commandEndEvent(t, dev.lastCommand().command, dev.status(), dev.error(),
                                   t - command_start_us));
        }

        // Sector fetches: post each once; results may complete a DRQ block.
        serveBus(dev, last_empty);
        ata::Fetch fetch;
        if (dev.pendingFetch(fetch) && fetch.id != posted_fetch && g_mailbox.post(fetch))
            posted_fetch = fetch.id;
        serveBus(dev, last_empty);
        FetchResult result;
        if (g_mailbox.takeResult(result)) {
            dev.fetchDone(result.id, result.ok);
            ide_bus::updateIntrq(dev.intrq());
        }
        // Waking from standby: BSY for WAKE_BUSY_US, timed from when it was first seen.
        if (!dev.waking()) {
            wake_start_us = 0;
        } else if (wake_start_us == 0) {
            wake_start_us = time_us_32() | 1;  // never 0 while timing
        } else if (time_us_32() - wake_start_us >= cfg::WAKE_BUSY_US) {
            dev.finishWake();
            ide_bus::updateIntrq(dev.intrq());
        }

        serveBus(dev, last_empty);
        uint32_t pins = pinsAsSample();
        bool low = !bus::high(pins, cfg::PIN_RESET_N);
        if (low != reset_low) {
            reset_low = low;
            dev.hardReset(low);
            if (!low) ide_bus::restart();  // drop anything left from the bus going dead
            ide_bus::updateIntrq(dev.intrq());
            record(low ? TraceKind::ResetAssert : TraceKind::ResetRelease, pins);
        }
    }
}

// Core 0's share of the drive: fetch the sectors core 1 asks for.
static GeneratedDisk g_disk;

// Activity classification for the HDD Synth (activity.h), fed from the
// command events on their way to the trace. Prints an ACT line on each change.
static activity::Classifier g_activity;
static uint32_t g_activity_checked_us = 0;
static bool g_activity_unprinted = false;

static void noteActivity(const TraceEvent& e) {
    uint8_t command = uint8_t(e.extra);
    if (!activity::isMediaCommand(command)) return;
    if (e.kind == TraceKind::CommandStart) {
        bool seek_only = (command >= 0x10 && command <= 0x1F) || (command >= 0x70 && command <= 0x7F);
        g_activity.onCommand(e.time_us, e.pins, seek_only ? 0 : (e.extra >> 8));
    } else if (e.kind == TraceKind::CommandEnd) {
        g_activity.onCommandEnd(e.time_us);
    }
}

static void serveFetches() {
    ata::Fetch fetch;
    while (g_mailbox.takeRequest(fetch)) {
        bool ok = g_disk.read(fetch.lba, fetch.buffer);
        while (!g_mailbox.postResult(FetchResult{fetch.id, ok})) {}
    }
}

#endif

// ---- Core 0 ---------------------------------------------------------------------

static TraceCoalescer g_coalescer;
static uint32_t g_last_event_ms = 0;
static uint32_t g_reported_drops = 0;

// Prints a line only if USB serial can take it now. A blocked write would
// hold up core 0, and in the drive build core 0 must keep serving fetches;
// lines that can't be printed wait in the trace ring (and are counted as
// dropped if it fills).
static bool printLine(const char* line, size_t n) {
    if (Serial && Serial.availableForWrite() < int(n)) return false;
    Serial.write(line, n);
    return true;
}

static void printReady() {
    Serial.printf("READY HDDSynthPro %s slice%d\n", FIRMWARE_VERSION, FIRMWARE_SLICE);
}

// Current level of every bus line: a wiring check. With the adapter powered
// and idle, expect DIOR#, DIOW#, CS0#, CS1#, RESET# and IORDY high, INTRQ and
// DD7 low (host pull-downs), and the other DD lines floating.
static void printPins() {
    uint32_t s = pinsAsSample();
    Serial.printf("PINS DD=%04x DA=%u CS0#=%d CS1#=%d DIOR#=%d DIOW#=%d IORDY=%d INTRQ=%d RESET#=%d\n",
                  (unsigned)bus::data(s), bus::address(s), bus::high(s, cfg::PIN_CS0_N),
                  bus::high(s, cfg::PIN_CS1_N), bus::high(s, cfg::PIN_DIOR_N),
                  bus::high(s, cfg::PIN_DIOW_N), bus::high(s, cfg::PIN_IORDY),
                  bus::high(s, cfg::PIN_INTRQ), bus::high(s, cfg::PIN_RESET_N));
}

static void printStats() {
    Serial.printf("STATS events=%lu dropped=%lu stalls=%lu ring=%u",
                  (unsigned long)g_events.load(), (unsigned long)g_dropped.load(),
                  (unsigned long)g_stalls.load(), (unsigned)g_trace.size());
#if FIRMWARE_SLICE != 0
    // Cycles at clk_sys -> ns. Must stay under ~1070 ns (see setup1).
    uint32_t ns = uint32_t(uint64_t(g_read_max_cycles.load()) * 1000000000ull / clock_get_hz(clk_sys));
    Serial.printf(" read_max_ns=%lu bus_trace=%d seq=%lu rand=%lu activity=%s", (unsigned long)ns,
                  (int)g_trace_bus.load(), (unsigned long)g_activity.sequentialCommands(),
                  (unsigned long)g_activity.randomCommands(), activity::stateName(g_activity.state()));
#endif
    Serial.print("\n");
}

// ---- Solder-bridge test (console `x`, then `Y`) -----------------------------
//
// Pulls each bus pin low in turn, with the others pulled up, to find solder
// bridges (short_test.h). Only with the IDE cable unplugged: it drives the
// bus. Core 1 is paused while it runs, and the board reboots afterwards so
// the normal pin setup comes back.

constexpr uint32_t SHORT_TEST_CONFIRM_MS = 10000;
static uint32_t g_short_test_armed_ms = 0;  // 0 = not armed

static void emitLine(const char* line, void*) { Serial.println(line); }

static void runShortTest() {
    Serial.println("# short test: running, core 1 paused");
    Serial.flush();
    rp2040.idleOtherCore();

    uint32_t observed = short_test::observedMask();
    uint32_t driven = short_test::drivenMask();
    for (unsigned g = 0; g < 32; ++g) {
        if ((observed >> g) & 1) {
            gpio_init(g);                     // SIO input (takes pins back from PIO)
            gpio_set_pulls(g, true, false);   // pull-up only: never pull-downs (E9)
        }
    }
    busy_wait_us(500);
    uint32_t baseline_low = ~gpio_get_all() & observed;

    uint32_t low_when_driven[32] = {};
    for (unsigned g = 0; g < 32; ++g) {
        if (!((driven >> g) & 1) || ((baseline_low >> g) & 1)) continue;  // stuck pins: never driven
        gpio_put(g, 0);
        gpio_set_dir(g, GPIO_OUT);  // 50 us at the default 4 mA drive
        busy_wait_us(50);
        low_when_driven[g] = ~gpio_get_all() & observed;
        gpio_set_dir(g, GPIO_IN);
        busy_wait_us(100);  // pull-ups bring the line back
    }

    short_test::report(short_test::analyse(baseline_low, low_when_driven), emitLine, nullptr);
    Serial.println("# rebooting to restore the bus setup");
    Serial.flush();
    delay(100);
    rp2040.reboot();
}

static void handleConsole() {
    while (Serial.available()) {
        int c = Serial.read();
        if (c != 'x' && c != 'Y' && c != '\r' && c != '\n') g_short_test_armed_ms = 0;
        switch (c) {
            case 'h':
                printReady();
#if FIRMWARE_SLICE == 0
                Serial.println("# h help, p pins, s stats, m mark, x short test (cable unplugged), "
                               "b reboot to bootloader");
#else
                Serial.println("# h help, p pins, s stats, r reset read max, t raw bus trace on/off, "
                               "m mark, x short test (cable unplugged), b reboot to bootloader");
#endif
                break;
            case 'p': printPins(); break;
            case 'm':  // a timestamp, so tests can line phases up with the trace
                Serial.printf("MARK %lu\n", (unsigned long)time_us_32());
                break;
            case 'x':
                Serial.println("# SHORT TEST drives the bus pins. UNPLUG THE IDE CABLE from the BB48 first,");
                Serial.println("# then type Y (capital) within 10 s to run it. Anything else cancels.");
                g_short_test_armed_ms = millis() | 1;
                break;
            case 'Y':
                if (g_short_test_armed_ms && millis() - g_short_test_armed_ms < SHORT_TEST_CONFIRM_MS) {
                    runShortTest();  // reboots
                }
                Serial.println("# short test not armed: type x first");
                break;
            case 's': printStats(); break;
#if FIRMWARE_SLICE != 0
            case 'r': g_read_max_reset.store(true); break;
            case 't': {
                bool on = !g_trace_bus.load();
                g_trace_bus.store(on);
                Serial.printf("# raw bus trace %s\n", on ? "on" : "off");
                break;
            }
#endif
            case 'b':
                Serial.println("# rebooting to bootloader");
                Serial.flush();
                delay(50);
                rp2040.rebootToBootloader();
                break;
            default: break;
        }
    }
}

void setup() {
    Serial.begin(115200);
    printReady();
}

void loop() {
#if FIRMWARE_SLICE != 0
    serveFetches();

    // Re-evaluate the activity state once a millisecond, before anything
    // below can return early on a busy serial link.
    uint32_t now_us = time_us_32();
    if (now_us - g_activity_checked_us >= 1000) {
        g_activity_checked_us = now_us;
        if (g_activity.update(now_us)) g_activity_unprinted = true;
    }
    if (g_activity_unprinted) {
        char line[48];
        size_t n = activity::formatActivity(now_us, g_activity, line, sizeof line);
        if (printLine(line, n)) g_activity_unprinted = false;
    }
#endif
    handleConsole();

    // Print what core 1 recorded; a bounded batch so fetches and the console
    // stay responsive. A run that can't be printed yet is kept and retried.
    static TraceRun pending;
    static bool has_pending = false;
    if (has_pending) {
        char line[96];
        size_t n = formatRun(pending, line, sizeof line);
        if (!printLine(line, n)) return;
        has_pending = false;
    }
    TraceEvent e;
    for (int i = 0; i < 64 && g_trace.pop(e); ++i) {
        g_last_event_ms = millis();
#if FIRMWARE_SLICE != 0
        noteActivity(e);
#endif
        if (g_coalescer.add(e, pending)) {
            char line[96];
            size_t n = formatRun(pending, line, sizeof line);
            if (!printLine(line, n)) {
                has_pending = true;
                return;
            }
        }
#if FIRMWARE_SLICE != 0
        serveFetches();
#endif
    }
    if (millis() - g_last_event_ms >= cfg::TRACE_FLUSH_MS && g_coalescer.flush(pending)) {
        char line[96];
        size_t n = formatRun(pending, line, sizeof line);
        if (!printLine(line, n)) has_pending = true;
    }

    uint32_t drops = g_dropped.load(std::memory_order_relaxed);
    if (drops != g_reported_drops) {
        Serial.printf("DROP %lu\n", (unsigned long)(drops - g_reported_drops));
        g_reported_drops = drops;
    }
}
