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
#include "drive_params.h"
#include "drive_timing.h"
#include "identify.h"
#include "seek_timing.h"
#include "sentence.h"
#include "ata_device.h"
#include "block_source.h"
#include "bus_observer.h"
#include <hardware/watchdog.h>
#include "fetch_mailbox.h"
#include "ide_bus.h"
#include "ide_dma.h"
#include "iocs16.h"
#include "bus_timing.h"
#include <hardware/structs/m33.h>
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

// On the PCB, connects the IDE bus: its bus switches stay off until BUS_EN#
// goes low. Called once the bus pins are set up (inputs, no pulls; on the
// drive build DD and IORDY released), so the host never sees them in another
// state. BUS_EN# isn't a bus line, so the listen build may drive it too. On
// the BB48 GPIO 41 is unconnected.
static void busSwitchesOn() {
    gpio_init(cfg::PIN_BUS_EN_N);
    gpio_put(cfg::PIN_BUS_EN_N, false);
    gpio_set_dir(cfg::PIN_BUS_EN_N, true);
    busy_wait_us(10);  // the switches close within nanoseconds; margin for the wiring
}

// ---- Core 1: slice 0, listen only ---------------------------------------------
#if FIRMWARE_SLICE == 0

// The capture window (console `w`, cfg::LISTEN_WINDOW_EVENTS): off, every
// access is recorded; on, only a window after each RESET# assert.
static std::atomic<uint32_t> g_window{0};
static std::atomic<uint32_t> g_skipped{0};  // accesses outside the window

void setup1() {
    listen::begin();
    busSwitchesOn();
}

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
static SharedFlag g_early{false};       // HDDSYNTH.INI early_answers: ide_bus_early (ide_bus::begin)
static SharedFlag g_isa{false};         // HDDSYNTH.INI isa: IORDY from the strobe (ide_bus::begin)
static SharedFlag g_read_ahead{true};  // HDDSYNTH.INI read_ahead; console `A` (debug)
// Off by default: with it on, writes hold some status reads 1.5-2 us (ATA
// allows 1.25), not yet understood. Console `W` turns it on to work on that.
static SharedFlag g_write_behind{true};  // HDDSYNTH.INI write_behind; console `W`
static bool g_link_on = true;  // HDDSYNTH.INI activity_link; console `L` (core 0 only)

// For the activity LED (core 0's updateLed): core 1 counts commands as they
// start and says whether one is running.
static std::atomic<uint32_t> g_command_starts{0};
static SharedFlag g_command_active{false};

// The disk's size, set by core 0 once it has chosen the disk (setup()).
// Core 1 starts first, so it waits for it: IDENTIFY reports the real size.
// The PIO mode to offer (HDDSYNTH.INI's `mode`) is set before it.
static std::atomic<uint32_t> g_disk_sectors{0};
static std::atomic<uint8_t> g_pio_mode{0};
static std::atomic<uint8_t> g_mdma_mode{NO_DMA};       // set by core 1 at start-up (setup1)
static std::atomic<uint8_t> g_mode_asked{0};            // HDDSYNTH.INI's mode, for core 1
static SharedFlag g_dmarq_wired{false};                 // found by core 1 at start-up
static SharedFlag g_dma_auto{false};                    // HDDSYNTH.INI dma: auto may offer DMA

// What IDENTIFY says: the device's, and the bus observer's to check it by.
static IdentifyInfo driveIdentity() {
    return IdentifyInfo{"HDDSYNTH PROTO", "HSP-000001", FIRMWARE_VERSION,
                        g_disk_sectors.load(std::memory_order_acquire),
                        g_pio_mode.load(std::memory_order_relaxed),
                        g_mdma_mode.load(std::memory_order_relaxed)};
}

// DMARQ wired? The host pulls DMARQ down (ATA asks for 5.6 kΩ), so with the
// pin undriven and the RP2350's own pull-up on (about 50 kΩ: only pull-downs
// are forbidden, erratum E9), a wired pin reads 0 and a bare one 1. After the
// bus switches are on (on the PCB the pin would look bare otherwise), before
// ide_dma takes the pin. An unpowered host's pull-down still holds it low.
static bool dmarqWired() {
    gpio_init(cfg::PIN_DMARQ);  // an input
    gpio_pull_up(cfg::PIN_DMARQ);
    busy_wait_us(1000);  // the pull-up charges the wire, if there's no pull-down
    bool wired = !gpio_get(cfg::PIN_DMARQ);
    gpio_disable_pulls(cfg::PIN_DMARQ);
    return wired;
}

// Core 1's longest loop passes, in cycles (DWT CYCCNT), by what the pass did,
// for console `s` (`c1_gap_ns`): a pass that takes long is a register read
// left waiting. Racy across cores, as diagnostics may be; `r` clears it.
enum : uint8_t { GAP_ACCESS = 1, GAP_LANE, GAP_COMMAND, GAP_BUS_ONLY, GAP_STEP0 };  // steps 0-4 follow
constexpr unsigned GAP_KINDS = GAP_STEP0 + 5;
static volatile uint32_t g_c1_gap[GAP_KINDS];
static volatile uint32_t g_c1_fifo_cycles, g_c1_fifo_max;  // the FIFO check alone (diagnostic)
static volatile uint32_t g_c1_passes;
static uint32_t g_bench_pio, g_bench_ram, g_bench_sio, g_bench_nop;  // cycles per 1000, measured in setup1
static volatile uint32_t g_bench_var;
static uint32_t g_bench_pio_ram, g_bench_nop_ram;  // the same loops, from core 1's RAM code
CORE1_RAM __attribute__((noinline)) static void benchFromRam() {
    uint32_t t = m33_hw->dwt_cyccnt;
    for (int i = 0; i < 1000; ++i) __asm volatile("nop");
    g_bench_nop_ram = m33_hw->dwt_cyccnt - t;
    t = m33_hw->dwt_cyccnt;
    for (int i = 0; i < 1000; ++i) (void)pio0_hw->fstat;
    g_bench_pio_ram = m33_hw->dwt_cyccnt - t;
}
  // core 1's loop passes, for the average (`c1_pass_ns`)
