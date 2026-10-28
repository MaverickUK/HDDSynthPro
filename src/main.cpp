// HDDSynthPro IDE drive emulator prototype.
//
// Two builds (FIRMWARE_SLICE, set in platformio.ini):
//   bb48        slice 0, listen only: core 1 drains the PIO listener and
//               records every bus access. Every pin stays an input.
//   bb48_drive  slices 1-3, the drive: core 1 serves the bus (ide_bus) and
//               runs the ATA device; core 0 fetches sectors for it from
//               the SD card's image, the welcome disk or the test disk.
//               Commands are traced; raw accesses on request (console `t`).
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
#include <hardware/structs/busctrl.h>
#include <hardware/uart.h>

#include "activity.h"
#include "activity_link.h"
#include "drive_config.h"
#include "ata_device.h"
#include "block_source.h"
#include "bus_observer.h"
#include <hardware/watchdog.h>
#include "fetch_mailbox.h"
#include "ide_bus.h"
#include "ide_dma.h"
#include "iordy_timer.h"
#include "sd_card.h"
#include "welcome_disk.h"
#include "welcome_text.h"
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

// The capture window (console `w`, cfg::LISTEN_WINDOW_EVENTS): off, every
// access is recorded; on, only a window after each RESET# assert.
static std::atomic<uint32_t> g_window{0};
static std::atomic<uint32_t> g_skipped{0};  // accesses outside the window

void setup1() { listen::begin(); }

