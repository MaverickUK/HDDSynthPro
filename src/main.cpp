// HDDSynthPro IDE drive emulator prototype.
//
// Three builds (FIRMWARE_SLICE and HDDPRO_PCB, in platformio.ini):
//   bb48        slice 0, listen only: core 1 drains the PIO listener and
//               records every bus access. Every pin stays an input.
//   bb48_drive  slices 1-3, the drive: core 1 serves the bus (ide_bus) and
//               runs the device (hdcd::Device); core 0 fetches sectors for
//               it from the SD card's image, the welcome disk or the test
//               disk. By HDDSYNTH.INI: a hard disk, with a second disk or a
//               CD-ROM drive as device 1 (`device1 = cdrom`), or a CD-ROM
//               drive alone (`device0 = cdrom`, docs/atapi.md). Commands are
//               traced; raw accesses on request (console `t`).
//   pcb_drive   bb48_drive for the PCB, the card on 4-bit SDIO.
// In all, core 1 records events into the trace ring and core 0 owns USB
// serial: it prints the trace and runs the console.
#include <Arduino.h>

#include <hardware/gpio.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "bus_sample.h"
#include "config.h"
#include "core1_ram.h"
#include "drive_device.h"
#include "short_test.h"
#include "stack_watch.h"
#include "spsc_ring.h"
#include "trace_format.h"
#include "version.h"

#if FIRMWARE_SLICE == 0
#include "ide_listen.h"
#else
#include <hardware/clocks.h>
#include <hardware/structs/busctrl.h>
#include <hardware/uart.h>

#include "access_log.h"
#include "activity.h"
#include "auto_bus.h"
#include "activity_link.h"
#include "drive_config.h"
#include "drive_params.h"
#include "drive_timing.h"
#include "identify.h"
#include "seek_timing.h"
#include "sentence.h"
#include "ata_device.h"
#include "hdcd_device.h"
#include <new>
#include <type_traits>
#include "block_source.h"
#include "cue_sheet.h"
#include "bus_observer.h"
#include <hardware/watchdog.h>
#include "fetch_mailbox.h"
#include "ide_bus.h"
#include "ide_table.h"
#include "ide_dma.h"
#include "iocs16.h"
#include "bus_timing.h"
#include "command_history.h"
#include "pio_plan.h"
#include "mmc.h"
#include <hardware/structs/m33.h>
#include "iordy_timer.h"
#include "sd_card.h"
#include "scratch.h"
#include "welcome_disk.h"
#include "welcome_text.h"
#endif
#include "main_state.h"

SpscRing<TraceEvent, cfg::TRACE_RING_EVENTS> g_trace;
std::atomic<uint32_t> g_events{0};   // recorded by core 1
std::atomic<uint32_t> g_dropped{0};  // lost because the ring was full
std::atomic<uint32_t> g_stalls{0};   // PIO FIFO filled: accesses missed

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
std::atomic<uint32_t> g_window{0};
std::atomic<uint32_t> g_skipped{0};  // accesses outside the window