static uint32_t g_bus_only_cycles;  // cfg::EARLY_BUS_ONLY_US in core 1's cycles, set in setup1
static const char* const GAP_NAMES[GAP_KINDS] = {"-", "access", "lane", "command", "busonly",
                                                  "step0", "step1", "step2", "step3", "step4"};

void setup1() {
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;  // core 1's own cycle counter, for g_c1_gap
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
    g_bus_only_cycles = cfg::EARLY_BUS_ONLY_US * (clock_get_hz(clk_sys) / 1000000);
    {  // micro-benchmark (diagnostic): cycles per 1000 reads, on core 1, from flash code here
        uint32_t t = m33_hw->dwt_cyccnt;
        for (int i = 0; i < 1000; ++i) __asm volatile("nop");
        g_bench_nop = m33_hw->dwt_cyccnt - t;
        t = m33_hw->dwt_cyccnt;
        for (int i = 0; i < 1000; ++i) (void)pio0_hw->fstat;
        g_bench_pio = m33_hw->dwt_cyccnt - t;
        t = m33_hw->dwt_cyccnt;
        for (int i = 0; i < 1000; ++i) (void)g_bench_var;
        g_bench_ram = m33_hw->dwt_cyccnt - t;
        t = m33_hw->dwt_cyccnt;
        for (int i = 0; i < 1000; ++i) (void)sio_hw->gpio_in;
        g_bench_sio = m33_hw->dwt_cyccnt - t;
        benchFromRam();
    }
    // Until core 0 has chosen the disk the bus pins stay inputs, so the host
    // sees no drive for that moment (well under a second with an SD card).
    while (g_disk_sectors.load(std::memory_order_acquire) == 0) tight_loop_contents();
    ide_bus::begin(g_isa.load(), g_early.load());
    busSwitchesOn();
    // Whether to offer DMA, before anything builds IDENTIFY (driveIdentity).
    g_dmarq_wired.store(dmarqWired());
    g_mdma_mode.store(drive_config::mdmaModeOffered(drive_config::Mode(g_mode_asked.load(std::memory_order_relaxed)),
                                                    g_dmarq_wired.load(), g_dma_auto.load()),
                      std::memory_order_relaxed);
    iordy_timer::begin(cfg::IORDY_REPORT_NS);  // here, so these never race to claim a PIO
    bus_observer::begin(g_disk_sectors.load(std::memory_order_relaxed), driveIdentity());
    // The third PIO block: multiword DMA if it's offered, otherwise IOCS16#
    // for ISA cards (which never use DMA). Neither fits beside the others.
    if (g_mdma_mode.load(std::memory_order_relaxed) != NO_DMA)
        ide_dma::begin();  // after ide_bus: its block's function on DD0-15 is the one to hand back to
    else {
        iocs16::begin();
        // DMACK#, as ide_dma::begin() leaves it: an input for console `p`.
        gpio_init(cfg::PIN_DMACK_N);
        gpio_disable_pulls(cfg::PIN_DMACK_N);
    }
    bus_timing::begin();  // the read timing probe, where there's room (not beside ide_dma)
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
        ide_bus::updateIntrq(dev);
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

// BSY for a while: waking from standby, or after a data-out block. Step 3 of
// loop1, and early_answers' tight bus loop runs it too, since the host may
// poll status throughout the hold and the loop never goes quiet to return.
CORE1_RAM static void busyTimers(ata::Device& dev) {
    static uint32_t wake_start_us = 0;  // 0: not timing a wake
    static uint32_t hold_start_us = 0;  // 0: not timing a data-out hold
    if (!dev.dataOutHeld()) {
        hold_start_us = 0;
    } else if (hold_start_us == 0) {
        hold_start_us = time_us_32() | 1;
    } else if (time_us_32() - hold_start_us >= cfg::DATA_OUT_BSY_US) {
        dev.releaseDataOut();
        ide_bus::updateIntrq(dev);
    }
    if (!dev.waking()) {
        wake_start_us = 0;
    } else if (wake_start_us == 0) {
        wake_start_us = time_us_32() | 1;  // never 0 while timing
    } else if (time_us_32() - wake_start_us >= cfg::WAKE_BUSY_US) {
        dev.finishWake();
        ide_bus::updateIntrq(dev);
    }
}

CORE1_RAM static void recordCommandStart(uint32_t t_us, const ata::CommandInfo& c) {
    record(commandStartEvent(t_us, c.command, c.lba, c.count));
}

// early_answers: a queued command, run from the tight bus loop (ide_bus::
// serveBusEarly) so core 1 doesn't leave it for one: going back to the main
// loop for each command left the host's next accesses waiting up to a pass
// (0.23.11). Its CMD line is recorded at once; loop1's step 0 records DONE
// from g_early_started and g_early_command_start_us.
static uint32_t g_early_started = 0;
static uint32_t g_early_command_start_us = 0;
CORE1_RAM static void runCommandEarly(ata::Device& dev) {
    dev.setReadAhead(g_read_ahead.load());
    dev.setWriteBehind(g_write_behind.load());
    uint32_t before = dev.commandsStarted();
    dev.runQueuedCommand();
    ide_bus::updateIntrq(dev);
    if (dev.commandsStarted() != before) {
        g_early_started = dev.commandsStarted();
        g_early_command_start_us = time_us_32();
        recordCommandStart(g_early_command_start_us, dev.lastCommand());
        g_command_starts.fetch_add(1, std::memory_order_relaxed);
        g_command_active.store(true);
    }
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
    ide_bus::updateIntrq(dev);
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
    bool reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
    dev.hardReset(reset_low);
    const bool early = g_early.load();  // ide_bus_early's samples (early_answers)
    uint32_t last_access_cycles = 0;  // core 1's cycle counter: core-local, unlike the timer behind the APB bridge
    const uint32_t bus_only_cycles = g_bus_only_cycles;
    ide_bus::updateIntrq(dev);  // driven low from the start: an ISA card's line would float high
    unsigned step = 0;

    uint32_t last_pass = m33_hw->dwt_cyccnt;
    uint8_t pass_kind = 0;
    for (;;) {
        uint32_t now = m33_hw->dwt_cyccnt;
        if (now - last_pass > g_c1_gap[pass_kind]) g_c1_gap[pass_kind] = now - last_pass;
        last_pass = now;
        g_c1_passes = g_c1_passes + 1;
        pass_kind = GAP_ACCESS;
        uint32_t sample;
        uint32_t f0 = m33_hw->dwt_cyccnt;
        bool got = ide_bus::serviceOnce(dev, sample);
        uint32_t f = m33_hw->dwt_cyccnt - f0;
        if (!got) {
            g_c1_fifo_cycles = g_c1_fifo_cycles + f;
            if (f > g_c1_fifo_max) g_c1_fifo_max = f;
        }
        if (got) {
            if (g_trace_bus.load()) record(TraceKind::Bus, sample);
            if (early) {
                last_access_cycles = m33_hw->dwt_cyccnt;
                if (!g_trace_bus.load()) ide_bus::serveBusEarly(dev, reset_low, g_mailbox, posted_fetch, bus_only_cycles, busyTimers, runCommandEarly);
            }
            continue;
        }
        pass_kind = GAP_LANE;
        // Mid-block, the fast lane answers data reads with nothing else in
        // between. Not while the raw trace or the debug delay is on.
        const uint16_t* next;
        uint16_t left;
        if (dev.dataInRemaining(next, left)) {
            if (g_fast_lane.load() && !g_trace_bus.load() && !ide_bus::dataReadDelayed()) {
                if (early ? ide_bus::serveDataInEarly(dev, reset_low, g_mailbox, posted_fetch)
                          : ide_bus::serveDataIn(dev, reset_low, g_mailbox, posted_fetch))
                    continue;
            } else {
                postFetch(dev, posted_fetch);  // the read-ahead, without the lane
            }
        }
        pass_kind = GAP_COMMAND;
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
            ide_bus::updateIntrq(dev);
            // early_answers: straight back to the tight bus loop (status polls, the
            // data block); the bookkeeping below can wait until the bus is quiet.
            if (early && !g_trace_bus.load())
                ide_bus::serveBusEarly(dev, reset_low, g_mailbox, posted_fetch, bus_only_cycles, busyTimers, runCommandEarly);
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
        // early_answers: for a while after any access, only the bus, so a host
        // that doesn't wait finds core 1 free to answer its next request.
        pass_kind = GAP_BUS_ONLY;
        if (early && m33_hw->dwt_cyccnt - last_access_cycles < bus_only_cycles) continue;
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

        pass_kind = uint8_t(GAP_STEP0 + step);
        switch (step) {
            case 0:  // the trace's CMD and DONE lines, in that order, at most one per pass
                if (early && g_early_started != 0 && g_early_started != started) {
                    started = g_early_started;  // commands the tight loop ran, CMD already recorded
                    command_start_us = g_early_command_start_us;
                }
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
                    ide_bus::updateIntrq(dev);
                }
                break;
            }
            case 3:  // BSY for a while: waking from standby, or after a data-out block
                busyTimers(dev);
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
                    ide_bus::updateIntrq(dev);
                    record(low ? TraceKind::ResetAssert : TraceKind::ResetRelease, pins);
                }
                break;
            }
        }
        step = step == 4 ? 0 : step + 1;
        // early_answers: back to the tight bus loop after every pass, so a quiet
        // spell's first access finds core 1 polling, not mid-pass (660 ns).
        if (early && !g_trace_bus.load())
            ide_bus::serveBusEarly(dev, reset_low, g_mailbox, posted_fetch, bus_only_cycles, busyTimers, runCommandEarly);
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
    g_mode_asked.store(uint8_t(g_config.mode), std::memory_order_relaxed);  // core 1 decides DMA from it
    // The on/off settings (the console's W, A and L change them until a restart).
    g_dma_auto.store(g_config.dma);
    g_write_behind.store(g_config.write_behind);
    g_read_ahead.store(g_config.read_ahead);
    g_link_on = g_config.activity_link;
    g_isa.store(g_config.isa);  // core 1 sets up the bus program from it
    g_early.store(g_config.early_answers);
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
        Serial.printf("# E: '%s' isn't a setting (image, mode, dma, write_behind, read_ahead, activity_link, seek_timing, isa, early_answers), "
                      "or the file would be too long\n", key);
        return;
    }
    if (!sd_card::writeConfig(edited, n, Serial)) return;
    drive_config::Config check;
    drive_config::parse(edited, n, check);
    Serial.printf("# saved %s = %s in %s; restart (!) to use it\n", key, value, drive_config::FILE_NAME);
    size_t shown = check.warning_count < drive_config::MAX_WARNINGS ? check.warning_count : drive_config::MAX_WARNINGS;
    for (size_t i = 0; i < shown; ++i) Serial.printf("CONFIG warning: %s\n", check.warnings[i]);
}

