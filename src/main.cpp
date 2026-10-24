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
#include <hardware/structs/busctrl.h>

#include "activity.h"
#include "ata_device.h"
#include "block_source.h"
#include "fetch_mailbox.h"
#include "ide_bus.h"
#include "iordy_timer.h"
#include "sd_card.h"
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
static std::atomic<bool> g_trace_bus{false};  // console `t`

// For the activity LED (core 0's updateLed): core 1 counts commands as they
// start and says whether one is running.
static std::atomic<uint32_t> g_command_starts{0};
static std::atomic<bool> g_command_active{false};

// The disk's size, set by core 0 once it has chosen the disk (setup()).
// Core 1 starts first, so it waits for it: IDENTIFY reports the real size.
static std::atomic<uint32_t> g_disk_sectors{0};

void setup1() {
    // Until core 0 has chosen the disk the bus pins stay inputs, so the host
    // sees no drive for that moment (well under a second with an SD card).
    while (g_disk_sectors.load(std::memory_order_acquire) == 0) tight_loop_contents();
    ide_bus::begin();
    iordy_timer::begin(cfg::IORDY_REPORT_NS);  // here, so the two never race to claim a PIO
    // Core 1 wins bus arbitration, so core 0's USB and other traffic can't
    // delay its PIO accesses while the host waits on a read.
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;
}

CORE1_RAM static void recordCommandStart(uint32_t t_us, const ata::CommandInfo& c) {
    record(commandStartEvent(t_us, c.command, c.lba, c.count));
}

CORE1_RAM void loop1() {
    // Never returns. The bus comes first: every access waiting in the FIFO is
    // served back to back, and only when it is empty does core 1 do one small
    // bookkeeping step before looking again. So a read waits for one step at
    // most; iordy_timer.pio measures how long the host actually waits.
    static ata::Device dev(IdentifyInfo{"HDDSYNTH PROTO", "HSP-000001", FIRMWARE_VERSION,
                                        g_disk_sectors.load(std::memory_order_acquire)});
    uint32_t started = dev.commandsStarted();
    uint32_t completed = dev.commandsCompleted();
    uint32_t command_start_us = 0;
    ata::CommandInfo start_info{};  // the command whose CMD line is still to record
    bool start_unrecorded = false;
    uint32_t posted_fetch = 0;
    uint32_t wake_start_us = 0;  // 0: not timing a wake
    bool reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
    dev.hardReset(reset_low);
    unsigned step = 0;

    for (;;) {
        uint32_t sample;
        if (ide_bus::serviceOnce(dev, sample)) {
            if (g_trace_bus.load(std::memory_order_relaxed)) record(TraceKind::Bus, sample);
            continue;
        }
        if (dev.commandQueued()) {  // a Command write: the first job, ahead of the rest
            // The host polls status about 1 us after writing a command, so run
            // it and get back to the bus; its CMD line waits for step 0.
            if (start_unrecorded) {  // rare: two commands with no quiet moment between
                recordCommandStart(command_start_us, start_info);
                start_unrecorded = false;
            }
            dev.runQueuedCommand();
            ide_bus::updateIntrq(dev.intrq());
            if (dev.commandsStarted() != started) {
                started = dev.commandsStarted();
                command_start_us = time_us_32();
                start_info = dev.lastCommand();
                start_unrecorded = true;
                g_command_starts.fetch_add(1, std::memory_order_relaxed);
                g_command_active.store(true, std::memory_order_relaxed);
            }
            continue;
        }

        // Mid-sector the host streams 256 words and nothing else can happen
        // but a reset, so only RESET# is checked between them.
        if (dev.transferring()) step = 4;

        switch (step) {
            case 0:  // the trace's CMD and DONE lines, in that order, at most one per pass
                if (start_unrecorded) {
                    recordCommandStart(command_start_us, start_info);
                    start_unrecorded = false;
                } else if (dev.commandsCompleted() != completed) {
                    completed = dev.commandsCompleted();
                    if (completed == started) g_command_active.store(false, std::memory_order_relaxed);
                    uint32_t t = time_us_32();
                    record(commandEndEvent(t, dev.lastCommand().command, dev.status(), dev.error(),
                                           t - command_start_us));
                }
                break;
            case 1: {  // a sector fetch, posted once
                ata::Fetch fetch;
                if (dev.pendingFetch(fetch) && fetch.id != posted_fetch && g_mailbox.post(fetch))
                    posted_fetch = fetch.id;
                break;
            }
            case 2: {  // a fetch result, which may complete a DRQ block
                FetchResult result;
                if (g_mailbox.takeResult(result)) {
                    dev.fetchDone(result.id, result.ok);
                    ide_bus::updateIntrq(dev.intrq());
                }
                break;
            }
            case 3:  // waking from standby: BSY for WAKE_BUSY_US, timed from when it was first seen
                if (!dev.waking()) {
                    wake_start_us = 0;
                } else if (wake_start_us == 0) {
                    wake_start_us = time_us_32() | 1;  // never 0 while timing
                } else if (time_us_32() - wake_start_us >= cfg::WAKE_BUSY_US) {
                    dev.finishWake();
                    ide_bus::updateIntrq(dev.intrq());
                }
                break;
            default: {  // RESET#
                uint32_t pins = pinsAsSample();
                bool low = !bus::high(pins, cfg::PIN_RESET_N);
                if (low != reset_low) {
                    reset_low = low;
                    dev.hardReset(low);
                    if (!low) {
                        ide_bus::restart();  // drop anything left from the bus going dead
                        // Time only real traffic: while the host powers up, its
                        // strobes float low, which looks like one long read.
                        iordy_timer::requestReset();
                    }
                    ide_bus::updateIntrq(dev.intrq());
                    record(low ? TraceKind::ResetAssert : TraceKind::ResetRelease, pins);
                }
                break;
            }
        }
        step = step == 4 ? 0 : step + 1;
    }
}