CORE1_RAM void loop1() {
    // Never returns: one tight loop, so nothing slow ever sits between a
    // FIFO word arriving and it being timestamped.
    bool reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
    if (reset_low) record(TraceKind::ResetAssert, pinsAsSample());
    bool window = false;
    uint32_t window_left = 0;  // accesses still to record in this window
    for (;;) {
        uint32_t sample;
        if (listen::pop(sample)) {
            if (!window) {
                record(TraceKind::Bus, sample);
            } else if (window_left) {
                --window_left;
                record(TraceKind::Bus, sample);
            } else {
                g_skipped.fetch_add(1, std::memory_order_relaxed);
            }
        }

        uint32_t pins = pinsAsSample();
        bool low = !bus::high(pins, cfg::PIN_RESET_N);
        if (low != reset_low) {
            reset_low = low;
            record(low ? TraceKind::ResetAssert : TraceKind::ResetRelease, pins);  // always recorded
            if (low) window_left = cfg::LISTEN_WINDOW_EVENTS;
        }
        bool want_window = g_window.load(std::memory_order_relaxed) != 0;
        if (want_window != window) {
            window = want_window;
            window_left = 0;  // turned on: closed until the next RESET#
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
static SharedFlag g_trace_bus{false};  // console `t`
static SharedFlag g_fast_lane{true};   // console `F` (debug)
static SharedFlag g_read_ahead{true};  // console `A` (debug)
// Off by default: with it on, writes hold some status reads 1.5-2 us (ATA
// allows 1.25), not yet understood. Console `W` turns it on to work on that.
static SharedFlag g_write_behind{false};  // console `W`

// For the activity LED (core 0's updateLed): core 1 counts commands as they
// start and says whether one is running.
static std::atomic<uint32_t> g_command_starts{0};
static SharedFlag g_command_active{false};

// The disk's size, set by core 0 once it has chosen the disk (setup()).
// Core 1 starts first, so it waits for it: IDENTIFY reports the real size.
// The PIO mode to offer (HDDSYNTH.INI's `mode`) is set before it.
static std::atomic<uint32_t> g_disk_sectors{0};
static std::atomic<uint8_t> g_pio_mode{0};
static std::atomic<uint8_t> g_mdma_mode{NO_DMA};

// What IDENTIFY says: the device's, and the bus observer's to check it by.
static IdentifyInfo driveIdentity() {
    return IdentifyInfo{"HDDSYNTH PROTO", "HSP-000001", FIRMWARE_VERSION,
                        g_disk_sectors.load(std::memory_order_acquire),
                        g_pio_mode.load(std::memory_order_relaxed),
                        g_mdma_mode.load(std::memory_order_relaxed)};
}

void setup1() {
    // Until core 0 has chosen the disk the bus pins stay inputs, so the host
    // sees no drive for that moment (well under a second with an SD card).
    while (g_disk_sectors.load(std::memory_order_acquire) == 0) tight_loop_contents();
    ide_bus::begin();
    iordy_timer::begin(cfg::IORDY_REPORT_NS);  // here, so these never race to claim a PIO
    bus_observer::begin(g_disk_sectors.load(std::memory_order_relaxed), driveIdentity());
    ide_dma::begin();  // after ide_bus: its block's function on DD0-15 is the one to hand back to
    // Core 1 wins bus arbitration, so core 0's USB and other traffic can't
    // delay its PIO accesses while the host waits on a read.
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;
}

// The DMA command's data phase (READ DMA, WRITE DMA): one bookkeeping step.
// A burst that ended goes back to the device; a new one starts when the
// device has a sector (or buffer) for it; and when the command is over, done
// or cut short (RESET#, SRST, a new command, an error), ide_dma tidies up.
CORE1_RAM static void serviceDma(ata::Device& dev) {
    if (ide_dma::burstRunning()) {
        if (!ide_dma::burstDone()) {
            if (!dev.dmaCommand()) ide_dma::endCommand();  // cut short mid-burst
            return;
        }
        dev.dmaBurstDone();
        ide_bus::updateIntrq(dev.intrq());
    }
    if (!dev.dmaCommand()) {
        ide_dma::endCommand();  // nothing to do unless one ran
        return;
    }
    ata::DmaBurst burst;
    if (dev.dmaBurstReady(burst)) ide_dma::startBurst(burst);
}

// Posts the device's fetch to core 0, once per fetch.
CORE1_RAM static void postFetch(ata::Device& dev, uint32_t& posted_fetch) {
    uint32_t id = dev.pendingFetchId();
    if (id == 0 || id == posted_fetch) return;  // the common case, without a call
    ata::Fetch fetch;
    if (dev.pendingFetch(fetch) && g_mailbox.post(fetch)) posted_fetch = fetch.id;
}

CORE1_RAM static void recordCommandStart(uint32_t t_us, const ata::CommandInfo& c) {
    record(commandStartEvent(t_us, c.command, c.lba, c.count));
}

// Console `V`: the drive goes silent, as if unplugged, while core 0 holds
// g_absent. The adapter's next command fails, it resets and probes, and the
// drive is detected again once it's back: a replug without hands on the rig.
// It comes back as from a power cycle, with the device reset. Runs once per
// absence, never per access, so it can stay in flash (one of
// check_core1_ram.py's one-offs).
static SharedFlag g_absent{false};

__attribute__((noinline)) static void stayAbsent(ata::Device& dev) {
    dev.hardReset(true);
    ide_bus::stop();
    ide_dma::endCommand();
    while (g_absent.load()) tight_loop_contents();
    dev.hardReset(false);
    ide_bus::restart();
    iordy_timer::requestReset();
    ide_bus::updateIntrq(dev.intrq());
    g_command_active.store(false);
}

CORE1_RAM void loop1() {
    // Never returns. The bus comes first: every access waiting in the FIFO is
    // served back to back, and only when it is empty does core 1 do one small
    // bookkeeping step before looking again. So a read waits for one step at
    // most; iordy_timer.pio measures how long the host actually waits.
    static ata::Device dev(driveIdentity());
    uint32_t started = dev.commandsStarted();
    uint32_t completed = dev.commandsCompleted();
    uint32_t command_start_us = 0;
    ata::CommandInfo start_info{};  // the command whose CMD line is still to record
    bool start_unrecorded = false;
    bool quiet = false;  // a command just started: bookkeeping waits (COMMAND_QUIET_US)
    uint32_t posted_fetch = 0;
    uint32_t wake_start_us = 0;  // 0: not timing a wake
    uint32_t hold_start_us = 0;  // 0: not timing a data-out hold
    bool reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
    dev.hardReset(reset_low);
    unsigned step = 0;

    for (;;) {
        uint32_t sample;
        if (ide_bus::serviceOnce(dev, sample)) {
            if (g_trace_bus.load()) record(TraceKind::Bus, sample);
            continue;
        }
        // Mid-block, the fast lane answers data reads with nothing else in
        // between. Not while the raw trace or the debug delay is on.
        const uint16_t* next;
        uint16_t left;
        if (dev.dataInRemaining(next, left)) {
            if (g_fast_lane.load() && !g_trace_bus.load() && !ide_bus::dataReadDelayed()) {
                if (ide_bus::serveDataIn(dev, reset_low, g_mailbox, posted_fetch)) continue;
            } else {
                postFetch(dev, posted_fetch);  // the read-ahead, without the lane
            }
        }
        if (dev.commandQueued()) {  // a Command write: the first job, ahead of the rest
            // The host polls status about 1 us after writing a command, so run
            // it and get back to the bus; its CMD line waits for step 0.
            if (start_unrecorded) {  // rare: two commands with no quiet moment between
                recordCommandStart(command_start_us, start_info);
                start_unrecorded = false;
            }
            dev.setReadAhead(g_read_ahead.load());
            dev.setWriteBehind(g_write_behind.load());
            dev.runQueuedCommand();
            ide_bus::updateIntrq(dev.intrq());
            // The host may already be polling (the adapter reads STATUS at
            // once after IDENTIFY): answer it before the bookkeeping below.
            while (ide_bus::serviceOnce(dev, sample)) {
                if (g_trace_bus.load()) record(TraceKind::Bus, sample);
            }
            if (dev.commandsStarted() != started) {
                started = dev.commandsStarted();
                command_start_us = time_us_32();
                start_info = dev.lastCommand();
                start_unrecorded = true;
                g_command_starts.fetch_add(1, std::memory_order_relaxed);
                g_command_active.store(true);
                quiet = true;
            }
            continue;
        }
        // For a moment after a command starts, only the bus: the host's first
        // status poll comes within a few microseconds and must find core 1
        // free, not in the middle of a step (CMD line, fetch post).
        if (quiet) {
            if (time_us_32() - command_start_us < cfg::COMMAND_QUIET_US) continue;
            quiet = false;
        }

        // Mid-sector the host streams 256 words and nothing else can happen
        // but a reset, so only RESET# is checked between them, plus the
        // write-behind's store: posted, and its result passed back, while the
        // host sends the next sector.
        if (dev.transferring()) {
            postFetch(dev, posted_fetch);  // the write-behind's store, while the host sends the next sector
            step = 4;
        }

        switch (step) {
            case 0:  // the trace's CMD and DONE lines, in that order, at most one per pass
                if (start_unrecorded) {
                    recordCommandStart(command_start_us, start_info);
                    start_unrecorded = false;
                } else if (dev.commandsCompleted() != completed) {
                    completed = dev.commandsCompleted();
                    if (completed == started) g_command_active.store(false);
                    uint32_t t = time_us_32();
                    record(commandEndEvent(t, dev.lastCommand().command, dev.status(), dev.error(),
                                           t - command_start_us));
                }
                break;
            case 1:  // a sector fetch, posted once; DMA bursts
                postFetch(dev, posted_fetch);
                serviceDma(dev);
                break;
            case 2: {  // a fetch result, which may complete a DRQ block
                FetchResult result;
                if (g_mailbox.takeResult(result)) {
                    dev.fetchDone(result.id, result.ok);
                    ide_bus::updateIntrq(dev.intrq());
                }
                break;
            }
            case 3:  // BSY for a while: waking from standby, or after a data-out block
                if (!dev.dataOutHeld()) {
                    hold_start_us = 0;
                } else if (hold_start_us == 0) {
                    hold_start_us = time_us_32() | 1;
                } else if (time_us_32() - hold_start_us >= cfg::DATA_OUT_BSY_US) {
                    dev.releaseDataOut();
                    ide_bus::updateIntrq(dev.intrq());
                }
                if (!dev.waking()) {
                    wake_start_us = 0;
                } else if (wake_start_us == 0) {
                    wake_start_us = time_us_32() | 1;  // never 0 while timing
                } else if (time_us_32() - wake_start_us >= cfg::WAKE_BUSY_US) {
                    dev.finishWake();
                    ide_bus::updateIntrq(dev.intrq());
                }
                break;
            default: {  // RESET#, and console `V`
                if (g_absent.load()) {
                    stayAbsent(dev);
                    reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
                    if (reset_low) dev.hardReset(true);  // back in the middle of a reset
                    break;
                }
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
// an image on the SD card (slice 5) if there is one to serve (chooseDisk()),
// else the welcome disk, or the generated test disk with `image = none` or
// console `T`. Both keep writes in the one RAM overlay, until a restart.
static const welcome_disk::File WELCOME_FILES[] = {
    {"README.TXT", welcome_text::README, welcome_text::README_SIZE},
    {drive_config::FILE_NAME, drive_config::defaultFile(), uint32_t(strlen(drive_config::defaultFile()))},
};
static WelcomeDisk g_welcome(WELCOME_FILES, 2, welcome_text::FAT_DATE);
static GeneratedDisk g_generated;
static RamOverlayDisk<cfg::RAM_DISK_SECTORS> g_overlay(g_welcome);
static BlockSource* g_disk = &g_overlay;
static const char* g_disk_name = "welcome";
static bool g_test_disk = false;  // the generated disk: the bus observer can check its data

// The BB48's LED as the drive's activity light, like a hard disk's: lit while
// a command runs, and for at least LED_MIN_MS after any command starts. The
// activity link sends the same busy flag.
static bool g_busy = false;

static void updateLed() {
    static uint32_t seen = 0;
    static uint32_t off_ms = 0;
    uint32_t starts = g_command_starts.load(std::memory_order_relaxed);
    uint32_t now = millis();
    if (starts != seen || g_command_active.load()) {
        seen = starts;
        off_ms = now + cfg::LED_MIN_MS;
    }
    g_busy = int32_t(off_ms - now) > 0;
    gpio_put(cfg::PIN_BOARD_LED, g_busy);
}

static uint32_t g_disk_ms = 0;  // how long choosing the disk took at boot (console `s`)

// Console `J`: serve HDDBENCH.IMG after the restart it triggers. Kept in a
// watchdog scratch register, which a restart keeps and a power cycle clears,
// so the drive is always back on HDDSYNTH.IMG after power-off.
static constexpr uint32_t BENCH_MAGIC = 0x48444442;  // "HDDB"
static bool benchRequested() { return watchdog_hw->scratch[0] == BENCH_MAGIC; }
// Console `T`: serve the test disk after the restart it triggers, card or no
// card, in the same register (so `T` and `J` undo each other).
static constexpr uint32_t TEST_DISK_MAGIC = 0x48444454;  // "HDDT"
static bool testDiskRequested() { return watchdog_hw->scratch[0] == TEST_DISK_MAGIC; }

// The generated test disk instead of the welcome disk, under the RAM overlay.
static void useTestDisk() {
    g_overlay.setBase(g_generated);
    g_disk_name = "generated";
    g_test_disk = true;
}

// HDDSYNTH.INI (drive_config.h) as read at boot, and what came of it. Boot
// messages go out before USB serial is connected, so console `S` shows them.
static drive_config::Config g_config;
static const char* g_config_state = "none";  // "none", "present" or "truncated"
static const char* g_image_name = "-";        // the image being served
static char g_image_note[96] = "";            // why the configured image isn't

// Opens image `name` as the disk. False, with sd_card::openError() or
// "empty" as the reason in `why`, if it can't be used.
static bool useImage(const char* name, const char*& why) {
    ImageFile* file = sd_card::openImage(name, Serial);
    if (!file) {
        why = sd_card::openError();
        return false;
    }
    static ImageDisk image(*file);  // always the same file object, reopened
    if (image.sectors() == 0) {
        why = "empty";
        return false;
    }
    g_disk = &image;
    g_image_name = name;
    return true;
}

// The disk: the test disk after console `T`, or HDDBENCH.IMG after `J`; else
// the image HDDSYNTH.INI names (HDDSYNTH.IMG by default), or the test disk
// for `image = none`; else HDDSYNTH.IMG; else the welcome disk.
static void chooseDisk() {
    uint32_t start = millis();
    drive_config::defaults(g_config);
    static char text[drive_config::MAX_FILE];
    size_t len = 0;
    bool truncated = false;
    if (sd_card::readConfig(text, sizeof text, len, truncated, Serial)) {
        drive_config::parse(text, len, g_config);
        g_config_state = truncated ? "truncated" : "present";
    }
    const char* why = "";
    if (testDiskRequested()) {
        useTestDisk();  // the card, if any, left alone
    } else if (!sd_card::mounted()) {
        // no card: the welcome disk, without waiting for the card again
    } else if (benchRequested()) {
        if (useImage(sd_card::BENCH_IMAGE_NAME, why)) g_disk_name = "bench";
    } else if (!drive_config::servesImage(g_config)) {
        useTestDisk();  // image = none: the card left alone
    } else if (useImage(g_config.image, why)) {
        g_disk_name = "image";
    } else {
        std::snprintf(g_image_note, sizeof g_image_note, "%s: %s", g_config.image, why);
        if (strcasecmp(g_config.image, drive_config::DEFAULT_IMAGE) != 0 &&
            useImage(drive_config::DEFAULT_IMAGE, why))
            g_disk_name = "image";
    }
    g_disk_ms = millis() - start;
    g_pio_mode.store(drive_config::pioModeOffered(g_config.mode), std::memory_order_relaxed);
    g_mdma_mode.store(drive_config::mdmaModeOffered(g_config.mode), std::memory_order_relaxed);
    g_disk_sectors.store(g_disk->sectors(), std::memory_order_release);  // after the mode: core 1 reads both
}

// Console `E`: sets one setting in HDDSYNTH.INI from "key = value", keeping
// the rest of the file and its comments (the defaults if there is no file).
static void editConfig(const char* line) {
    const char* eq = strchr(line, '=');
    char key[16] = "", value[drive_config::MAX_NAME + 1] = "";
    if (eq) {
        std::sscanf(line, " %15[^= \t]", key);
        std::sscanf(eq + 1, " %64[^\r\n]", value);
        for (char* p = key; *p; ++p) *p = char(tolower(*p));
        for (size_t n = strlen(value); n && (value[n - 1] == ' ' || value[n - 1] == '\t'); --n) value[n - 1] = '\0';
    }
    if (!key[0] || !value[0]) {
        Serial.println("# E: nothing changed; type key = value, e.g. image = GAMES.IMG");
        return;
    }
    static char text[drive_config::MAX_FILE], edited[drive_config::MAX_FILE];
    size_t len = 0;
    bool truncated = false;
    if (!sd_card::readConfig(text, sizeof text, len, truncated, Serial)) {
        if (!sd_card::mounted()) return;  // no card: readConfig said so
        len = strlen(drive_config::defaultFile());  // no file yet: start from the defaults
        memcpy(text, drive_config::defaultFile(), len);
    } else if (truncated) {
        Serial.printf("# E: %s is longer than %u bytes: edit it on a computer\n", drive_config::FILE_NAME,
                      unsigned(drive_config::MAX_FILE));
        return;
    }
    size_t n = drive_config::setKey(text, len, key, value, edited, sizeof edited);
    if (n == 0) {
        Serial.printf("# E: '%s' isn't a setting (image or mode), or the file would be too long\n", key);
        return;
    }
    if (!sd_card::writeConfig(edited, n, Serial)) return;
    drive_config::Config check;
    drive_config::parse(edited, n, check);
    Serial.printf("# saved %s = %s in %s; restart (!) to use it\n", key, value, drive_config::FILE_NAME);
    size_t shown = check.warning_count < drive_config::MAX_WARNINGS ? check.warning_count : drive_config::MAX_WARNINGS;
    for (size_t i = 0; i < shown; ++i) Serial.printf("CONFIG warning: %s\n", check.warnings[i]);
}

// Console `S`: the settings and what came of them, after the card's own line.
static void printConfig() {
    drive_config::Mode used = drive_config::modeSupported(g_config.mode) ? g_config.mode : drive_config::Mode::Auto;
    uint8_t mdma = g_mdma_mode.load();
    char mdma_text[8] = "none";
    if (mdma != NO_DMA) std::snprintf(mdma_text, sizeof mdma_text, "%u", unsigned(mdma));
    Serial.printf("CONFIG file=%s image=%s mode=%s mode_used=%s pio_offered=%u mdma_offered=%s serving=%s\n",
                  g_config_state,
                  g_config.image, drive_config::modeName(g_config.mode), drive_config::modeName(used),
                  unsigned(g_pio_mode.load()), mdma_text, g_image_name);
    if (g_image_note[0]) Serial.printf("CONFIG image not used: %s\n", g_image_note);
    size_t shown = g_config.warning_count < drive_config::MAX_WARNINGS ? g_config.warning_count : drive_config::MAX_WARNINGS;
    for (size_t i = 0; i < shown; ++i) Serial.printf("CONFIG warning: %s\n", g_config.warnings[i]);
    if (g_config.warning_count > shown)
        Serial.printf("CONFIG warning: and %u more\n", unsigned(g_config.warning_count - shown));
    if (strcmp(g_config_state, "truncated") == 0)
        Serial.printf("CONFIG warning: only the first %u bytes of the file were read\n", unsigned(drive_config::MAX_FILE));
}

// Activity classification for the HDD Synth (activity.h), fed from the
// command events on their way to the trace. Prints an ACT line on each change.
static activity::Classifier g_activity;
static uint32_t g_activity_checked_us = 0;
static bool g_activity_unprinted = false;

// The activity link to an HDD Synth (activity_link.h), on UART0, transmit
// only. Core 0 must never wait on it, so a sentence (at most 20 bytes) is
// written only when the 32-byte TX FIFO is empty; otherwise it stays due
// and goes a millisecond later, with whatever is current then.
static activity_link::Sender g_link(cfg::LINK_HEARTBEAT_MS);
static activity_link::Op g_link_op = activity_link::Op::None;  // the last media command's
static bool g_link_on = true;  // console `L`

static void beginLink() {
    uart_init(uart0, cfg::LINK_BAUD);  // 8N1
    gpio_set_function(cfg::PIN_LINK_TX, GPIO_FUNC_UART);  // GPIO 1 (RX) stays untouched
}

static void serviceLink() {
    if (!g_link_on) return;
    activity_link::Fields f;
    f.busy = g_busy;
    f.state = g_activity.state();
    f.op = g_link_op;
    uint32_t now = millis();
    if (!g_link.due(f, now)) return;
    if (!(uart_get_hw(uart0)->fr & UART_UARTFR_TXFE_BITS)) return;  // not empty yet
    char line[activity_link::SENTENCE_MAX];
    size_t n = activity_link::formatSentence(f, line, sizeof line);
    if (n == 0) return;
    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(line), n);  // fits: never waits
    g_link.sent(f, now);
}

// The loopback test (console `U`): with a jumper from GPIO 0 (TX) to GPIO 1
// (RX), read back what the link sends for a second and check it. GPIO 1 is
// a UART input only while the test runs, with a pull-up so that without the
// jumper it reads idle instead of noise; afterwards it goes back as it was.
// Polled on every loop pass, so core 0 never waits on it and the 32-byte RX
// FIFO (2.8 ms at 115200 baud) can't overflow.
constexpr uint32_t LOOPBACK_MS = 1000;  // five heartbeats
static struct {
    enum class Phase : uint8_t { Off, Starting, Listening, Finishing } phase = Phase::Off;
    uint32_t until_ms = 0;
    uint32_t sent_before = 0;
    uint32_t bytes = 0;
    uint32_t errors = 0;  // framing, parity, break or overrun
    activity_link::Receiver receiver;
    activity_link::Fields last;
    // GPIO 1 as it was before the test.
    gpio_function_t function = GPIO_FUNC_NULL;
    bool pulled_up = false;
    bool pulled_down = false;
} g_loopback;

static void startLoopback() {
    if (g_loopback.phase != decltype(g_loopback)::Phase::Off) return;
    if (!g_link_on) {
        Serial.println("# loopback: the link is off (L turns it on)");
        return;
    }
    g_loopback.function = gpio_get_function(cfg::PIN_LINK_RX);
    g_loopback.pulled_up = gpio_is_pulled_up(cfg::PIN_LINK_RX);
    g_loopback.pulled_down = gpio_is_pulled_down(cfg::PIN_LINK_RX);
    gpio_set_pulls(cfg::PIN_LINK_RX, true, false);
    gpio_set_function(cfg::PIN_LINK_RX, GPIO_FUNC_UART);
    g_loopback.phase = decltype(g_loopback)::Phase::Starting;
    Serial.printf("# loopback: reading GPIO %u for %lu ms (needs a jumper from GPIO %u)\n",
                  unsigned(cfg::PIN_LINK_RX), (unsigned long)LOOPBACK_MS, unsigned(cfg::PIN_LINK_TX));
}

// Reads what has arrived, without waiting.
static void drainLoopback() {
    uart_hw_t* hw = uart_get_hw(uart0);
    while (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
        uint32_t dr = hw->dr;
        ++g_loopback.bytes;
        if (dr & (UART_UARTDR_FE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_OE_BITS)) {
            ++g_loopback.errors;
            continue;
        }
        activity_link::Fields f;
        if (g_loopback.receiver.feed(char(dr & 0xFF), f)) g_loopback.last = f;
    }
}

static void serviceLoopback() {
    using Phase = decltype(g_loopback)::Phase;
    uart_hw_t* hw = uart_get_hw(uart0);
    bool sending = hw->fr & UART_UARTFR_BUSY_BITS;  // a sentence still on the wire
    uint32_t now = millis();
    switch (g_loopback.phase) {
        case Phase::Off: return;
        case Phase::Starting:
            // Between sentences, so listening starts at the beginning of one.
            // Resetting the UART then (nothing is being sent) clears what it
            // received before GPIO 1 was its input: breaks from an unconnected
            // input fill the FIFO, and the overrun that follows marks the next
            // character even after the FIFO is drained and the errors cleared.
            if (sending) return;
            uart_init(uart0, cfg::LINK_BAUD);
            g_loopback.receiver.reset();
            g_loopback.bytes = g_loopback.errors = 0;
            g_loopback.sent_before = g_link.count();
            g_loopback.until_ms = now + LOOPBACK_MS;
            g_loopback.phase = Phase::Listening;
            return;
        case Phase::Listening:
            drainLoopback();
            if (int32_t(now - g_loopback.until_ms) >= 0) g_loopback.phase = Phase::Finishing;
            return;
        case Phase::Finishing:
            if (sending) return;  // let the last sentence finish arriving
            drainLoopback();
            break;
    }

    uint32_t sent = g_link.count() - g_loopback.sent_before;
    uint32_t valid = g_loopback.receiver.valid();
    uint32_t rejected = g_loopback.receiver.rejected();
    bool last_matches = valid && g_loopback.last == g_link.last();
    const char* result = g_loopback.bytes == 0                                         ? "no-data"
                         : (valid == sent && !rejected && !g_loopback.errors && last_matches) ? "pass"
                                                                                       : "fail";
    char last[activity_link::SENTENCE_MAX] = "-";
    if (valid) {
        size_t n = activity_link::formatSentence(g_loopback.last, last, sizeof last);
        if (n >= 2) last[n - 2] = '\0';  // without the \r\n
    }
    Serial.printf("LOOPBACK %s sent=%lu valid=%lu rejected=%lu errors=%lu bytes=%lu last=%s\n", result,
                  (unsigned long)sent, (unsigned long)valid, (unsigned long)rejected,
                  (unsigned long)g_loopback.errors, (unsigned long)g_loopback.bytes, last);
    if (g_loopback.bytes == 0)
        Serial.printf("# loopback: nothing arrived; is GPIO %u jumpered to GPIO %u?\n", unsigned(cfg::PIN_LINK_TX),
                      unsigned(cfg::PIN_LINK_RX));

    gpio_set_function(cfg::PIN_LINK_RX, g_loopback.function);
    gpio_set_pulls(cfg::PIN_LINK_RX, g_loopback.pulled_up, g_loopback.pulled_down);
    g_loopback.phase = Phase::Off;
}

// Console `V`: the drive goes silent (core 1's stayAbsent) until the adapter,
// having failed a command, asserts RESET# to look for a drive. Core 0 watches
// RESET# itself (core 1 records nothing meanwhile) and brings the drive back
// at that edge, so it's present before the reset ends, as a drive powering up
// with the adapter would be. Coming back later, after the adapter had started
// probing an empty bus, once left it with a garbled IDENTIFY. A second `V`, or
// ABSENT_MAX_MS, also brings it back.
constexpr uint32_t ABSENT_MAX_MS = 120000;
static uint32_t g_absent_since_ms = 0;  // while g_absent
static uint32_t g_absent_resets = 0;
static bool g_absent_reset_low = false;

static void setAbsent(bool on, const char* why) {
    if (on == g_absent.load()) return;
    if (on) {
        g_absent_since_ms = millis();
        g_absent_resets = 0;
        g_absent_reset_low = !gpio_get(cfg::PIN_RESET_N);
        g_absent.store(true);
        Serial.println("ABSENT on");
    } else {
        g_absent.store(false);
        Serial.printf("ABSENT off resets=%lu ms=%lu%s\n", (unsigned long)g_absent_resets,
                      (unsigned long)(millis() - g_absent_since_ms), why);
    }
}

static void serviceAbsence() {
    if (!g_absent.load()) return;
    bool low = !gpio_get(cfg::PIN_RESET_N);
    if (low != g_absent_reset_low) {
        g_absent_reset_low = low;
        if (low) {
            ++g_absent_resets;
            setAbsent(false, " at RESET#");
            return;
        }
    }
    if (millis() - g_absent_since_ms >= ABSENT_MAX_MS) setAbsent(false, " timeout");
}

// The last media command seen, for the STUCK line (checkStuck).
static struct {
    uint8_t command = 0;
    uint32_t lba = 0, count = 0;
} g_last_command;

// On an SD image the bus observer checks IDENTIFY only around detection,
// which comes about 2.6 s after RESET# is released: for this long after each
// release, and after boot (bus_observer::poll says why).
constexpr uint32_t IDENTIFY_CHECK_MS = 10000;
static uint32_t g_identify_check_until_ms = 30000;

static void noteActivity(const TraceEvent& e) {
    if (e.kind == TraceKind::ResetRelease) g_identify_check_until_ms = millis() + IDENTIFY_CHECK_MS;
    uint8_t command = uint8_t(e.extra);
    if (!activity::isMediaCommand(command)) return;
    if (e.kind == TraceKind::CommandStart) {
        g_last_command.command = command;
        g_last_command.lba = e.pins;
        g_last_command.count = e.extra >> 8;
        activity_link::Op op = activity_link::opOf(command);
        if (op != activity_link::Op::None) g_link_op = op;
        bool seek_only = (command >= 0x10 && command <= 0x1F) || (command >= 0x70 && command <= 0x7F);
        g_activity.onCommand(e.time_us, e.pins, seek_only ? 0 : (e.extra >> 8));
    } else if (e.kind == TraceKind::CommandEnd) {
        g_activity.onCommandEnd(e.time_us);
    }
}

// The last fetch served, for the STUCK line.
static struct {
    uint32_t count = 0, id = 0, lba = 0, at_ms = 0;
    bool store = false, ok = false;
} g_last_fetch;

static void serveFetches() {
    ata::Fetch fetch;
    while (g_mailbox.takeRequest(fetch)) {
        bool ok = fetch.store ? g_disk->write(fetch.lba, fetch.buffer)
                  : fetch.flush ? true  // FLUSH CACHE: nothing to move
                                : g_disk->read(fetch.lba, fetch.buffer);
        if (ok && fetch.flush) ok = g_disk->flush();
        while (!g_mailbox.postResult(FetchResult{fetch.id, ok})) {}
        g_last_fetch.count++;
        g_last_fetch.id = fetch.id;
        g_last_fetch.lba = fetch.lba;
        g_last_fetch.store = fetch.store;
        g_last_fetch.ok = ok;
        g_last_fetch.at_ms = millis();
    }
}

// A command still running after STUCK_MS: one STUCK line, saying whether core
// 0 ever served its fetch and whether the result is still waiting for core 1.
// From core 0's side only, so core 1 is untouched.
constexpr uint32_t STUCK_MS = 3000;

static void checkStuck() {
    static uint32_t starts = 0;  // the command being timed, by g_command_starts
    static uint32_t since_ms = 0;
    static bool reported = false;
    uint32_t now = millis();
    uint32_t now_starts = g_command_starts.load(std::memory_order_relaxed);
    if (!g_command_active.load() || now_starts != starts) {  // idle, or another command
        starts = now_starts;
        since_ms = now;
        reported = false;
        return;
    }
    if (reported || now - since_ms < STUCK_MS) return;
    reported = true;
    Serial.printf("STUCK cmd=%02x lba=%lu count=%lu busy_ms=%lu fetches=%lu last_fetch=%lu lba=%lu %s ok=%d "
                  "ago_ms=%lu results_waiting=%u\n",
                  g_last_command.command, (unsigned long)g_last_command.lba, (unsigned long)g_last_command.count,
                  (unsigned long)(now - since_ms), (unsigned long)g_last_fetch.count, (unsigned long)g_last_fetch.id,
                  (unsigned long)g_last_fetch.lba, g_last_fetch.store ? "store" : "read", (int)g_last_fetch.ok,
                  (unsigned long)(now - g_last_fetch.at_ms), unsigned(g_mailbox.resultsWaiting()));
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

#if FIRMWARE_SLICE != 0
// A CARD line when the card the image is on is pulled out, comes back, or a
// different card (or the image changed) comes back, which isn't served.
static void printCardChanges() {
    static media_watch::State shown = sd_card::media().state();
    const media_watch::Watch& card = sd_card::media();
    if (card.state() == shown) return;
    char line[160];
    int n;
    switch (card.state()) {
        case media_watch::State::Present:
            n = std::snprintf(line, sizeof line, "CARD back %s\n", g_image_name);
            break;
        case media_watch::State::Changed:
            n = std::snprintf(line, sizeof line,
                              "CARD changed: %s; not served (eject the disk on the computer, then restart the drive)\n",
                              card.why());
            break;
        default:
            n = std::snprintf(line, sizeof line, "CARD lost: the image's reads and writes fail until it's back\n");
            break;
    }
    if (printLine(line, size_t(n))) shown = card.state();
}

// A SLOWREAD line for each read IORDY held past ATA's limit. The time is when
// core 0 collected the report: up to a sector's store (about 0.5 ms on the
// SD card) after the read.
static void printSlowReads() {
    static iordy_timer::OverLimit pending;
    static bool has_pending = false;
    for (;;) {
        if (!has_pending && !iordy_timer::takeOverLimit(pending)) return;
        has_pending = true;
        char line[48];
        int n = std::snprintf(line, sizeof line, "SLOWREAD %lu %s %lu\n", (unsigned long)pending.at_us,
                              pending.reg, (unsigned long)pending.ns);
        if (n <= 0 || !printLine(line, size_t(n) < sizeof line ? size_t(n) : sizeof line - 1)) return;
        has_pending = false;
    }
}
#endif

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
#if FIRMWARE_SLICE == 0
    Serial.printf(" window=%lu skipped=%lu", (unsigned long)g_window.load(), (unsigned long)g_skipped.load());
#endif
#if FIRMWARE_SLICE != 0
    iordy_timer::poll();
    Serial.printf(" iordy_slow=%lu iordy_max_ns=%lu iordy_max_reg=%s iordy_max_at_us=%lu",
                  (unsigned long)iordy_timer::slowReads(), (unsigned long)iordy_timer::maxNs(),
                  iordy_timer::maxRegister(), (unsigned long)iordy_timer::maxAtUs());
    Serial.printf(" iordy_early=%lu iordy_early_reg=%s", (unsigned long)iordy_timer::earlyReads(),
                  iordy_timer::earlyRegister());
    Serial.printf(" bus_sectors=%lu bus_bad=%lu bus_skipped=%lu bus_gaps=%lu",
                  (unsigned long)bus_observer::sectorsChecked(), (unsigned long)bus_observer::sectorsBad(),
                  (unsigned long)bus_observer::sectorsSkipped(), (unsigned long)bus_observer::gaps());
    Serial.printf(" bus_identify=%lu bus_identify_bad=%lu", (unsigned long)bus_observer::identifyChecked(),
                  (unsigned long)bus_observer::identifyBad());
    Serial.printf(" bus_contested=%lu bus_stalls=%lu", (unsigned long)iordy_timer::contestedAccesses(),
                  (unsigned long)iordy_timer::stallCycles());
    Serial.printf(" clk_sys_mhz=%lu clk_peri_mhz=%lu", (unsigned long)(clock_get_hz(clk_sys) / 1000000),
                  (unsigned long)(clock_get_hz(clk_peri) / 1000000));
    Serial.printf(" disk=%s sectors=%lu disk_ms=%lu", g_disk_name, (unsigned long)g_disk->sectors(),
                  (unsigned long)g_disk_ms);
    Serial.printf(" image_name=%s config=%s", g_image_name, g_config_state);
    const media_watch::Watch& card = sd_card::media();
    Serial.printf(" card=%s card_lost=%lu card_back=%lu", media_watch::stateName(card.state()),
                  (unsigned long)card.losses(), (unsigned long)card.returns());
    Serial.printf(" bus_trace=%d seq=%lu rand=%lu activity=%s",
                  (int)g_trace_bus.load(), (unsigned long)g_activity.sequentialCommands(),
                  (unsigned long)g_activity.randomCommands(), activity::stateName(g_activity.state()));
    Serial.printf(" write_behind=%d", (int)g_write_behind.load());
    char last[activity_link::SENTENCE_MAX] = "-";
    if (g_link.count()) {
        size_t n = activity_link::formatSentence(g_link.last(), last, sizeof last);
        if (n >= 2) last[n - 2] = '\0';  // without the \r\n
    }
    Serial.printf(" link=%d link_sent=%lu link_last=%s", (int)g_link_on, (unsigned long)g_link.count(), last);
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
#if FIRMWARE_SLICE != 0
        static bool edit_line = false;  // `E`, then key = value and Enter
        static char edit_buf[96];
        static size_t edit_len = 0;
        if (edit_line) {
            if (c == '\r' || c == '\n') {
                edit_line = false;
                edit_buf[edit_len] = '\0';
                editConfig(edit_buf);
            } else if (edit_len + 1 < sizeof edit_buf) {
                edit_buf[edit_len++] = char(c);
            }
            continue;
        }
        static bool delay_digit_next = false;  // `D` then a digit
        if (delay_digit_next) {
            delay_digit_next = false;
            if (c >= '0' && c <= '9') {
                uint32_t ns = uint32_t(c - '0') * 250;
                ide_bus::setDataReadDelay(ns);
                Serial.printf("DELAY %lu\n", (unsigned long)ns);
                continue;
            }
        }
#endif
        switch (c) {
            case 'h':
                printReady();
#if FIRMWARE_SLICE == 0
                Serial.println("# h help, p pins, s stats, m mark, w capture window after each RESET# on/off, "
                               "x short test (cable unplugged), b reboot to bootloader");
#else
                Serial.println("# h help, p pins, s stats, r reset read timings, t raw bus trace on/off, "
                               "m mark, S SD card and settings, C create HDDSYNTH.INI, E set a setting, ! restart, "
                               "I create SD image, K create bench image, "
                               "J serve the bench image or back (restarts), T serve the test disk or back (restarts), "
                               "L activity link, U link loopback test, V drive gone/back, W write-behind, "
                               "x short test (cable unplugged), "
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
#if FIRMWARE_SLICE == 0
            case 'w': {  // the capture window on or off
                uint32_t on = !g_window.load();
                g_window.store(on);
                Serial.printf("WINDOW %s\n", on ? "on" : "off");
                break;
            }
#endif
#if FIRMWARE_SLICE != 0
            case 'r':
                iordy_timer::reset();
                bus_observer::resetCounts();
                break;
            case 'D': delay_digit_next = true; break;  // debug: delay data reads
            case 'U': startLoopback(); break;  // the link's loopback test
            case 'V': setAbsent(!g_absent.load(), ""); break;  // the drive gone from the bus, or back
            case 'L':  // the activity link on or off
                g_link_on = !g_link_on;
                Serial.printf("LINK %s\n", g_link_on ? "on" : "off");
                break;
            case 'W': {  // debug: write-behind on or off, to compare
                bool on = !g_write_behind.load();
                g_write_behind.store(on);
                Serial.printf("WRITEBEHIND %s\n", on ? "on" : "off");
                break;
            }
            case 'A': {  // debug: read-ahead on or off, to compare
                bool on = !g_read_ahead.load();
                g_read_ahead.store(on);
                Serial.printf("READAHEAD %s\n", on ? "on" : "off");
                break;
            }
            case 'F': {  // debug: the fast lane on or off, to compare
                bool on = !g_fast_lane.load();
                g_fast_lane.store(on);
                Serial.printf("FASTLANE %s\n", on ? "on" : "off");
                break;
            }
            case 't': {
                bool on = !g_trace_bus.load();
                g_trace_bus.store(on);
                Serial.printf("# raw bus trace %s\n", on ? "on" : "off");
                break;
            }
#endif
#if FIRMWARE_SLICE != 0
            case 'S':
                sd_card::printInfo(Serial);
                printConfig();
                break;
            case 'C':  // create HDDSYNTH.INI with the defaults, if it isn't there
                if (sd_card::configExists()) {
                    Serial.printf("# %s already exists: left as it is\n", drive_config::FILE_NAME);
                } else if (sd_card::writeConfig(drive_config::defaultFile(), strlen(drive_config::defaultFile()), Serial)) {
                    Serial.printf("# created %s with the defaults\n", drive_config::FILE_NAME);
                }
                break;
            case 'E':  // set one setting in HDDSYNTH.INI: key = value, then Enter
                edit_line = true;
                edit_len = 0;
                Serial.println("# type key = value (image or mode), then Enter");
                break;
            case '!':  // restart, to apply HDDSYNTH.INI (keeps a `J` or `T` choice)
                Serial.println("# restarting");
                Serial.flush();
                delay(50);
                watchdog_reboot(0, 0, 10);
                break;
            case 'I': sd_card::createImage(sd_card::IMAGE_NAME, gen_disk::DEFAULT_SECTORS, Serial); break;
            case 'K': sd_card::createImage(sd_card::BENCH_IMAGE_NAME, gen_disk::DEFAULT_SECTORS, Serial); break;
            case 'J': {  // serve the other image after a restart (replug the adapter afterwards)
                bool bench = !benchRequested();
                watchdog_hw->scratch[0] = bench ? BENCH_MAGIC : 0;
                Serial.printf("# restarting to serve %s\n", bench ? sd_card::BENCH_IMAGE_NAME : sd_card::IMAGE_NAME);
                Serial.flush();
                delay(50);
                watchdog_reboot(0, 0, 10);
                break;
            }
            case 'T': {  // serve the test disk after a restart, or back (replug the adapter afterwards)
                bool test = !testDiskRequested();
                watchdog_hw->scratch[0] = test ? TEST_DISK_MAGIC : 0;
                Serial.printf("# restarting to serve %s\n", test ? "the test disk" : "the card's image or the welcome disk");
                Serial.flush();
                delay(50);
                watchdog_reboot(0, 0, 10);
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

// The peripheral clock (SPI for the card, the link's UART) runs from the
// system clock, as it does at the default 150 MHz. Setting another clock
// (board_build.f_cpu in platformio.ini) moves it to the 48 MHz USB clock,
// where SPI would top out at 24 MHz.
static void peripheralClockFromSystem() {
    clock_configure_undivided(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                              clock_get_hz(clk_sys));
}

void setup() {
    peripheralClockFromSystem();  // before SPI and the UART work out their dividers
    Serial.begin(115200);
#if FIRMWARE_SLICE != 0
    gpio_init(cfg::PIN_BOARD_LED);
    gpio_set_dir(cfg::PIN_BOARD_LED, true);
    beginLink();
    chooseDisk();
#endif
    printReady();
}

void loop() {
#if FIRMWARE_SLICE != 0
    serveFetches();
    iordy_timer::poll();
    printSlowReads();
    // Data sectors only on the generated disk; IDENTIFY there, and around detection.
    bus_observer::poll(Serial, g_test_disk, int32_t(g_identify_check_until_ms - millis()) > 0);
    updateLed();
    serviceLoopback();
    serviceAbsence();
    checkStuck();
    sd_card::serviceMedia();
    printCardChanges();

    // Re-evaluate the activity state once a millisecond, before anything
    // below can return early on a busy serial link.
    uint32_t now_us = time_us_32();
    if (now_us - g_activity_checked_us >= 1000) {
        g_activity_checked_us = now_us;
        if (g_activity.update(now_us)) g_activity_unprinted = true;
        serviceLink();
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