static void printSeekConfig();

// Console `S`: the settings and what came of them, after the card's own line.
static void printConfig() {
    drive_config::Mode used = drive_config::modeSupported(g_config.mode) ? g_config.mode : drive_config::Mode::Auto;
    uint8_t mdma = g_mdma_mode.load();
    char mdma_text[8] = "none";
    if (mdma != NO_DMA) std::snprintf(mdma_text, sizeof mdma_text, "%u", unsigned(mdma));
    Serial.printf("CONFIG file=%s image=%s mode=%s mode_used=%s pio_offered=%u mdma_offered=%s serving=%s contiguous=%s extents=%u\n",
                  g_config_state,
                  g_config.image, drive_config::modeName(g_config.mode), drive_config::modeName(used),
                  unsigned(g_pio_mode.load()), mdma_text, g_image_name,
                  sd_card::imageContiguous() ? "yes" : "no", sd_card::imageExtents());
    if (g_image_note[0]) Serial.printf("CONFIG image not used: %s\n", g_image_note);
    Serial.printf("CONFIG dmarq=%s dma=%s write_behind=%s read_ahead=%s activity_link=%s isa=%s early_answers=%s\n",
                  g_dmarq_wired.load() ? "wired" : "absent", drive_config::onOff(g_config.dma),
                  drive_config::onOff(g_config.write_behind), drive_config::onOff(g_config.read_ahead),
                  drive_config::onOff(g_config.activity_link), drive_config::onOff(g_config.isa),
                  drive_config::onOff(g_config.early_answers));
    printSeekConfig();
    if (drive_config::dmaRefusedForWiring(g_config.mode, g_dmarq_wired.load()))
        Serial.printf("CONFIG warning: mode = %s needs DMARQ wired (IDE pin 21 to GPIO 31); offering PIO only\n",
                      drive_config::modeName(g_config.mode));
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

// The activity link to an HDD Synth (activity_link.h), on UART0. Core 0
// must never wait on it, so a sentence (at most 23 bytes) is written only
// when the 32-byte TX FIFO is empty; otherwise it stays due and goes a
// millisecond later, with whatever is current then. $HDA goes first; $HDE,
// where each media command takes the head, fills the gaps (at most one every
// cfg::LINK_HEAD_GAP_US, the newest).
//
// The other direction (GPIO 1, protocol version 2) brings the synth's $HDM:
// the modelled drive it's playing, for seek_timing = synth. GPIO 1 is pulled
// up (a UART line idles high), so unconnected it reads idle, not noise.
static activity_link::Sender g_link(cfg::LINK_HEARTBEAT_MS);
static activity_link::HeadPacer g_head(cfg::LINK_HEAD_GAP_US);
static activity_link::Op g_link_op = activity_link::Op::None;  // the last media command's
static LineAssembler g_link_rx;
static seek_timing::SynthModel g_synth;
static uint32_t g_link_rx_errors = 0;  // framing, parity, break or overrun

static void beginLink() {
    uart_init(uart0, cfg::LINK_BAUD);  // 8N1
    gpio_set_function(cfg::PIN_LINK_TX, GPIO_FUNC_UART);
    gpio_set_pulls(cfg::PIN_LINK_RX, true, false);  // a pull-up only (erratum E9)
    gpio_set_function(cfg::PIN_LINK_RX, GPIO_FUNC_UART);
}

// A head target for $HDE: the command's first sector, from `at_us` (when its
// seek starts).
static void postHeadTarget(uint32_t lba, uint32_t sectors, activity_link::Op op, uint64_t at_us) {
    activity_link::HeadTarget t;
    t.pos = activity_link::headPosition(lba, g_disk->sectors());
    t.op = op;
    t.sectors = sectors;
    g_head.post(t, at_us);
}

static bool loopbackSending();

static void serviceLink() {
    if (!g_link_on || loopbackSending()) return;
    activity_link::Fields f;
    f.busy = g_busy;
    f.state = g_activity.state();
    f.op = g_link_op;
    uint32_t now = millis();
    uint64_t now_us = time_us_64();
    activity_link::Next next = activity_link::nextSentence(g_link, f, now, g_head, now_us);
    if (next == activity_link::Next::Nothing) return;
    if (!(uart_get_hw(uart0)->fr & UART_UARTFR_TXFE_BITS)) return;  // not empty yet
    char line[activity_link::HEAD_SENTENCE_MAX];
    size_t n = next == activity_link::Next::Activity ? activity_link::formatSentence(f, line, sizeof line)
                                                     : activity_link::formatHeadSentence(g_head.target(), line, sizeof line);
    if (n == 0) return;
    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(line), n);  // fits: never waits
    if (next == activity_link::Next::Activity)
        g_link.sent(f, now);
    else
        g_head.sent(now_us);
}