// Core 0's share of the drive: move the sectors core 1 asks for. The disk is
// the generated one, with writes kept in a RAM overlay on top.
// The disk is HDDSYNTH.IMG on the SD card if it is there (slice 5), else the
// generated one with writes kept in a RAM overlay on top.
static GeneratedDisk g_generated;
static RamOverlayDisk<cfg::RAM_DISK_SECTORS> g_overlay(g_generated);
static BlockSource* g_disk = &g_overlay;
static const char* g_disk_name = "generated";

// The BB48's LED as the drive's activity light, like a hard disk's: lit while
// a command runs, and for at least LED_MIN_MS after any command starts.
static void updateLed() {
    static uint32_t seen = 0;
    static uint32_t off_ms = 0;
    uint32_t starts = g_command_starts.load(std::memory_order_relaxed);
    uint32_t now = millis();
    if (starts != seen || g_command_active.load(std::memory_order_relaxed)) {
        seen = starts;
        off_ms = now + cfg::LED_MIN_MS;
    }
    gpio_put(cfg::PIN_BOARD_LED, int32_t(off_ms - now) > 0);
}

static void chooseDisk() {
    if (ImageFile* file = sd_card::openImage(Serial)) {
        static ImageDisk image(*file);
        if (image.sectors() > 0) {
            g_disk = &image;
            g_disk_name = "image";
        }
    }
    g_disk_sectors.store(g_disk->sectors(), std::memory_order_release);
}

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
        bool ok = fetch.store ? g_disk->write(fetch.lba, fetch.buffer) : g_disk->read(fetch.lba, fetch.buffer);
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
    iordy_timer::poll();
    Serial.printf(" iordy_slow=%lu iordy_max_ns=%lu iordy_max_reg=%s iordy_max_at_us=%lu",
                  (unsigned long)iordy_timer::slowReads(), (unsigned long)iordy_timer::maxNs(),
                  iordy_timer::maxRegister(), (unsigned long)iordy_timer::maxAtUs());
    Serial.printf(" bus_contested=%lu bus_stalls=%lu", (unsigned long)iordy_timer::contestedAccesses(),
                  (unsigned long)iordy_timer::stallCycles());
    Serial.printf(" disk=%s sectors=%lu", g_disk_name, (unsigned long)g_disk->sectors());
    Serial.printf(" bus_trace=%d seq=%lu rand=%lu activity=%s",
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
                Serial.println("# h help, p pins, s stats, r reset read timings, t raw bus trace on/off, "
                               "m mark, S SD card check, I create SD image, x short test (cable unplugged), "
                               "b reboot to bootloader");
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
            case 'r': iordy_timer::reset(); break;
            case 't': {
                bool on = !g_trace_bus.load();
                g_trace_bus.store(on);
                Serial.printf("# raw bus trace %s\n", on ? "on" : "off");
                break;
            }
#endif
#if FIRMWARE_SLICE != 0
            case 'S': sd_card::printInfo(Serial); break;
            case 'I': sd_card::createImage(gen_disk::DEFAULT_SECTORS, Serial); break;
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
#if FIRMWARE_SLICE != 0
    gpio_init(cfg::PIN_BOARD_LED);
    gpio_set_dir(cfg::PIN_BOARD_LED, true);
    chooseDisk();
#endif
    printReady();
}

void loop() {
#if FIRMWARE_SLICE != 0
    serveFetches();
    iordy_timer::poll();
    updateLed();

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