void setup1() {
    stack_watch::paint(1);
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
SharedFlag g_trace_bus{false};  // console `t`
SharedFlag g_fast_lane{true};   // console `F` (debug)
SharedFlag g_early{false};       // HDDSYNTH.INI early_answers: ide_bus_early (ide_bus::begin)
SharedFlag g_bus_auto{false};    // bus = auto (docs/auto-bus.md): core 1 switches the program live
SharedFlag g_bus_want_lane{false};  // core 0's advice (auto_bus): the lane, or early answers
std::atomic<uint32_t> g_bus_switches{0};
std::atomic<uint32_t> g_bus_switch_us{0};  // when the last switch was made (core 1)
SharedFlag g_bus_trace_switches{false};     // debug (set it here): the raw trace on for 30 ms at each switch
// dma = auto (with bus = auto): multiword DMA added once the host is learned.
// Core 0 (offerDmaToLearnedHost) writes the patch, then bumps the sequence;
// core 1 applies it to the device between commands.
SharedFlag g_dma_when_learned{false};       // HDDSYNTH.INI dma = auto
SharedFlag g_dma_added{false};              // offerDmaToLearnedHost has run
static IdentifyPatch g_dma_patch[2];        // device 0's and device 1's
static uint8_t g_dma_patch_mode = NO_DMA;
static std::atomic<uint32_t> g_dma_patch_seq{0};
static uint32_t g_dma_patch_applied = 0;    // core 1's
SharedFlag g_table{false};       // HDDSYNTH.INI register_table, or console `P t`: ide_table (docs/register-table.md)
SharedFlag g_isa{false};         // HDDSYNTH.INI isa: iordy_watchdog and IOCS16# (ide_bus::begin)
SharedFlag g_no_lane{false};     // a CD-ROM: ide_bus, never the data lane (ide_bus::begin)
SharedFlag g_iordy{true};        // HDDSYNTH.INI iordy: off leaves IORDY alone (ide_bus::begin)
SharedFlag g_read_ahead{true};  // HDDSYNTH.INI read_ahead; console `A` (debug)
// Off by default: with it on, writes hold some status reads 1.5-2 us (ATA
// allows 1.25), not yet understood. Console `W` turns it on to work on that.
SharedFlag g_write_behind{true};  // HDDSYNTH.INI write_behind; console `W`
bool g_link_on = true;  // HDDSYNTH.INI activity_link; console `L` (core 0 only)

// For the activity LED (core 0's updateLed): core 1 counts commands as they
// start and says whether one is running.
std::atomic<uint32_t> g_command_starts{0};
static SharedFlag g_command_active{false};

// The disk's size, set by core 0 once it has chosen the disk (setup()).
// Core 1 starts first, so it waits for it: IDENTIFY reports the real size.
// The PIO mode to offer (HDDSYNTH.INI's `mode`) is set before it.
std::atomic<uint32_t> g_disk_sectors{0};
std::atomic<uint8_t> g_reset_swap{RESET_SWAP_NONE};
IdentifyInfo g_reset_swap_info{};
uint16_t g_reset_swap_identify[256];
std::atomic<bool> g_disk_chosen{false};  // set after g_disk_sectors: a CD-ROM with no disc has 0
std::atomic<uint8_t> g_pio_mode{0};
std::atomic<uint8_t> g_mdma_mode{NO_DMA};       // set by core 1 at start-up (setup1)
std::atomic<uint8_t> g_mode_asked{0};            // HDDSYNTH.INI's mode, for core 1
SharedFlag g_dmarq_wired{false};                 // found by core 1 at start-up
SharedFlag g_dma_auto{false};                    // HDDSYNTH.INI dma: auto may offer DMA
// Which PIO programs core 1 started (pio_plan.h), for `s` and `S`: written
// once by core 1 at start-up, then g_pio_planned set; core 0 reads it after.
pio_plan::Plan g_pio_plan;
std::atomic<bool> g_pio_planned{false};  // start-up only, so std::atomic is fine (not per access)

// Which bus program the drive would choose for this host (docs/auto-bus.md):
// advice only, printed as AUTOBUS lines and in `s`; nothing switches yet.
// Core 0 only. g_sectors_served counts the data sectors serveFetches() moves.
auto_bus::Advisor g_auto_bus;
bool g_auto_bus_started = false;
uint32_t g_sectors_served = 0;

// HDDSYNTH.INI's geometry for core 1, packed (cylinders << 16 | heads << 8 |
// sectors), or 0 for auto; stored with the disk's size (geometryFor).
std::atomic<uint32_t> g_geometry{0};
// Device 1 (docs/two-drives.md): its disk's size (0: no device 1) and default
// geometry, packed as g_geometry is. Core 0 sets both at boot, before the
// device 0's size, which core 1 waits for.
std::atomic<uint32_t> g_device1_sectors{0};
std::atomic<uint32_t> g_device1_geometry{0};
// A CD-ROM drive (docs/atapi.md): as device 1 beside the disk (`device1 =
// cdrom`: g_device1_sectors is then its disc's, 0 for none), or alone as
// device 0 (`device0 = cdrom`: g_disk_sectors is its disc's). Plain bools:
// core 0 sets them before publishing g_disk_chosen (release), and core 1 reads
// them only after waiting for that (acquire); an atomic's load could be
// called out of line, from flash.
bool g_device0_cdrom = false;
bool g_device1_cdrom = false;
bool g_geometry_trimmed = false;  // the cylinders were cut to fit the disk (CONFIG says so)
const char* g_geometry_from = "auto";  // "set", "partition table" or "auto" (the default), for CONFIG

// (driveIdentity() and device1Identity(), what IDENTIFY says, are in main_state.h.)

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

// (C1_PROFILE and CMD_PROFILE, the profiling switches, are in main_state.h.)
// Console `M`: the CD-ROM's disc changed at run time (docs/atapi.md). Core 0
// asks with a new sequence number and the disc's size (0: no disc); core 1
// changes the device's disc once it's idle (atapi::Device::idle) and echoes
// the number. Core 0 ejects first and swaps files only once core 1 has
// confirmed, so no fetch for one disc is ever served from the next.
std::atomic<uint32_t> g_disc_seq{0};
std::atomic<uint32_t> g_disc_sectors{0};
std::atomic<uint32_t> g_disc_done{0};

#if CMD_PROFILE
volatile uint32_t g_cmd_run_max = 0;   // cycles
volatile uint32_t g_cmd_run_what = 0;  // command | packet << 8
#endif

// Core 1's longest loop passes, by what the pass did (GAP_*, main_state.h).
volatile uint32_t g_c1_gap[GAP_KINDS];
volatile uint32_t g_c1_fifo_cycles, g_c1_fifo_max;  // the FIFO check alone (diagnostic)
volatile uint32_t g_c1_passes;
volatile uint32_t g_c1_iser[2], g_c1_primask;  // core 1's interrupt set-up, captured in setup1
#ifndef HDDPRO_C1_NO_IRQ
#define HDDPRO_C1_NO_IRQ 0  // -DHDDPRO_C1_NO_IRQ=1: core 1 masks all interrupts (a bench test)
#endif
uint32_t g_bench_pio, g_bench_ram, g_bench_sio, g_bench_nop;  // cycles per 1000, measured in setup1
static volatile uint32_t g_bench_var;
uint32_t g_bench_pio_ram, g_bench_nop_ram;  // the same loops, from core 1's RAM code
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
static uint32_t g_switch_quiet_cycles;  // cfg::BUS_SWITCH_QUIET_US likewise (bus = auto)
const char* const GAP_NAMES[GAP_KINDS] = {"-", "access", "lane", "command", "busonly",
                                                  "step0", "step1", "step2", "step3", "step4"};

// The diagnostics the PIO plan has room for (iordy_timer, bus_observer):
// started by core 1 at start-up, and by core 0 again when the PCB's card
// gives back the PIO block it borrowed for a card operation.
// bus = auto with dma = auto (core 0, printAutoBus): the host earned the lane,
// so it isn't an ISA card and multiword DMA can be offered. The DMA program
// takes the block the diagnostics (the PCB) or IOCS16# and the read timing
// probe (the BB48) had (pio_plan), and IDENTIFY says so from the next time
// the host asks: Windows' own driver does, after the BIOS has booted it. The
// advisor keeps the host learned, since the diagnostics' evidence stops.
bool offerDmaToLearnedHost() {
    if (g_dma_added.load() || !g_dmarq_wired.load() || g_mdma_mode.load() != NO_DMA) return false;
    const uint8_t mode = MAX_MDMA_MODE;
    uint16_t without[256], with[256];
    IdentifyInfo info = driveIdentity();
    buildIdentify(info, without);
    info.mdma_mode = mode;
    buildIdentify(info, with);
    if (!identifyDiff(without, with, g_dma_patch[0])) return false;
    info = device1Identity();
    buildIdentify(info, without);
    info.mdma_mode = mode;
    buildIdentify(info, with);
    if (!identifyDiff(without, with, g_dma_patch[1])) return false;

    const pio_plan::Plan was = g_pio_plan;
    const pio_plan::Plan now = pio_plan::choose(sd_card::sdio(), true, false);
    if (was.iordy_timer && !now.iordy_timer) iordy_timer::end();
    if (was.bus_observer && !now.bus_observer) bus_observer::end();
    if (was.iocs16 && !now.iocs16) iocs16::end();
    if (was.bus_timing && !now.bus_timing) bus_timing::end();
    ide_dma::begin();  // core 1 uses it only once the device takes a DMA command
    g_pio_plan = now;
    g_mdma_mode.store(mode, std::memory_order_relaxed);
    g_dma_patch_mode = mode;
    g_dma_added.store(true);
    g_dma_patch_seq.store(1, std::memory_order_release);
    return true;
}

static void startDiagnostics() {
    if (g_pio_plan.iordy_timer) iordy_timer::begin(cfg::IORDY_REPORT_NS);
    if (g_pio_plan.bus_observer) {
        const IdentifyInfo device1 = device1Identity();
        const bool cdrom1 = g_device1_cdrom;  // a CD-ROM, disc or none
        bus_observer::begin(g_disk_sectors.load(std::memory_order_relaxed), driveIdentity(),
                            device1.total_sectors || cdrom1 ? &device1 : nullptr, cdrom1, g_device0_cdrom);
    }
}

// The PCB's card borrowing the diagnostics' PIO block (sd_card::setPioLender):
// core 0, for a console card operation while the test disk is served. Waits
// for core 1 to have shared out the blocks, so the card never races it.
static bool lendDiagnostics() {
    for (uint32_t start = millis(); !g_pio_planned.load(std::memory_order_acquire);)
        if (millis() - start > 2000) return false;
    if (g_pio_plan.table_write) {
        // The register table's write side shares the diagnostics' block with
        // bus_observer and can't stop; the card's block went to IOCS16# and the
        // read timing probe instead (pio_plan::choose), so those lend it.
        iocs16::end();
        bus_timing::end();
        return true;
    }
    if (!g_pio_plan.iordy_timer || !g_pio_plan.bus_observer) return false;  // they don't have a block of their own
    iordy_timer::end();
    bus_observer::end();
    return true;
}
static void reclaimDiagnostics() {
    if (g_pio_plan.table_write) {
        if (g_pio_plan.iocs16) iocs16::begin();
        if (g_pio_plan.bus_timing) bus_timing::begin();
        return;
    }
    startDiagnostics();
}

void setup1() {
    stack_watch::paint(1);  // core 1's stack, for `s`
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;  // core 1's own cycle counter, for g_c1_gap
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
    g_bus_only_cycles = cfg::EARLY_BUS_ONLY_US * (clock_get_hz(clk_sys) / 1000000);
    g_switch_quiet_cycles = cfg::BUS_SWITCH_QUIET_US * (clock_get_hz(clk_sys) / 1000000);
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
    while (!g_disk_chosen.load(std::memory_order_acquire)) tight_loop_contents();  // a CD-ROM with no disc: 0 sectors
    if (g_table.load())
        ide_table::begin();  // every read answered from the PIO's registers (docs/register-table.md)
    else
        ide_bus::begin(g_isa.load(), g_early.load(), g_iordy.load(), !g_no_lane.load());
    busSwitchesOn();
    // READ/WRITE MULTIPLE, before loop1 makes the devices: not on the data
    // lane (docs/performance.md, item 1).
    ata::Device::offerMultiple(g_table.load() || !ide_bus::dataLaneOn());
    // Whether to offer DMA, before anything builds IDENTIFY (driveIdentity).
    g_dmarq_wired.store(dmarqWired());
    // Both builds by the same settings (`mode`, `dma`): the CD-ROM drive moves
    // PACKET's data by DMA when offered (0.30.0-dev.3).
    g_mdma_mode.store(g_table.load() ? NO_DMA  // not with the register table, yet
                                     : drive_config::mdmaModeOffered(drive_config::Mode(g_mode_asked.load(std::memory_order_relaxed)),
                                                                     g_dmarq_wired.load(), g_dma_auto.load()),
                      std::memory_order_relaxed);
    // The other PIO blocks, shared out by what this configuration needs
    // (pio_plan.h): the card's SDIO takes one on the PCB build.
    const bool dma = g_mdma_mode.load(std::memory_order_relaxed) != NO_DMA;
    g_pio_plan = pio_plan::choose(sd_card::sdio(), dma, g_isa.load(), g_table.load());
    startDiagnostics();  // here, so these never race to claim a PIO
    if (g_table.load()) ide_table::attachObserver();
    // Multiword DMA if it's offered, otherwise IOCS16# for ISA cards (which
    // never use DMA).
    if (g_pio_plan.ide_dma) ide_dma::begin();  // after ide_bus: its block's function on DD0-15 is the one to hand back to
    if (g_pio_plan.iocs16) iocs16::begin();
    if (!dma) {
        // DMACK#, as ide_dma::begin() leaves it: an input for console `p`.
        gpio_init(cfg::PIN_DMACK_N);
        gpio_disable_pulls(cfg::PIN_DMACK_N);
    }
    if (g_pio_plan.bus_timing) bus_timing::begin();  // the read timing probe, where there's room
    g_pio_planned.store(true, std::memory_order_release);
    // Core 1 wins bus arbitration, so core 0's USB and other traffic can't
    // delay its PIO accesses while the host waits on a read.
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;
    // Which interrupts core 1 takes (its own NVIC), for `s` (`c1_irq`): a
    // handler running from flash would stall it for microseconds while core 0
    // is busy in flash (the CD-ROM's slow Status polls during a card read).
    g_c1_iser[0] = m33_hw->nvic_iser[0];
    g_c1_iser[1] = m33_hw->nvic_iser[1];
#if HDDPRO_C1_NO_IRQ
    // Bench: none at all from here (PRIMASK). Arduino-Pico's idleOtherCore,
    // which flash writes use, would then wait for good; the drive build never
    // writes flash.
    __asm volatile("cpsid i" ::: "memory");
#endif
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    g_c1_primask = primask;
}


// Core 1's helpers, one copy per device kind: the hard disk alone
// (ata::Device) and with a CD-ROM (hdcd::Device). Each body is an inlined
// template (NAME##T); CORE1_FOR below defines the copies, the hard disk's in
// CORE1_RAM in the order 0.31.0 had them and the CD-ROM's after all of it
// (CORE1_RAM_CD), so the hard disk's code sits where it did (core1_ram.h).
#define CORE1_DECLARE(D)                                    \
    static void serviceDma(D& dev);                         \
    static void postFetch(D& dev, uint32_t& posted_fetch);  \
    static void busyTimers(D& dev);                         \
    static void recordDone(D& dev);                         \
    static void earlyIdle(D& dev);                          \
    static void runCommandEarly(D& dev);                    \
    static void runDrive(D& dev, bool first);
CORE1_DECLARE(ata::Device)
CORE1_DECLARE(hdcd::Device)

// The DMA command's data phase (READ DMA, WRITE DMA): one bookkeeping step.
// A burst that ended goes back to the device; a new one starts when the
// device has a sector (or buffer) for it; and when the command is over, done
// or cut short (RESET#, SRST, a new command, an error), ide_dma tidies up.
template <class Dev>
__attribute__((always_inline)) static inline void serviceDmaT(Dev& dev) {
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
template <class Dev>
__attribute__((always_inline)) static inline void postFetchT(Dev& dev, uint32_t& posted_fetch) {
    uint32_t id = dev.pendingFetchId();
    if (id == 0 || id == posted_fetch) return;  // the common case, without a call
    ata::Fetch fetch;
    if (dev.pendingFetch(fetch) && g_mailbox.post(fetch)) posted_fetch = fetch.id;
}

// BSY for a while: waking from standby, or after a data-out block. Step 3 of
// loop1, and early_answers' tight bus loop runs it too, since the host may
// poll status throughout the hold and the loop never goes quiet to return.
template <class Dev>
__attribute__((always_inline)) static inline void busyTimersT(Dev& dev) {
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

CORE1_RAM static void recordBus(uint32_t sample) { record(TraceKind::Bus, sample); }

// A command's CMD line: the hard disk's, or the CD-ROM build's (with PACKET's opcode).
CORE1_RAM static void recordCommandStart(uint32_t t_us, const ata::CommandInfo& c) {
    record(commandStartEvent(t_us, c.command, c.lba, c.count, 0, c.unit));
}
CORE1_RAM_CD static void recordCommandStart(uint32_t t_us, const hdcd::CommandInfo& c) {
    record(commandStartEvent(t_us, c.command, c.lba, c.count, c.packet, c.unit));
}
[[maybe_unused]] static uint8_t packetOf(const ata::CommandInfo&) { return 0; }
[[maybe_unused]] static uint8_t packetOf(const hdcd::CommandInfo& c) { return c.packet; }

// early_answers: a queued command, run from the tight bus loop (ide_bus::
// serveBusEarly) so core 1 doesn't leave it for one: going back to the main
// loop for each command left the host's next accesses waiting up to a pass
// (0.23.11). Its CMD line is recorded at once; loop1's step 0 records DONE
// from g_early_started and g_early_command_start_us.
static uint32_t g_early_started = 0;
static uint32_t g_early_command_start_us = 0;
// Commands whose DONE line has been recorded (step 0 and the tight loop share
// it). In early mode the tight loop records DONE itself, between accesses,
// as a command completes: left to step 0, which runs only once the bus is
// quiet, a command followed straight by another never got one (the USB
// adapter's back-to-back reads, 0.27.0).
static uint32_t g_done_recorded = 0;

template <class Dev>
__attribute__((always_inline)) static inline void recordDoneT(Dev& dev) {
    if (dev.commandsCompleted() == g_done_recorded) return;
    g_done_recorded = dev.commandsCompleted();
    if (g_done_recorded == dev.commandsStarted()) g_command_active.store(false);
    uint32_t t = time_us_32();
    record(commandEndEvent(t, dev.lastCommand().command, dev.status(), dev.error(), t - g_early_command_start_us));
}

// The tight loop's idle work: the BSY timers, DONE for a command just
// completed, and a DMA command's bursts. In DMA the host waits for DMARQ
// instead of polling, so the bus goes quiet and the main loop's step 1 came
// only after EARLY_BUS_ONLY_US: a 64 KB READ DMA took 12 s (0.30.0-dev.3).
template <class Dev>
__attribute__((always_inline)) static inline void earlyIdleT(Dev& dev) {
    busyTimers(dev);
    recordDone(dev);
    if (dev.dmaCommand() || ide_dma::active()) serviceDma(dev);
}

// CMD_PROFILE's bookkeeping: `start` is when the command's handling began.
template <class Dev>
CORE1_RAM static inline void noteCommandTime(Dev& dev, uint32_t start) {
#if CMD_PROFILE
    uint32_t t = m33_hw->dwt_cyccnt - start;
    if (t > g_cmd_run_max) {
        g_cmd_run_max = t;
        const auto& c = dev.lastCommand();
        g_cmd_run_what = uint32_t(c.command) | uint32_t(packetOf(c)) << 8;
    }
#else
    (void)dev;
    (void)start;
#endif
}

template <class Dev>
__attribute__((always_inline)) static inline void runCommandEarlyT(Dev& dev) {
#if CMD_PROFILE
    const uint32_t profile_start = m33_hw->dwt_cyccnt;
#endif
    recordDone(dev);  // the last command's DONE before this one's CMD
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
#if CMD_PROFILE
    noteCommandTime(dev, profile_start);
#endif
}

// Console `V`: the drive goes silent, as if unplugged, while core 0 holds
// g_absent. The adapter's next command fails, it resets and probes, and the
// drive is detected again once it's back: a replug without hands on the rig.
// It comes back as from a power cycle, with the device reset. Runs once per
// absence, never per access, so it can stay in flash (one of
// check_core1_ram.py's one-offs).
SharedFlag g_absent{false};

template <class Dev>
__attribute__((noinline)) static void stayAbsent(Dev& dev) {
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

// Core 1's loop for one kind of device (drive_device.h): the hard disk alone,
// or with a CD-ROM. Returns only when bus = auto has switched the bus program
// (docs/auto-bus.md): loop1 then enters it again with `first` false, so the
// device carries on as it was rather than being reset. The bus comes first: every access waiting
// in the FIFO is served back to back, and only when it is empty does core 1
// do one small bookkeeping step before looking again. So a read waits for one
// step at most; iordy_timer.pio measures how long the host actually waits.
// bus = auto's step in core 1's loop (the hard disk only), out of line and out
// of core 1's hot block (CORE1_RAM_RARE): inlined, it grew runDrive and the data
// lane's writes slowed (0.35.0-dev.6).
//  - dma = auto: multiword DMA added (offerDmaToLearnedHost), between commands;
//  - the program core 0 advises, switched while RESET# is held (back to early
//    answers, the bus idle by definition) or at a quiet moment: no command,
//    not BSY or DRQ, no fetch out, no access for a while.
// Its quiet spell is timed here, from what the bus shows each time it runs
// (ide_bus::busActive), not per access in runDrive: there the flag and timer
// cost runDrive registers (`early` went to the stack) and the lane's writes
// slowed 1.7%. Missing a short access only moves the moment; the switch
// itself goes ahead only between accesses. True if it switched.
static uint32_t g_auto_last_busy = 0;  // core 1's cycle counter when the bus last looked busy
CORE1_RAM_RARE __attribute__((noinline)) static bool busAutoStep(ata::Device& dev, bool reset_low, bool data_lane) {
    const uint32_t now = m33_hw->dwt_cyccnt;
    if (ide_bus::busActive()) g_auto_last_busy = now;
    const bool idle = !dev.commandQueued() && !g_command_active.load() && !(dev.status() & (ata::ST_BSY | ata::ST_DRQ));
    const uint32_t seq = g_dma_patch_seq.load(std::memory_order_relaxed);
    if (seq != g_dma_patch_applied && idle) {
        std::atomic_thread_fence(std::memory_order_acquire);  // the patch core 0 wrote before seq
        dev.offerDma(g_dma_patch_mode, g_dma_patch[0], g_dma_patch[1]);
        g_dma_patch_applied = seq;
    }
    if (!idle) g_auto_last_busy = now;
    const bool want_lane = g_bus_want_lane.load() && !reset_low;
    if (want_lane == data_lane) return false;
    if (!reset_low && !(idle && !dev.pendingFetchId() && now - g_auto_last_busy >= g_switch_quiet_cycles))
        return false;
    if (!ide_bus::switchProgram(!want_lane)) {  // an access under way: after the next quiet spell
        g_auto_last_busy = now;
        return false;
    }
    if (g_bus_trace_switches.load()) g_trace_bus.store(true);  // debug: the accesses around it
    g_bus_switch_us.store(time_us_32(), std::memory_order_relaxed);
    g_early.store(!want_lane);
    ide_bus::updateIntrq(dev);
    g_bus_switches.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// A new hard disk at the host's reset (main_state.h, ResetSwap), out of core
// 1's hot block. At RESET# asserted, an armed swap is asked for; at its
// release, true while core 0 is still opening the image: the device stays in
// reset (BSY) until it's done.
static bool s_reset_swap_held = false;

// The new disk into the device: its size, geometry and IDENTIFY, as core 0 left them.
CORE1_RAM_RARE static void resetSwapTake(ata::Device& dev) {
    dev.setDisk(g_reset_swap_info, g_reset_swap_identify);
    g_reset_swap.store(RESET_SWAP_NONE, std::memory_order_release);
}

// At each RESET# edge: true to stay in reset (BSY) after its release.
CORE1_RAM_RARE __attribute__((noinline)) static bool resetSwapEdge(ata::Device& dev, bool low) {
    const uint8_t state = g_reset_swap.load(std::memory_order_acquire);
    s_reset_swap_held = false;
    if (low) {
        if (state == RESET_SWAP_ARMED) g_reset_swap.store(RESET_SWAP_REQUESTED, std::memory_order_release);
        return false;
    }
    if (state == RESET_SWAP_DONE) resetSwapTake(dev);  // opened while RESET# was held
    s_reset_swap_held = state == RESET_SWAP_REQUESTED;
    return s_reset_swap_held;
}

// Held in reset after RESET#'s release: out of it once core 0 has the new disk open.
CORE1_RAM_RARE __attribute__((noinline)) static void resetSwapFinish(ata::Device& dev) {
    if (g_reset_swap.load(std::memory_order_acquire) != RESET_SWAP_DONE) return;
    s_reset_swap_held = false;
    resetSwapTake(dev);
    dev.hardReset(false);
    ide_bus::restart();
    iordy_timer::requestReset();
    ide_bus::updateIntrq(dev);
}

template <class Dev>
__attribute__((always_inline)) static inline void runDriveT(Dev& dev, bool first) {
    uint32_t started = dev.commandsStarted();
    g_done_recorded = dev.commandsCompleted();
    uint32_t command_start_us = 0;
    std::decay_t<decltype(dev.lastCommand())> start_info{};  // the command whose CMD line is still to record
    bool start_unrecorded = false;
    bool quiet = false;  // a command just started: bookkeeping waits (COMMAND_QUIET_US)
    uint32_t posted_fetch = 0;
    bool reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
    if (first) dev.hardReset(reset_low);  // a switched program carries on with the device as it was
    const bool early = g_early.load();  // ide_bus_early's samples (early_answers)
    const bool data_lane = ide_bus::dataLaneOn();  // data reads never reach core 1 (docs/data-lane.md)
    unsigned served_with_command = 0;  // accesses served since a command was queued
    uint32_t last_access_cycles = 0;  // core 1's cycle counter: core-local, unlike the timer behind the APB bridge
    const uint32_t bus_only_cycles = g_bus_only_cycles;
    ide_bus::updateIntrq(dev);  // driven low from the start: an ISA card's line would float high
    unsigned step = 0;

    uint32_t last_pass = m33_hw->dwt_cyccnt;
    uint8_t pass_kind = 0;
    for (;;) {
        uint32_t sample;
        bool got;
        if constexpr (C1_PROFILE) {
            uint32_t now = m33_hw->dwt_cyccnt;
            if (now - last_pass > g_c1_gap[pass_kind]) g_c1_gap[pass_kind] = now - last_pass;
            last_pass = now;
            g_c1_passes = g_c1_passes + 1;
            pass_kind = GAP_ACCESS;
            uint32_t f0 = m33_hw->dwt_cyccnt;
            got = ide_bus::serviceOnce(dev, sample);
            uint32_t f = m33_hw->dwt_cyccnt - f0;
            if (!got) {
                g_c1_fifo_cycles = g_c1_fifo_cycles + f;
                if (f > g_c1_fifo_max) g_c1_fifo_max = f;
            }
        } else {
            got = ide_bus::serviceOnce(dev, sample);
        }
        if (got) {
            if (g_trace_bus.load()) record(TraceKind::Bus, sample);
            if (early) {
                last_access_cycles = m33_hw->dwt_cyccnt;  // early answers' bus-only spell
                ide_bus::serveBusEarly(dev, reset_low, g_mailbox, posted_fetch, bus_only_cycles, static_cast<void (*)(Dev&)>(earlyIdle),
                                       static_cast<void (*)(Dev&)>(runCommandEarly), g_trace_bus.load() ? recordBus : nullptr);
            }
            // Back to the bus, but with a command queued only for a few accesses:
            // the host polls STATUS straight after a command and gets BSY at once,
            // and if the polls came back to back, before core 1 found the FIFO
            // empty, the command would otherwise never run (STATUS at BSY for good:
            // the data lane, 0.29.0-2d.4). Running it at once instead held the
            // first poll for the whole command (IDENTIFY: 3 us).
            if (!dev.commandQueued()) continue;
            if (++served_with_command < cfg::COMMAND_POLLS_FIRST) continue;  // cleared when it runs
        }
        if (early) ide_bus::checkIordyStall();  // IORDY held with no strobe: let it go
        pass_kind = GAP_LANE;
        // Mid-block, the fast lane answers data reads with nothing else in
        // between. Not while the raw trace or the debug delay is on.
        const uint16_t* next;
        uint16_t left;
        if (data_lane) {
            // The data lane serves data reads itself: while a block is on it,
            // wait for its end there (posting the read-ahead and taking its
            // result), so the next sector is ready when the host's status read
            // comes. A flag test otherwise: this runs on every idle pass.
            if (ide_bus::lane_busy && ide_bus::serveLane(dev, reset_low, g_mailbox, posted_fetch)) continue;
        } else if (dev.dataInRemaining(next, left)) {
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
            served_with_command = 0;
#if CMD_PROFILE
            const uint32_t profile_start = m33_hw->dwt_cyccnt;
#endif
            dev.setReadAhead(g_read_ahead.load());
            dev.setWriteBehind(g_write_behind.load());
            access_log::note(access_log::COMMAND, 0);
            dev.runQueuedCommand();
            ide_bus::syncDataLane(dev);  // IDENTIFY's block onto the lane, then INTRQ
            access_log::note(access_log::COMMAND, 1);
#if CMD_PROFILE
            noteCommandTime(dev, profile_start);
#endif
            // early_answers: straight back to the tight bus loop (status polls, the
            // data block); the bookkeeping below can wait until the bus is quiet.
            if (early)
                ide_bus::serveBusEarly(dev, reset_low, g_mailbox, posted_fetch, bus_only_cycles, static_cast<void (*)(Dev&)>(earlyIdle),
                                       static_cast<void (*)(Dev&)>(runCommandEarly), g_trace_bus.load() ? recordBus : nullptr);
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
        [[maybe_unused]] const uint32_t step_start = access_log::ON ? access_log::now() : 0;
        switch (step) {
            case 0:  // the trace's CMD and DONE lines, in that order, at most one per pass
                if (early && g_early_started != 0 && g_early_started != started) {
                    started = g_early_started;  // commands the tight loop ran, CMD already recorded
                    command_start_us = g_early_command_start_us;
                }
                if (start_unrecorded) {
                    recordCommandStart(command_start_us, start_info);
                    start_unrecorded = false;
                } else if (dev.commandsCompleted() != g_done_recorded) {
                    g_done_recorded = dev.commandsCompleted();
                    if (g_done_recorded == started) g_command_active.store(false);
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
                    dev.fetchDone(result.id, result.ok, result.failed);
                    ide_bus::syncDataLane(dev);  // a sector's block onto the lane, then INTRQ
                }
                break;
            }
            case 3:  // BSY for a while: waking from standby, or after a data-out block
                busyTimers(dev);
                if constexpr (std::is_same_v<Dev, hdcd::Device>) {  // a disc change from the console, between commands
                    uint32_t seq = g_disc_seq.load(std::memory_order_acquire);
                    if (seq != g_disc_done.load(std::memory_order_relaxed) && dev.discIdle()) {
                        dev.changeDisc(g_disc_sectors.load(std::memory_order_relaxed));
                        g_disc_done.store(seq, std::memory_order_release);
                    }
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
                    bool hold = false;  // a new disk still opening: stay in reset (BSY)
                    if constexpr (std::is_same_v<Dev, ata::Device>) hold = resetSwapEdge(dev, low);
                    if (!hold) {
                        dev.hardReset(low);
                        if (!low) {
                            ide_bus::restart();  // drop anything left from the bus going dead
                            // Time only real traffic: while the host powers up, its
                            // strobes float low, which looks like one long read.
                            iordy_timer::requestReset();
                        }
                        ide_bus::updateIntrq(dev);
                    }
                    record(low ? TraceKind::ResetAssert : TraceKind::ResetRelease, pins);
                }
                if constexpr (std::is_same_v<Dev, ata::Device>) {
                    if (s_reset_swap_held) {
                        resetSwapFinish(dev);  // the new disk open: out of reset with it
                        // runDrive 0.35.0's size plus 32 bytes, a whole RAM bank
                        // period, so the code after it keeps its banks
                        __asm__ volatile("nop\n nop\n nop\n nop");
                    }
                }
                // bus = auto (docs/auto-bus.md, the hard disk only), out of line:
                // one test here when it's off
                if constexpr (std::is_same_v<Dev, ata::Device>) {
                    if (g_bus_auto.load() && busAutoStep(dev, reset_low, data_lane))
                        return;  // switched: loop1 enters again with the new program
                }
                break;
            }
        }
        access_log::step(step, step_start);
        step = step == 4 ? 0 : step + 1;
        // early_answers: back to the tight bus loop after every pass, so a quiet
        // spell's first access finds core 1 polling, not mid-pass (660 ns).
        if (early)
            ide_bus::serveBusEarly(dev, reset_low, g_mailbox, posted_fetch, bus_only_cycles, static_cast<void (*)(Dev&)>(earlyIdle),
                                   static_cast<void (*)(Dev&)>(runCommandEarly), g_trace_bus.load() ? recordBus : nullptr);
    }
}


#define CORE1_FOR(D, RAM)                                                                     \
    RAM static void recordDone(D& dev) { recordDoneT(dev); }                                    \
    RAM static void runCommandEarly(D& dev) { runCommandEarlyT(dev); }                          \
    RAM static void serviceDma(D& dev) { serviceDmaT(dev); }                                    \
    RAM static void busyTimers(D& dev) { busyTimersT(dev); }                                    \
    RAM static void earlyIdle(D& dev) { earlyIdleT(dev); }                                      \
    RAM static void postFetch(D& dev, uint32_t& posted_fetch) { postFetchT(dev, posted_fetch); } \
    RAM __attribute__((noinline)) static void runDrive(D& dev, bool first) { runDriveT(dev, first); }
CORE1_FOR(ata::Device, CORE1_RAM)
CORE1_FOR(hdcd::Device, CORE1_RAM_CD)

// Core 1's loop with the register table (console `P t`, docs/register-table.md):
// no read reaches core 1, so instead of serving accesses it keeps the PIO's
// register image in step with the device. Writes come from W's ring, Status
// reads (which acknowledge the interrupt) from the observer's; data blocks
// move by DMA, started before the device's DRQ is published and caught up
// once the block is done. The hard disk only. Never returns.
template <class Dev>
__attribute__((always_inline)) static inline ide_table::Image tableImage(Dev& dev) {
    constexpr uint32_t DRIVE = ide_table::DRIVE;
    ide_table::Image m;
    m.error = DRIVE | dev.peek(ata::Reg::ErrorFeatures);
    m.sector_count = DRIVE | dev.peek(ata::Reg::SectorCount);
    m.lba_low = DRIVE | dev.peek(ata::Reg::LbaLow);
    m.lba_mid = DRIVE | dev.peek(ata::Reg::LbaMid);
    m.lba_high = DRIVE | dev.peek(ata::Reg::LbaHigh);
    m.device = DRIVE | dev.peek(ata::Reg::Device);
    m.status = DRIVE | dev.peek(ata::Reg::StatusCommand);
    return m;
}

// INTRQ with the register table: R1 lowers it as it answers a Status read, in
// time for a host that checks it straight after (the Innostor bridge waited for
// it); core 1 sets it only when the device's interrupt changes, so a Status read
// it hasn't yet seen doesn't raise it again.
// The level core 1 last set (bit 0 asserted, bit 1 driven), or -1. C lowers
// INTRQ itself at a Command write, so taking one clears bit 0 here: with data
// ready at once, the interrupt went off and on again within one pass, and
// INTRQ was never raised again (the JMicron adapter then waited for good).
static int8_t g_table_intrq = -1;

template <class Dev>
__attribute__((always_inline)) static inline void tableIntrq(Dev& dev) {
    int8_t& shown = g_table_intrq;
    int8_t now = int8_t((dev.intrq() ? 1 : 0) | (dev.intrqDriven() ? 2 : 0));
    if (now == shown) return;
    shown = now;
    ide_table::setIntrq(now & 1, now & 2);
}

template <class Dev>
__attribute__((always_inline)) static inline void runTableT(Dev& dev) {
    g_done_recorded = dev.commandsCompleted();
    uint32_t posted_fetch = 0;
    bool reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
    dev.hardReset(reset_low);
    bool dirty = true;
    uint32_t shown_status = ~0u;
    // Interrupts: the device's events (a command done, a block ready or wanted)
    // raise INTRQ, and only a Status read after the raise acknowledges it (the
    // observer's ring lags; a Status read for the last block mustn't clear the
    // next block's interrupt). irq_events counts what could raise one.
    uint32_t irq_raised_at = 0;
    uint32_t irq_events = 0, irq_events_shown = ~0u;
    for (;;) {
        // Writes, in order. A Command write's busy came from C's DMA chain;
        // queueCommand makes the device busy too, so C1 is armed again.
        uint32_t sample;
        while (ide_table::popWrite(sample)) {
            bus::Block block = bus::block(sample);
            ata::Reg reg = ata::regFor(block == bus::Block::Command, block == bus::Block::Control, bus::address(sample));
            if (reg == ata::Reg::Data || reg == ata::Reg::None) continue;  // WD has the data words
            if (reg == ata::Reg::StatusCommand) {
                dev.queueCommand(uint8_t(bus::data(sample)));
                ide_table::armCommandBusy();
                if (g_table_intrq > 0) g_table_intrq = int8_t(g_table_intrq & ~1);  // C lowered INTRQ
            } else {
                dev.write(reg, bus::data(sample));
            }
            dirty = true;
        }
        if (ide_table::statusReadSince(irq_raised_at) && dev.intrq()) {
            dev.consumeRead(ata::Reg::StatusCommand);  // acknowledges the interrupt raised at irq_raised_at
            dirty = true;
        }
        if (dev.commandQueued()) {
            if (ide_table::dataInActive()) ide_table::stopDataIn();  // a new command abandons a block
            if (ide_table::dataOutActive()) ide_table::stopDataOut();
            runCommandEarly(dev);  // its CMD line, and it runs
            dirty = true;
        }

        // Core 0's side, and the timers
        postFetch(dev, posted_fetch);
        FetchResult result;
        if (g_mailbox.takeResult(result)) {
            dev.fetchDone(result.id, result.ok, result.failed);
            dirty = true;
        }
        busyTimers(dev);
        recordDone(dev);

        if (g_absent.load()) {  // console V: off the bus, as if unplugged, until it's lifted
            ide_table::stopDataIn();
            ide_table::stopDataOut();
            dev.hardReset(true);
            ide_table::stop();
            while (g_absent.load()) {
            }
            dev.hardReset(false);
            ide_table::restart();
            iordy_timer::requestReset();
            g_command_active.store(false);
            reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
            if (reset_low) dev.hardReset(true);  // back in the middle of a reset (as runDriveT)
            dirty = true;
        }
        uint32_t pins = pinsAsSample();
        bool low = !bus::high(pins, cfg::PIN_RESET_N);
        if (low != reset_low) {
            reset_low = low;
            ide_table::stopDataIn();
            ide_table::stopDataOut();
            dev.hardReset(low);
            if (!low) iordy_timer::requestReset();
            record(low ? TraceKind::ResetAssert : TraceKind::ResetRelease, pins);
            dirty = true;
        }

        // Data blocks, after everything that can start one (a command, a fetch, a
        // timer) and before publishing: the host must not see DRQ before its block
        // is on its way (0.32.3-dev: a sector's first five reads came before it).
        // Data in: a block streams by DMA; once its busy write has fired the host
        // has read it all, and the device moves on.
        const uint16_t* next;
        uint16_t left;
        if (ide_table::dataInActive()) {
            if (ide_table::dataInDone()) {
                ide_table::stopDataIn();
                if (dev.dataInRemaining(next, left)) dev.consumeDataWords(left);
                dirty = true;
            } else if (!dev.dataInRemaining(next, left)) {  // the device dropped it (SRST, a reset)
                ide_table::stopDataIn();
                dirty = true;
            }
        }
        if (!ide_table::dataInActive() && dev.dataInRemaining(next, left)) {
            ide_table::startDataIn(next, left);  // before the host can see DRQ
            dirty = true;
        }

        // Data out: WD's channel fills the buffer, then the device takes the block.
        uint16_t* dest;
        if (ide_table::dataOutActive()) {
            if (ide_table::dataOutDone()) {
                ide_table::stopDataOut();
                if (dev.dataOutRemaining(dest, left)) dev.consumeDataOut(left);
                dirty = true;
            } else if (!dev.dataOutRemaining(dest, left)) {
                ide_table::stopDataOut();
                dirty = true;
            }
        }
        if (!ide_table::dataOutActive() && dev.dataOutRemaining(dest, left)) {
            ide_table::startDataOut(dest, left);
            dirty = true;
        }

        irq_events = dev.commandsCompleted() + dev.commandsStarted() + ide_table::blocksIn() + ide_table::blocksOut() +
                     (ide_table::dataInActive() ? 1u : 0u) + (ide_table::dataOutActive() ? 1u : 0u) * 2u;
        // The registers as the device now has them, before anything else is read
        if (dirty || dev.status() != shown_status || irq_events != irq_events_shown) {
            ide_table::publish(tableImage(dev));
            shown_status = dev.status();
            if (irq_events != irq_events_shown && dev.intrq()) {  // a new interrupt: raise it, whatever was shown
                irq_events_shown = irq_events;
                irq_raised_at = ide_table::observerPosition();
                g_table_intrq = -1;
            }
            irq_events_shown = irq_events;
            tableIntrq(dev);
            dirty = false;
        }
    }
}
CORE1_RAM_TABLE __attribute__((noinline)) static void runTable(ata::Device& dev) { runTableT(dev); }

// The device, constructed by loop1 in storage both kinds share, so RAM holds
// only the one that runs.
alignas(hdcd::Device) static unsigned char g_device_mem[sizeof(hdcd::Device)];
static_assert(sizeof(hdcd::Device) >= sizeof(ata::Device) && alignof(hdcd::Device) >= alignof(ata::Device));

// Picks core 1's loop once, by HDDSYNTH.INI: the hard disk alone (with a second
// disk or folder as device 1, or none) runs ata::Device's, the code bb48_drive
// ran up to 0.31.0, with nothing per access for a CD-ROM; with a CD-ROM as
// device 0 or 1, hdcd::Device's (bb48_hdcd's and bb48_cdrom's). Measured: the
// combined device alone cost PIO writes 20-27% (docs/one-build-per-board.md).
CORE1_RAM void loop1() {
    const IdentifyInfo device0 = driveIdentity();
    const IdentifyInfo device1 = device1Identity();
    const bool cdrom0 = g_device0_cdrom, cdrom1 = g_device1_cdrom;
    if (g_table.load() && !cdrom0 && !cdrom1)  // the register table: the hard disk only
        runTable(*new (g_device_mem) ata::Device(device0, device1.total_sectors ? &device1 : nullptr));
    if (cdrom0 || cdrom1) {  // a CD-ROM alone as device 0 (core 0 leaves device 1 out), or as device 1
        auto& dev = *new (g_device_mem) hdcd::Device(device0, nullptr, cdrom0 ? &device0 : &device1, cdrom0 ? 0 : 1);
        for (bool first = true;; first = false) runDrive(dev, first);
    }
    auto& dev = *new (g_device_mem) ata::Device(device0, device1.total_sectors ? &device1 : nullptr);
    for (bool first = true;; first = false) runDrive(dev, first);  // again after a bus = auto switch
}

// The BB48's LED as the drive's activity light, like a hard disk's: lit while
// a command runs, and for at least LED_MIN_MS after any command starts. The
// activity link sends the same busy flag.
bool g_busy = false;

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

void setAbsent(bool on, const char* why) {
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

// The last command started, of any kind and on either device, for the STUCK
// line (checkStuck): a CD-ROM's READ(10) is a PACKET, not a media command.
static struct {
    uint8_t command = 0, packet = 0, unit = 0;
    uint32_t lba = 0, count = 0;
} g_last_started;

// On an SD image the bus observer checks IDENTIFY only around detection,
// which comes about 2.6 s after RESET# is released: for this long after each
// release, and after boot (bus_observer::poll says why).
constexpr uint32_t IDENTIFY_CHECK_MS = 10000;
static uint32_t g_identify_check_until_ms = 30000;

// What kind of host bus the drive is on, as evidence for choosing a bus
// program by itself one day: an ISA card passes every I/O read the PC makes
// to the cable as DIOR# with no chip select (the ICL 386: 19.9 million in a
// session), where a PCI controller or a USB adapter strobes only for the
// drive. The bus observer counts them as DMA words, which they can only be
// when the drive offers DMA. Counted from each RESET# release (or boot).
uint32_t g_release_ms = 0;
uint64_t g_no_cs_at_release = 0;
bool g_no_cs_reported = false;
uint32_t g_no_cs_seen_ms = 0;  // when the first was seen (| 1), until its line is printed

// The last few commands, for the NOIORDY line: which one an unwaited read
// fell in or followed (iordy_timer reports those separately).
CommandHistory g_commands;

static void noteActivity(const TraceEvent& e) {
    g_commands.note(e);
    if (e.kind == TraceKind::CommandStart) {
        g_last_started.command = uint8_t(e.extra);
        g_last_started.packet = uint8_t(e.extra >> 24);
        g_last_started.unit = e.unit;
        g_last_started.lba = e.pins;
        g_last_started.count = commandCount(e);
    }
    if (e.kind == TraceKind::ResetRelease) {
        g_identify_check_until_ms = millis() + IDENTIFY_CHECK_MS;
        g_release_ms = millis();
        g_no_cs_at_release = bus_observer::dmaWords();
        g_no_cs_reported = false;
        g_no_cs_seen_ms = 0;
        if (g_auto_bus_started) g_auto_bus.reset(autoBusCounters(), millis());
    }
    uint8_t command = uint8_t(e.extra);
    if (!activity::isMediaCommand(command)) return;
    if (e.kind == TraceKind::CommandStart) {
        activity_link::Op op = activity_link::opOf(command);
        bool seek_only = (command >= 0x10 && command <= 0x1F) || (command >= 0x70 && command <= 0x7F);
        if (op != activity_link::Op::None)
            g_link_op = op;  // its head target goes from serveFetches(), with its first sector
        else
            postHeadTarget(e.pins, seek_only ? 0 : commandCount(e), op, time_us_64());  // verify or seek
        g_activity.onCommand(e.time_us, e.pins, seek_only ? 0 : commandCount(e));
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

CORE0_RAM static void releaseHeld() {
    if (!g_held_count) return;
    uint64_t now = time_us_64();
    while (g_held_count && g_held[g_held_first].ready_us <= now) {
        while (!g_mailbox.postResult(g_held[g_held_first].result)) {}
        g_held_first = (g_held_first + 1) % HELD_MAX;
        --g_held_count;
    }
}

// SynthMinder's 8Dh (bay_service.cpp), out of serveFetches and in flash: rare,
// and serveFetches runs for every sector (CORE0_RAM; its size moves what
// follows it in RAM).
__attribute__((noinline, cold)) static void collectSynthMinderReply() {
    uint32_t waited;  // its RECEIVE, once the synth has answered
    if (g_held_count < HELD_MAX && synthMinderReady(waited)) {
        g_held[(g_held_first + g_held_count) % HELD_MAX] = HeldResult{FetchResult{waited, true, 0}, 0};
        ++g_held_count;
    }
}

// Its fields as values, not the fetch by reference: taking the fetch's address
// kept it in memory all through serveFetches' loop, 64 bytes more code.
__attribute__((noinline, cold)) static void serveSynthMinderFetch(uint32_t id, uint16_t* buffer, bool store,
                                                                  uint32_t command_lba) {
    ata::Fetch fetch{};
    fetch.id = id;
    fetch.buffer = buffer;
    fetch.store = store;
    fetch.command_lba = command_lba;
    bool ok;
    if (synthMinderFetch(fetch, ok)) {
        g_held[(g_held_first + g_held_count) % HELD_MAX] = HeldResult{FetchResult{fetch.id, ok, 0}, 0};
        ++g_held_count;
        releaseHeld();
    }
}

CORE0_RAM static void serveFetches() {
    if (__builtin_expect(g_synthminder_waiting, 0)) {
        collectSynthMinderReply();
        // serveFetches 0.35.0's size plus 64 bytes, whole RAM bank periods, so
        // core 0's per-sector code after it keeps its banks (check_core1_layout.py)
        __asm__ volatile("nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop");
    }
    releaseHeld();
    ata::Fetch fetch;
    while (g_held_count < HELD_MAX && g_mailbox.takeRequest(fetch)) {
        if (__builtin_expect(ata::isMessage(fetch), 0)) {  // SynthMinder's: no disk, no seek timing
            serveSynthMinderFetch(fetch.id, fetch.buffer, fetch.store, fetch.command_lba);
            continue;
        }
        uint64_t now = time_us_64();  // the command's time starts before the card's
        BlockSource* disk = fetch.unit ? g_device1_disk : g_disk;  // device 1's, or device 0's
        // A fetch moves `sectors` of them (one, or READ/WRITE MULTIPLE's block),
        // stopping at the first that fails. FLUSH CACHE alone moves nothing.
        const bool moves = fetch.store || !fetch.flush;
        bool ok = disk != nullptr;
        uint8_t failed = 0;
        uint64_t ready = now;
        for (uint8_t i = 0; ok && moves && i < fetch.sectors; ++i) {
            const uint32_t lba = fetch.lba + i;
            uint16_t* words = fetch.buffer + size_t(i) * ata::SECTOR_WORDS;
            ok = fetch.store ? disk->write(lba, words) : disk->read(lba, words);
            if (!ok) {
                failed = i;
                break;
            }
            if (!fetch.store) bus_observer::expect(lba, words);  // before core 1 can send it
            ++g_sectors_served;  // auto_bus: past detection?
            // Seek timing sees each sector, as it did one fetch at a time: the
            // block is ready when its last sector would be.
            seek_timing::Release r = g_seek.onSector(now, fetch.command_lba, lba, fetch.store, fetch.command_sectors);
            ready = r.ready_us;
            // The head's moves to the synth: a cached write or a read from the
            // cache moves nothing; a run of cached writes written out does.
            if (r.flushed) postHeadTarget(r.flush_lba, r.flush_sectors, activity_link::Op::Write, r.flush_at_us);
            if (r.first && r.moves)
                postHeadTarget(fetch.command_lba, fetch.command_sectors,
                               fetch.store ? activity_link::Op::Write : activity_link::Op::Read, r.seek_start_us);
        }
        if (ok && fetch.flush) {
            ok = disk->flush();
            // FLUSH CACHE: done once the modelled drive's cache is written out
            seek_timing::Release f{};
            uint64_t done = g_seek.flushAll(now, &f);
            if (done > ready) ready = done;
            if (f.flushed) postHeadTarget(f.flush_lba, f.flush_sectors, activity_link::Op::Write, f.flush_at_us);
        }
        g_held[(g_held_first + g_held_count) % HELD_MAX] = HeldResult{FetchResult{fetch.id, ok, failed}, ready};
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
    // The command named is the last one started, on either device: what
    // core 1 is running, unless its CMD event is still in the trace ring.
    const auto& c = g_last_started;
    Serial.printf("STUCK cmd=%02x%s%s lba=%lu count=%lu%s busy_ms=%lu fetches=%lu last_fetch=%lu lba=%lu %s ok=%d "
                  "ago_ms=%lu results_waiting=%u\n",
                  c.command, c.command == 0xA0 ? " " : "", c.command == 0xA0 ? mmc::name(c.packet) : "",
                  (unsigned long)c.lba, (unsigned long)c.count, c.unit ? " d1" : "",
                  (unsigned long)(now - since_ms), (unsigned long)g_last_fetch.count, (unsigned long)g_last_fetch.id,
                  (unsigned long)g_last_fetch.lba, g_last_fetch.store ? "store" : "read", (int)g_last_fetch.ok,
                  (unsigned long)(now - g_last_fetch.at_ms), unsigned(g_mailbox.resultsWaiting()));
}

#endif

// ---- Core 0 ---------------------------------------------------------------------

static TraceCoalescer g_coalescer;
static uint32_t g_last_event_ms = 0;
static uint32_t g_reported_drops = 0;

static void peripheralClockFromSystem() {
    clock_configure_undivided(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                              clock_get_hz(clk_sys));
}

void setup() {
    stack_watch::paint(0);  // core 0's stack, for `s`
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
#if FIRMWARE_SLICE != 0
    sd_card::setPioLender(lendDiagnostics, reclaimDiagnostics);  // before chooseDisk shares out the blocks
#endif
    chooseDisk();
    beginSeekTiming();
#endif
#ifdef RIG_WATCHDOG_MS
    // Rig only, for unattended work (PLATFORMIO_BUILD_FLAGS=-DRIG_WATCHDOG_MS=8000):
    // if core 0 halts (a hard fault, an abort), the board restarts and can be
    // flashed over USB again, with no replug. Not in releases: console K and
    // I, which make an image, block core 0 for longer than this.
    watchdog_enable(RIG_WATCHDOG_MS, true);
#endif
    printReady();
}

void loop() {
#ifdef RIG_WATCHDOG_MS
    watchdog_update();
#endif
#if FIRMWARE_SLICE != 0
    serveFetches();
    serviceDiscChange();
    serviceResetSwap();
    iordy_timer::poll();
    bus_timing::poll();
    bus_timing::printPending(2);
    printSlowReads();
    printAccessLog();
    printIgnoredIordy();
    printNoChipSelect();
    printAutoBus();
    // Data sectors only on the generated disk; IDENTIFY there, and around detection.
    bus_observer::poll(Serial, g_test_disk, int32_t(g_identify_check_until_ms - millis()) > 0, serveFetches);
    updateLed();
    serviceLoopback();
    serviceLinkRx();
    serviceAbsence();
    checkStuck();
    if (sd_card::wantsSdioRestart()) {
        Serial.println("SD: a card was put back; restarting to run it over SDIO");
        Serial.flush();
        delay(50);
        restart();  // keeps `J` or `T`
    }
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
        // Drops come when serial can't keep up: the line waits for room (and counts on).
        if (printfLine("DROP %lu\n", (unsigned long)(drops - g_reported_drops))) g_reported_drops = drops;
    }
}