// Authentic seek timing (seek_timing.h): HDDSYNTH.INI's seek_timing, which
// console `H` turns off and on until a restart. The model is a preset, or
// the synth's from $HDM; serveFetches() holds each result until its time.
static seek_timing::Scheduler g_seek;
static SeekModel g_seek_preset;
static bool g_seek_on = true;

static void beginSeekTiming() {
    uint32_t sectors = g_disk->sectors();
    g_seek.setGeometry(sectors, defaultGeometry(sectors).sectors_per_track);
    if (g_config.seek_timing.source == seek_timing::Source::Preset) {
        DriveParams p;
        drivePreset(g_config.seek_timing.preset, p);
        g_seek_preset = seekModelOf(p);
    }
}

// The model to follow now, or null.
static const SeekModel* seekModel(uint32_t now_ms) {
    if (!g_seek_on) return nullptr;
    switch (g_config.seek_timing.source) {
        case seek_timing::Source::Synth: return g_synth.model(now_ms);
        case seek_timing::Source::Preset: return &g_seek_preset;
        default: return nullptr;
    }
}

// A model as "voice 5400rpm 6000cyl stroke=19.0ms track=2.0ms", for S and s.
static void describeModel(const SeekModel* m, char* out, size_t size) {
    if (!m) {
        std::snprintf(out, size, "none");
        return;
    }
    if (m->actuator == Actuator::Stepper)
        std::snprintf(out, size, "stepper %ldrpm %ldcyl step=%ld.%ldms zoned=%d", lroundf(m->rpm), lroundf(m->cylinders),
                      lroundf(m->stepMs * 10) / 10, lroundf(m->stepMs * 10) % 10, int(m->zoned));
    else
        std::snprintf(out, size, "voice %ldrpm %ldcyl stroke=%ld.%ldms track=%ld.%ldms zoned=%d", lroundf(m->rpm),
                      lroundf(m->cylinders), lroundf(m->fullStrokeMs * 10) / 10, lroundf(m->fullStrokeMs * 10) % 10,
                      lroundf(m->trackMs * 10) / 10, lroundf(m->trackMs * 10) % 10, int(m->zoned));
}

