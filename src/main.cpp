// HDDSynthPro IDE drive emulator prototype: slice 0 (listen only).
//
// Core 1 drains the PIO listener and watches RESET#, recording every event
// with a timestamp into the trace ring. Core 0 owns USB serial: it prints the
// trace and runs the console. Every bus pin stays an input.
#include <Arduino.h>

#include <atomic>
#include <cstdio>

#include "bus_sample.h"
#include "config.h"
#include "ide_listen.h"
#include "spsc_ring.h"
#include "trace_format.h"
#include "version.h"

static SpscRing<TraceEvent, cfg::TRACE_RING_EVENTS> g_trace;
static std::atomic<uint32_t> g_events{0};   // recorded by core 1
static std::atomic<uint32_t> g_dropped{0};  // lost because the ring was full
static std::atomic<uint32_t> g_stalls{0};   // PIO FIFO filled: accesses missed

// Pins in sample layout (bit n = GPIO PIN_DD0 + n), for non-bus events.
static uint32_t pinsAsSample() { return listen::pinsNow() >> cfg::PIN_DD0; }

// ---- Core 1 ---------------------------------------------------------------

static void record(TraceKind kind, uint32_t pins) {
    g_events.fetch_add(1, std::memory_order_relaxed);
    if (!g_trace.push(TraceEvent{time_us_32(), pins, kind}))
        g_dropped.fetch_add(1, std::memory_order_relaxed);
}

void setup1() { listen::begin(); }

void loop1() {
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

// ---- Core 0 ---------------------------------------------------------------

static TraceCoalescer g_coalescer;
static uint32_t g_last_event_ms = 0;
static uint32_t g_reported_drops = 0;

static void printRun(const TraceRun& run) {
    char line[96];
    size_t n = formatRun(run, line, sizeof line);
    Serial.write(line, n);
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
    Serial.printf("STATS events=%lu dropped=%lu stalls=%lu ring=%u\n",
                  (unsigned long)g_events.load(), (unsigned long)g_dropped.load(),
                  (unsigned long)g_stalls.load(), (unsigned)g_trace.size());
}

static void handleConsole() {
    while (Serial.available()) {
        switch (Serial.read()) {
            case 'h':
                printReady();
                Serial.println("# h help, p pins, s stats, b reboot to bootloader");
                break;
            case 'p': printPins(); break;
            case 's': printStats(); break;
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
    handleConsole();

    // Print what core 1 recorded; a bounded batch so the console stays responsive.
    TraceEvent e;
    TraceRun run;
    for (int i = 0; i < 512 && g_trace.pop(e); ++i) {
        g_last_event_ms = millis();
        if (g_coalescer.add(e, run)) printRun(run);
    }
    if (millis() - g_last_event_ms >= cfg::TRACE_FLUSH_MS && g_coalescer.flush(run)) printRun(run);

    uint32_t drops = g_dropped.load(std::memory_order_relaxed);
    if (drops != g_reported_drops) {
        Serial.printf("DROP %lu\n", (unsigned long)(drops - g_reported_drops));
        g_reported_drops = drops;
    }
}