// Console `S`'s line for seek timing.
static void printSeekConfig() {
    uint32_t now = millis();
    char model[80];
    const SeekModel* m = g_config.seek_timing.source == seek_timing::Source::Synth ? g_synth.model(now)
                         : g_config.seek_timing.source == seek_timing::Source::Preset ? &g_seek_preset
                                                                                     : nullptr;
    describeModel(m, model, sizeof model);
    Serial.printf("CONFIG seek_timing=%s%s model=%s\n", seek_timing::settingName(g_config.seek_timing),
                  g_seek_on ? "" : " (off: console H)", model);
    if (g_config.seek_timing.source == seek_timing::Source::Synth || g_synth.valid()) {
        if (g_synth.heard(now))
            Serial.printf("CONFIG synth $HDM %lu ms ago: %s\n", (unsigned long)(now - g_synth.lastMs()),
                          g_synth.model(now) ? "a modelled drive" : "a recorded pack (no model)");
        else
            Serial.printf("CONFIG synth: no $HDM in the last %lu s (GPIO 1 wired to the synth's J6 pin 4?)\n",
                          (unsigned long)(seek_timing::MODEL_TIMEOUT_MS / 1000));
    }
}

// The loopback test (console `U`): with a jumper from GPIO 0 (TX) to GPIO 1
// (RX), read back for a second what the link sends ($HDA and $HDE), plus one
// $HDM the test sends itself, as the synth would, so both directions'
// parsers are checked. Polled on every loop pass, so core 0 never waits on
// it and the 32-byte RX FIFO (2.8 ms at 115200 baud) can't overflow.
constexpr uint32_t LOOPBACK_MS = 1000;  // five heartbeats
static struct {
    enum class Phase : uint8_t { Off, Starting, Listening, Finishing } phase = Phase::Off;
    uint32_t until_ms = 0;
    uint32_t sent_before = 0;
    uint32_t head_sent_before = 0;
    uint32_t bytes = 0;
    uint32_t errors = 0;  // framing, parity, break or overrun
    uint32_t valid = 0, head_valid = 0, rejected = 0;
    activity_link::Fields last;
    LineAssembler rx;
    // The $HDM the test sends (longer than the TX FIFO, so a piece at a time),
    // and whether it came back.
    char model_line[SENTENCE_MAX_LINE + 4];
    size_t model_len = 0, model_sent = 0;
    bool model_back = false, model_wrong = false;
} g_loopback;

static bool loopbackSending() {
    return g_loopback.phase != decltype(g_loopback)::Phase::Off && g_loopback.model_sent < g_loopback.model_len;
}

static void startLoopback() {
    if (g_loopback.phase != decltype(g_loopback)::Phase::Off) return;
    if (!g_link_on) {
        Serial.println("# loopback: the link is off (L turns it on)");
        return;
    }
    g_loopback.phase = decltype(g_loopback)::Phase::Starting;
    Serial.printf("# loopback: reading GPIO %u for %lu ms (needs a jumper from GPIO %u)\n",
                  unsigned(cfg::PIN_LINK_RX), (unsigned long)LOOPBACK_MS, unsigned(cfg::PIN_LINK_TX));
}

// The voice97 preset, as the synth would send it.
static size_t loopbackModelLine(char* out, size_t size) {
    DriveParams p;
    drivePreset("voice97", p);
    SeekModel m = seekModelOf(p);
    char body[SENTENCE_MAX_LINE];
    if (!formatModelBody(&m, body, sizeof body)) return 0;
    return formatSentence(out, size, body);
}

static void loopbackLine(const char* line, size_t len) {
    activity_link::Fields f;
    activity_link::HeadTarget t;
    SeekModel m;
    bool present = false;
    if (activity_link::parseLine(line, len, f)) {
        ++g_loopback.valid;
        g_loopback.last = f;
    } else if (activity_link::parseHeadLine(line, len, t)) {
        ++g_loopback.head_valid;
    } else if (parseModelSentence(line, len, m, present)) {
        // Back as it went: the same sentence, formatted again from what was read.
        char body[SENTENCE_MAX_LINE], again[SENTENCE_MAX_LINE + 4];
        size_t n = formatModelBody(present ? &m : nullptr, body, sizeof body);
        size_t k = n ? formatSentence(again, sizeof again, body) : 0;
        bool same = k == g_loopback.model_len && memcmp(again, g_loopback.model_line, k) == 0;
        if (same && !g_loopback.model_back) g_loopback.model_back = true;
        else g_loopback.model_wrong = true;
    } else {
        ++g_loopback.rejected;
    }
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
        switch (g_loopback.rx.feed(char(dr & 0xFF))) {
            case LineAssembler::Result::Line: loopbackLine(g_loopback.rx.line(), g_loopback.rx.length()); break;
            case LineAssembler::Result::Dropped: ++g_loopback.rejected; break;
            default: break;
        }
    }
}

// Sends what fits of the test's $HDM, without waiting.
static void feedLoopbackModel() {
    uart_hw_t* hw = uart_get_hw(uart0);
    while (g_loopback.model_sent < g_loopback.model_len && !(hw->fr & UART_UARTFR_TXFF_BITS))
        hw->dr = uint8_t(g_loopback.model_line[g_loopback.model_sent++]);
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
            // Resetting the UART then (nothing is being sent) clears anything
            // half received and any error left from before.
            if (sending) return;
            uart_init(uart0, cfg::LINK_BAUD);
            g_loopback.rx = LineAssembler();
            g_loopback.bytes = g_loopback.errors = 0;
            g_loopback.valid = g_loopback.head_valid = g_loopback.rejected = 0;
            g_loopback.model_back = g_loopback.model_wrong = false;
            g_loopback.model_len = loopbackModelLine(g_loopback.model_line, sizeof g_loopback.model_line);
            g_loopback.model_sent = 0;
            g_loopback.sent_before = g_link.count();
            g_loopback.head_sent_before = g_head.count();
            g_loopback.until_ms = now + LOOPBACK_MS;
            g_loopback.phase = Phase::Listening;
            feedLoopbackModel();  // first, before the link's next sentence
            return;
        case Phase::Listening:
            feedLoopbackModel();
            drainLoopback();
            if (int32_t(now - g_loopback.until_ms) >= 0) g_loopback.phase = Phase::Finishing;
            return;
        case Phase::Finishing:
            if (sending) return;  // let the last sentence finish arriving
            drainLoopback();
            break;
    }

    uint32_t sent = g_link.count() - g_loopback.sent_before;
    uint32_t head_sent = g_head.count() - g_loopback.head_sent_before;
    bool last_matches = g_loopback.valid && g_loopback.last == g_link.last();
    const char* model = g_loopback.model_wrong ? "bad" : g_loopback.model_back ? "ok" : "missing";
    bool pass = g_loopback.valid == sent && g_loopback.head_valid == head_sent && !g_loopback.rejected &&
                !g_loopback.errors && last_matches && g_loopback.model_back && !g_loopback.model_wrong;
    const char* result = g_loopback.bytes == 0 ? "no-data" : pass ? "pass" : "fail";
    char last[activity_link::SENTENCE_MAX] = "-";
    if (g_loopback.valid) {
        size_t n = activity_link::formatSentence(g_loopback.last, last, sizeof last);
        if (n >= 2) last[n - 2] = '\0';  // without the \r\n
    }
    Serial.printf("LOOPBACK %s sent=%lu valid=%lu rejected=%lu errors=%lu bytes=%lu last=%s head_sent=%lu "
                  "head_valid=%lu model=%s\n",
                  result, (unsigned long)sent, (unsigned long)g_loopback.valid, (unsigned long)g_loopback.rejected,
                  (unsigned long)g_loopback.errors, (unsigned long)g_loopback.bytes, last, (unsigned long)head_sent,
                  (unsigned long)g_loopback.head_valid, model);
    if (g_loopback.bytes == 0)
        Serial.printf("# loopback: nothing arrived; is GPIO %u jumpered to GPIO %u?\n", unsigned(cfg::PIN_LINK_TX),
                      unsigned(cfg::PIN_LINK_RX));
    g_loopback.phase = Phase::Off;
}

// The synth's side of the link: $HDM lines, read on every loop pass (an $HDM
// is about 45 bytes, more than the RX FIFO holds; one lost now and then
// doesn't matter, the next comes 2 s later). The loopback test reads the
// FIFO itself while it runs.
static void serviceLinkRx() {
    if (g_loopback.phase != decltype(g_loopback)::Phase::Off) return;
    uart_hw_t* hw = uart_get_hw(uart0);
    uint32_t now = millis();
    while (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
        uint32_t dr = hw->dr;
        if (dr & (UART_UARTDR_FE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_OE_BITS)) {
            ++g_link_rx_errors;
            continue;
        }
        switch (g_link_rx.feed(char(dr & 0xFF))) {
            case LineAssembler::Result::Line: g_synth.onLine(g_link_rx.line(), g_link_rx.length(), now); break;
            case LineAssembler::Result::Dropped: g_synth.onDropped(); break;
            default: break;
        }
    }
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
        bool seek_only = (command >= 0x10 && command <= 0x1F) || (command >= 0x70 && command <= 0x7F);
        if (op != activity_link::Op::None)
            g_link_op = op;  // its head target goes from serveFetches(), with its first sector
        else
            postHeadTarget(e.pins, seek_only ? 0 : (e.extra >> 8), op, time_us_64());  // verify or seek
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

// Results waiting for their time (seek timing), oldest first. Core 1 has
// one fetch out at a time, so a few are plenty; with timing off each goes at
// once.
struct HeldResult {
    FetchResult result;
    uint64_t ready_us;
};
constexpr size_t HELD_MAX = 4;
static HeldResult g_held[HELD_MAX];
static size_t g_held_first = 0, g_held_count = 0;

static void releaseHeld() {
    if (!g_held_count) return;
    uint64_t now = time_us_64();
    while (g_held_count && g_held[g_held_first].ready_us <= now) {
        while (!g_mailbox.postResult(g_held[g_held_first].result)) {}
        g_held_first = (g_held_first + 1) % HELD_MAX;
        --g_held_count;
    }
}

static void serveFetches() {
    releaseHeld();
    ata::Fetch fetch;
    while (g_held_count < HELD_MAX && g_mailbox.takeRequest(fetch)) {
        uint64_t now = time_us_64();  // the command's time starts before the card's
        bool ok = fetch.store ? g_disk->write(fetch.lba, fetch.buffer)
                  : fetch.flush ? true  // FLUSH CACHE: nothing to move
                                : g_disk->read(fetch.lba, fetch.buffer);
        if (ok && fetch.flush) ok = g_disk->flush();
        uint64_t ready = now;
        if (fetch.store || !fetch.flush) {  // a sector read or written
            seek_timing::Release r = g_seek.onSector(now, fetch.command_lba, fetch.lba);
            ready = r.ready_us;
            if (r.first)
                postHeadTarget(fetch.command_lba, fetch.command_sectors,
                               fetch.store ? activity_link::Op::Write : activity_link::Op::Read, r.seek_start_us);
        }
        g_held[(g_held_first + g_held_count) % HELD_MAX] = HeldResult{FetchResult{fetch.id, ok}, ready};
        ++g_held_count;
        releaseHeld();
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

// A NOIORDY line when the host ends reads without waiting for IORDY, at most
// once a second: such a host reads garbage whenever core 1 is slower than its
// strobe (iordy_check.pio).
static void printIgnoredIordy() {
    static uint32_t shown = 0, last_ms = 0;
    uint32_t n = iordy_timer::ignoredReads();
    if (n == shown || millis() - last_ms < 1000) return;
    char line[96];
    int len = std::snprintf(line, sizeof line, "NOIORDY %lu reads=%lu last=%s: the host didn't wait for IORDY\n",
                            (unsigned long)time_us_32(), (unsigned long)n, iordy_timer::ignoredRegister());
    if (printLine(line, size_t(len))) {
        shown = n;
        last_ms = millis();
    }
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
#if FIRMWARE_SLICE != 0
    // Multiword DMA's handshake: set up by ide_dma, so only on the drive build.
    // Also the PCB's card detect and bus-switch enable: unconnected on the
    // BB48, where SD_CD floats.
    Serial.printf("PINS DMARQ=%d DMACK#=%d SD_CD=%d BUS_EN#=%d\n", (int)gpio_get(cfg::PIN_DMARQ),
                  (int)gpio_get(cfg::PIN_DMACK_N), (int)gpio_get(cfg::PIN_SD_CD), (int)gpio_get(cfg::PIN_BUS_EN_N));
#endif
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
    Serial.printf(" iordy_ignored=%lu iordy_ignored_reg=%s", (unsigned long)iordy_timer::ignoredReads(),
                  iordy_timer::ignoredRegister());
    static uint32_t s_passes_then, s_ms_then;
    uint32_t passes = g_c1_passes, ms = millis();
    if (passes != s_passes_then && ms != s_ms_then)
        Serial.printf(" c1_pass_ns=%lu", (unsigned long)(uint64_t(ms - s_ms_then) * 1000000ull / (passes - s_passes_then)));
    static uint32_t s_fifo_then;
    uint32_t fifo = g_c1_fifo_cycles;
    if (passes != s_passes_then)
        Serial.printf(" c1_fifo_cyc=%lu/%lu", (unsigned long)((fifo - s_fifo_then) / (passes - s_passes_then)),
                      (unsigned long)g_c1_fifo_max);
    s_fifo_then = fifo;
    Serial.printf(" c1_bench_per1000=nop:%lu,pio:%lu,ram:%lu,sio:%lu", (unsigned long)g_bench_nop,
                  (unsigned long)g_bench_pio, (unsigned long)g_bench_ram, (unsigned long)g_bench_sio);
    Serial.printf(" c1_bench_ram_per1000=nop:%lu,pio:%lu", (unsigned long)g_bench_nop_ram, (unsigned long)g_bench_pio_ram);
    s_passes_then = passes, s_ms_then = ms;
    Serial.print(" c1_gap_ns=");
    for (unsigned k = 1; k < GAP_KINDS; ++k)
        if (g_c1_gap[k])
            Serial.printf("%s:%lu,", GAP_NAMES[k],
                          (unsigned long)(uint64_t(g_c1_gap[k]) * 1000000000ull / clock_get_hz(clk_sys)));
    bus_timing::poll();
    Serial.printf(" rd_timed=%lu rd_t1_ns=%lu-%lu rd_strobe_ns=%lu-%lu", (unsigned long)bus_timing::count(),
                  (unsigned long)bus_timing::t1MinNs(), (unsigned long)bus_timing::t1MaxNs(),
                  (unsigned long)bus_timing::strobeMinNs(), (unsigned long)bus_timing::strobeMaxNs());
    Serial.printf(" bus_sectors=%lu bus_bad=%lu bus_skipped=%lu bus_gaps=%lu bus_dma_words=%llu",
                  (unsigned long)bus_observer::sectorsChecked(), (unsigned long)bus_observer::sectorsBad(),
                  (unsigned long)bus_observer::sectorsSkipped(), (unsigned long)bus_observer::gaps(),
                  (unsigned long long)bus_observer::dmaWords());
    Serial.printf(" bus_ring_max=%lu/%lu bus_dropped=%llu bus_lost=%lu bus_checks=%s",
                  (unsigned long)bus_observer::ringMax(), (unsigned long)bus_observer::ringSize(),
                  (unsigned long long)bus_observer::dropped(), (unsigned long)bus_observer::lost(),
                  bus_observer::full() ? "full" : "spot");
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
    Serial.printf(" write_behind=%d read_ahead=%d", (int)g_write_behind.load(), (int)g_read_ahead.load());
    char last[activity_link::SENTENCE_MAX] = "-";
    if (g_link.count()) {
        size_t n = activity_link::formatSentence(g_link.last(), last, sizeof last);
        if (n >= 2) last[n - 2] = '\0';  // without the \r\n
    }
    Serial.printf(" link=%d link_sent=%lu link_last=%s", (int)g_link_on, (unsigned long)g_link.count(), last);
    char head[activity_link::HEAD_SENTENCE_MAX] = "-";
    if (g_head.count()) {
        size_t n = activity_link::formatHeadSentence(g_head.last(), head, sizeof head);
        if (n >= 2) head[n - 2] = '\0';
    }
    Serial.printf(" head_sent=%lu head_replaced=%lu head_last=%s", (unsigned long)g_head.count(),
                  (unsigned long)g_head.replaced(), head);
    Serial.printf(" hdm_valid=%lu hdm_rejected=%lu link_rx_errors=%lu", (unsigned long)g_synth.valid(),
                  (unsigned long)g_synth.rejected(), (unsigned long)g_link_rx_errors);
    const SeekModel* model = g_seek.active() ? &g_seek.model() : nullptr;
    uint32_t commands = g_seek.commands();
    Serial.printf(" seek_timing=%s seek_active=%d seek_rpm=%ld seek_actuator=%s", seek_timing::settingName(g_config.seek_timing),
                  int(model != nullptr), model ? lroundf(model->rpm) : 0L, model ? seek_timing::actuatorName(*model) : "-");
    Serial.printf(" seek_commands=%lu seek_held=%lu seek_avg_us=%lu seek_max_us=%lu seek_total_us=%llu seek_capped=%lu",
                  (unsigned long)commands, (unsigned long)g_seek.held(),
                  (unsigned long)(commands ? g_seek.totalUs() / commands : 0), (unsigned long)g_seek.maxUs(),
                  (unsigned long long)g_seek.totalUs(), (unsigned long)g_seek.capped());
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
                               "L activity link, U link loopback test, V drive gone/back, W write-behind, O observer full/spot checks, "
                               "H seek timing on/off, "
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
                bus_timing::reset();
                for (auto& g : g_c1_gap) g = 0;
                bus_observer::resetCounts();
                break;
            case 'D': delay_digit_next = true; break;  // debug: delay data reads
            case 'U': startLoopback(); break;  // the link's loopback test
            case 'V': setAbsent(!g_absent.load(), ""); break;  // the drive gone from the bus, or back
            case 'H':  // seek timing off or on, until a restart (to benchmark)
                g_seek_on = !g_seek_on;
                Serial.printf("SEEKTIMING %s seek_timing=%s\n", g_seek_on ? "on" : "off",
                              seek_timing::settingName(g_config.seek_timing));
                break;
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
            case 'O': {  // the bus observer's full checks on or off (spot checks)
                bool full = !bus_observer::full();
                bus_observer::setFull(full);
                Serial.printf("OBSERVER %s\n", full ? "full" : "spot");
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
                Serial.println("# type key = value (e.g. image = GAMES.IMG or seek_timing = voice97), then Enter");
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
    // The PCB's card detect, readable for `p` (not used yet): an input with no
    // internal pulls (erratum E9; the PCB pulls it up).
    gpio_init(cfg::PIN_SD_CD);
    gpio_disable_pulls(cfg::PIN_SD_CD);
#if FIRMWARE_SLICE != 0
    gpio_init(cfg::PIN_BOARD_LED);
    gpio_set_dir(cfg::PIN_BOARD_LED, true);
    beginLink();
    chooseDisk();
    beginSeekTiming();
#endif
    printReady();
}

void loop() {
#if FIRMWARE_SLICE != 0
    serveFetches();
    iordy_timer::poll();
    bus_timing::poll();
    bus_timing::printPending(2);
    printSlowReads();
    printIgnoredIordy();
    // Data sectors only on the generated disk; IDENTIFY there, and around detection.
    bus_observer::poll(Serial, g_test_disk, int32_t(g_identify_check_until_ms - millis()) > 0, serveFetches);
    updateLed();
    serviceLoopback();
    serviceLinkRx();
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
        g_seek.setModel(seekModel(millis()));
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
