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
static SharedFlag g_table{false};       // console `P t`: the register table (ide_table, docs/register-table.md)
static SharedFlag g_isa{false};         // HDDSYNTH.INI isa: iordy_watchdog and IOCS16# (ide_bus::begin)
static SharedFlag g_no_lane{false};     // a CD-ROM: ide_bus, never the data lane (ide_bus::begin)
static SharedFlag g_iordy{true};        // HDDSYNTH.INI iordy: off leaves IORDY alone (ide_bus::begin)
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
static std::atomic<bool> g_disk_chosen{false};  // set after g_disk_sectors: a CD-ROM with no disc has 0
static std::atomic<uint8_t> g_pio_mode{0};
static std::atomic<uint8_t> g_mdma_mode{NO_DMA};       // set by core 1 at start-up (setup1)
static std::atomic<uint8_t> g_mode_asked{0};            // HDDSYNTH.INI's mode, for core 1
static SharedFlag g_dmarq_wired{false};                 // found by core 1 at start-up
static SharedFlag g_dma_auto{false};                    // HDDSYNTH.INI dma: auto may offer DMA
// Which PIO programs core 1 started (pio_plan.h), for `s` and `S`: written
// once by core 1 at start-up, then g_pio_planned set; core 0 reads it after.
static pio_plan::Plan g_pio_plan;
static std::atomic<bool> g_pio_planned{false};  // start-up only, so std::atomic is fine (not per access)

// Which bus program the drive would choose for this host (docs/auto-bus.md):
// advice only, printed as AUTOBUS lines and in `s`; nothing switches yet.
// Core 0 only. g_sectors_served counts the data sectors serveFetches() moves.
static auto_bus::Advisor g_auto_bus;
static bool g_auto_bus_started = false;
static uint32_t g_sectors_served = 0;
static auto_bus::Counters autoBusCounters();

// HDDSYNTH.INI's geometry for core 1, packed (cylinders << 16 | heads << 8 |
// sectors), or 0 for auto; stored with the disk's size (geometryFor).
static std::atomic<uint32_t> g_geometry{0};
// Device 1 (docs/two-drives.md): its disk's size (0: no device 1) and default
// geometry, packed as g_geometry is. Core 0 sets both at boot, before the
// device 0's size, which core 1 waits for.
static std::atomic<uint32_t> g_device1_sectors{0};
static std::atomic<uint32_t> g_device1_geometry{0};
// A CD-ROM drive (docs/atapi.md): as device 1 beside the disk (`device1 =
// cdrom`: g_device1_sectors is then its disc's, 0 for none), or alone as
// device 0 (`device0 = cdrom`: g_disk_sectors is its disc's). Plain bools:
// core 0 sets them before publishing g_disk_chosen (release), and core 1 reads
// them only after waiting for that (acquire); an atomic's load could be
// called out of line, from flash.
static bool g_device0_cdrom = false;
static bool g_device1_cdrom = false;
static bool g_geometry_trimmed = false;  // the cylinders were cut to fit the disk (CONFIG says so)
static const char* g_geometry_from = "auto";  // "set", "partition table" or "auto" (the default), for CONFIG

// What IDENTIFY says: the device's, and the bus observer's to check it by.
__attribute__((always_inline)) static inline IdentifyInfo driveIdentity() {
    uint32_t g = g_geometry.load(std::memory_order_relaxed);
    return IdentifyInfo{g_device0_cdrom ? "HDDSYNTH PRO CD-ROM" : "HDDSYNTH PROTO", "HSP-000001", FIRMWARE_VERSION,
                        g_disk_sectors.load(std::memory_order_acquire),
                        g_pio_mode.load(std::memory_order_relaxed),
                        g_mdma_mode.load(std::memory_order_relaxed),
                        Geometry{uint16_t(g >> 16), uint16_t((g >> 8) & 0xFF), uint16_t(g & 0xFF)}};
}

// Device 1's identity: its own model, serial and size, device 0's modes.
__attribute__((always_inline)) static inline IdentifyInfo device1Identity() {
    uint32_t g = g_device1_geometry.load(std::memory_order_relaxed);
    const bool cdrom = g_device1_cdrom;
    return IdentifyInfo{cdrom ? "HDDSYNTH PRO CD-ROM" : "HDDSYNTH PRO D1", cdrom ? "HSP-000001-CD" : "HSP-000001-D1",
                        FIRMWARE_VERSION,
                        g_device1_sectors.load(std::memory_order_relaxed),
                        g_pio_mode.load(std::memory_order_relaxed),
                        g_mdma_mode.load(std::memory_order_relaxed),
                        Geometry{uint16_t(g >> 16), uint16_t((g >> 8) & 0xFF), uint16_t(g & 0xFF)}};
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

// Core 1's per-pass profiling (c1_gap_ns, c1_pass_ns, c1_fifo_cyc), built in
// only when this is true. On every pass it costs enough that, in the normal
// (non-early) mode, reads ran past ATA's 1,250 ns IORDY limit through the USB
// adapter (0.23.x-0.26.2: up to 3,570 ns, and the adapter never saw DRQ).
constexpr bool C1_PROFILE = false;

// The longest time core 1 spent on a command (the device's own work and its
// trace: a CD-ROM packet's reply is built there) while the host's next access
// waited, and which command and packet it was, for `s` (`cmd_run_max_ns`,
// `cmd_run_max`). A bench diagnostic, built in only with -DCMD_PROFILE=1
// (docs/bench-checklist.md), so the normal builds' core 1 code stays as measured.
#ifndef CMD_PROFILE
#define CMD_PROFILE 0
#endif
// Console `M`: the CD-ROM's disc changed at run time (docs/atapi.md). Core 0
// asks with a new sequence number and the disc's size (0: no disc); core 1
// changes the device's disc once it's idle (atapi::Device::idle) and echoes
// the number. Core 0 ejects first and swaps files only once core 1 has
// confirmed, so no fetch for one disc is ever served from the next.
static std::atomic<uint32_t> g_disc_seq{0};
static std::atomic<uint32_t> g_disc_sectors{0};
static std::atomic<uint32_t> g_disc_done{0};

#if CMD_PROFILE
static volatile uint32_t g_cmd_run_max = 0;   // cycles
static volatile uint32_t g_cmd_run_what = 0;  // command | packet << 8
#endif

// Core 1's longest loop passes, in cycles (DWT CYCCNT), by what the pass did,
// for console `s` (`c1_gap_ns`): a pass that takes long is a register read
// left waiting. Racy across cores, as diagnostics may be; `r` clears it.
enum : uint8_t { GAP_ACCESS = 1, GAP_LANE, GAP_COMMAND, GAP_BUS_ONLY, GAP_STEP0 };  // steps 0-4 follow
constexpr unsigned GAP_KINDS = GAP_STEP0 + 5;
static volatile uint32_t g_c1_gap[GAP_KINDS];
static volatile uint32_t g_c1_fifo_cycles, g_c1_fifo_max;  // the FIFO check alone (diagnostic)
static volatile uint32_t g_c1_passes;
static volatile uint32_t g_c1_iser[2], g_c1_primask;  // core 1's interrupt set-up, captured in setup1
#ifndef HDDPRO_C1_NO_IRQ
#define HDDPRO_C1_NO_IRQ 0  // -DHDDPRO_C1_NO_IRQ=1: core 1 masks all interrupts (a bench test)
#endif
static uint32_t g_bench_pio, g_bench_ram, g_bench_sio, g_bench_nop;  // cycles per 1000, measured in setup1
static volatile uint32_t g_bench_var;
static uint32_t g_bench_pio_ram, g_bench_nop_ram;

#ifdef HDDPRO_OBS_REGS
namespace bus_observer { extern volatile bool g_obs_rearm; }
#endif
volatile uint32_t g_wlog_pos[64];
volatile uint32_t g_wlog[64]; volatile uint32_t g_wlog_n; volatile uint32_t g_wlog_stop; uint32_t g_obs_snap[160]; uint32_t g_obs_snap_end; volatile uint32_t g_obs_snapped; uint32_t g_cmd_snap_pos;  // bench: the table's last writes  // the same loops, from core 1's RAM code
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

// The diagnostics the PIO plan has room for (iordy_timer, bus_observer):
// started by core 1 at start-up, and by core 0 again when the PCB's card
// gives back the PIO block it borrowed for a card operation.
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
    if (!g_pio_plan.iordy_timer || !g_pio_plan.bus_observer) return false;  // they don't have a block of their own
    iordy_timer::end();
    bus_observer::end();
    return true;
}
static void reclaimDiagnostics() { startDiagnostics(); }

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
    while (!g_disk_chosen.load(std::memory_order_acquire)) tight_loop_contents();  // a CD-ROM with no disc: 0 sectors
    if (g_table.load())
        ide_table::begin();  // every read answered from the PIO's registers (docs/register-table.md)
    else
        ide_bus::begin(g_isa.load(), g_early.load(), g_iordy.load(), !g_no_lane.load());
    busSwitchesOn();
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
    static void runDrive(D& dev);
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
static SharedFlag g_absent{false};

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
// or with a CD-ROM. Never returns. The bus comes first: every access waiting
// in the FIFO is served back to back, and only when it is empty does core 1
// do one small bookkeeping step before looking again. So a read waits for one
// step at most; iordy_timer.pio measures how long the host actually waits.
template <class Dev>
__attribute__((always_inline)) static inline void runDriveT(Dev& dev) {
    uint32_t started = dev.commandsStarted();
    g_done_recorded = dev.commandsCompleted();
    uint32_t command_start_us = 0;
    std::decay_t<decltype(dev.lastCommand())> start_info{};  // the command whose CMD line is still to record
    bool start_unrecorded = false;
    bool quiet = false;  // a command just started: bookkeeping waits (COMMAND_QUIET_US)
    uint32_t posted_fetch = 0;
    bool reset_low = !bus::high(pinsAsSample(), cfg::PIN_RESET_N);
    dev.hardReset(reset_low);
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
                last_access_cycles = m33_hw->dwt_cyccnt;
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
                    dev.fetchDone(result.id, result.ok);
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
    RAM __attribute__((noinline)) static void runDrive(D& dev) { runDriveT(dev); }
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
    constexpr uint32_t DRIVE = 0x00FF0000;  // DD0-7, as ide_table.cpp's
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
// bench: core 1's events against the observer's ring position (console R, `EV1`):
// 1 Command taken, 2 INTRQ set (bits 28-29: asserted, driven), 3 interrupt
// acknowledged, 4 fetch result taken, 5 data-in block started.
volatile uint32_t g_elog[64]; volatile uint32_t g_elog_n;
__attribute__((always_inline)) static inline void elog(uint32_t kind, uint32_t bits = 0) {
    g_elog[g_elog_n & 63] = ide_table::observerRingIndex() | bits << 28 | kind << 24;
    g_elog_n = g_elog_n + 1;
}

template <class Dev>
__attribute__((always_inline)) static inline void tableIntrq(Dev& dev) {
    int8_t& shown = g_table_intrq;
    int8_t now = int8_t((dev.intrq() ? 1 : 0) | (dev.intrqDriven() ? 2 : 0));
    if (now == shown) return;
    shown = now;
    elog(2, uint32_t(now & 3));
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
            if (g_wlog_stop == 0 || g_wlog_stop > 1) {  // bench: freeze 16 writes after an SRST mid-block
                if (g_wlog_stop > 1) g_wlog_stop = g_wlog_stop - 1;
                if (g_wlog_stop == 2 && !g_obs_snapped) { ide_table::snapObserver(g_obs_snap, 160, g_obs_snap_end); g_obs_snapped = 1; }
                g_wlog_pos[g_wlog_n & 63] = ide_table::observerRingIndex() | (ide_table::dataOutActive() ? 0x80000000u : 0u);
                g_wlog[g_wlog_n & 63] = sample | (dev.status() & ata::ST_DRQ ? 0x80000000u : 0) | (dev.commandQueued() ? 0x40000000u : 0); g_wlog_n = g_wlog_n + 1;
                if (g_wlog_stop == 0 && reg == ata::Reg::AltStatusDevCtrl && (bus::data(sample) & ata::DC_SRST) && dev.status() & ata::ST_DRQ) g_wlog_stop = 17;
            }
            if (reg == ata::Reg::Data || reg == ata::Reg::None) continue;  // WD has the data words
            if (reg == ata::Reg::StatusCommand) {
                g_cmd_snap_pos = ide_table::observerRingIndex() | 0x80000000u;  // bench: snapshot the reads after it
                elog(1, bus::data(sample) & 0xF);
                dev.queueCommand(uint8_t(bus::data(sample)));
                ide_table::armCommandBusy();
                if (g_table_intrq > 0) g_table_intrq = int8_t(g_table_intrq & ~1);  // C lowered INTRQ
            } else {
                dev.write(reg, bus::data(sample));
            }
            dirty = true;
        }
        if (g_cmd_snap_pos && !g_wlog_stop) {  // bench: the reads around the last Command write
            uint32_t now = ide_table::observerRingIndex(), from = g_cmd_snap_pos & 0x7FFFFFFF;
            uint32_t ring = 1u << 12;  // bus_observer::ringSize() (RING_WORDS)
            if ((now - from + ring) % ring >= 60) {  // 160 reads: about 100 before the Command, 60 after
                ide_table::snapObserver(g_obs_snap, 160, g_obs_snap_end);
                g_obs_snapped = 1;
                g_cmd_snap_pos = 0;
            }
        }
        if (ide_table::statusReadSince(irq_raised_at) && dev.intrq()) {
            dev.consumeRead(ata::Reg::StatusCommand);  // acknowledges the interrupt raised at irq_raised_at
            elog(3);
            dirty = true;
        }
        if (dev.commandQueued()) {
            if (ide_table::dataInActive()) ide_table::stopWhy(3), ide_table::stopDataIn();  // a new command abandons a block
            if (ide_table::dataOutActive()) ide_table::stopDataOut();
            runCommandEarly(dev);  // its CMD line, and it runs
            dirty = true;
        }

        // Core 0's side, and the timers
        postFetch(dev, posted_fetch);
        FetchResult result;
        if (g_mailbox.takeResult(result)) {
            dev.fetchDone(result.id, result.ok);
            elog(4);
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
            dirty = true;
        }
        uint32_t pins = pinsAsSample();
        bool low = !bus::high(pins, cfg::PIN_RESET_N);
        if (low != reset_low) {
            reset_low = low;
            ide_table::stopWhy(4);
            ide_table::stopDataIn();
            ide_table::stopDataOut();
            dev.hardReset(low);
            if (!low) iordy_timer::requestReset();
            record(low ? TraceKind::ResetAssert : TraceKind::ResetRelease, pins);
            dirty = true;
        }

        if (ide_table::benchHold()) continue;  // bench: console Q is driving the blocks
        {
            const uint16_t* n_;
            uint16_t l_;
            ide_table::notePass(dev.dataInRemaining(n_, l_) ? 1u : 0u);
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
                ide_table::stopWhy(1);
                ide_table::stopDataIn();
                if (dev.dataInRemaining(next, left)) dev.consumeDataWords(left);
                dirty = true;
            } else if (!dev.dataInRemaining(next, left)) {  // the device dropped it (SRST, a reset)
                ide_table::stopWhy(2);
                ide_table::stopDataIn();
                dirty = true;
            }
        }
        if (!ide_table::dataInActive() && dev.dataInRemaining(next, left)) {
            ide_table::startDataIn(next, left);  // before the host can see DRQ
            elog(5);
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
    if (cdrom0 || cdrom1)  // a CD-ROM alone as device 0 (core 0 leaves device 1 out), or as device 1
        runDrive(*new (g_device_mem) hdcd::Device(device0, nullptr, cdrom0 ? &device0 : &device1, cdrom0 ? 0 : 1));
    else
        runDrive(*new (g_device_mem) ata::Device(device0, device1.total_sectors ? &device1 : nullptr));
}

// Core 0's share of the drive: move the sectors core 1 asks for. The disk is
// an image on the SD card (slice 5) if there is one to serve (chooseDisk()),
// else the welcome disk, or the generated test disk with `device0 = none` or
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

// Device 1's disk (`device1` in HDDSYNTH.INI, docs/two-drives.md), or nullptr:
// a second image, or a folder served read-only.
static BlockSource* g_device1_disk = nullptr;
static const char* g_device1_kind = "none";  // "none", "image" or "folder"
static char g_device1_note[112] = "";         // why the device1 configured isn't served
static folder_disk::Volume g_folder;
static FolderDisk g_folder_disk(g_folder);
static uint16_t g_folder_capacity = 0;  // entries the folder's list has room for

// A CD-ROM drive as device 0 with no disc to serve: a drive with no disc in it.
class EmptyDrive : public BlockSource {
public:
    uint32_t sectors() const override { return 0; }
    bool read(uint32_t, uint16_t[256]) override { return false; }
};
static EmptyDrive g_no_disc;

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
// Console `P`: a bus program after the restart it triggers, whatever
// HDDSYNTH.INI says, until a power cycle (watchdog scratch 1): `e` early
// answers, `l` the data lane, `i` ide_bus (as `isa = on`), `0` the settings'.
// For the rig, to compare programs with no card or without editing the file.
static constexpr uint32_t BUS_MAGIC = 0x48445000;  // "HDP", then the program's letter
static char busRequested() {
    uint32_t v = watchdog_hw->scratch[1];
    char c = char(v & 0xff);
    return (v & 0xffffff00) == BUS_MAGIC && (c == 'e' || c == 'l' || c == 'i' || c == 't') ? c : 0;
}

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
[[maybe_unused]] static bool useImage(const char* name, const char*& why, bool read_only = false) {
    ImageFile* file = sd_card::openImage(name, Serial, read_only);
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

// A CD-ROM's disc from the card, read-only: an ISO as it is, or a CUE
// sheet's data track from its BIN (cue_sheet.h). In device 0's image slot for
// `device0 = cdrom`, device 1's for `device1 = cdrom` (never both: a CD-ROM
// as device 0 has no device 1). nullptr if it can't be used, with why.
static BlockSource* openCdDisc(const char* name, const char*& why, uint8_t unit) {
    auto open = unit ? sd_card::openDevice1Image : sd_card::openImage;
    if (!cue_sheet::isCueName(name)) {
        ImageFile* file = open(name, Serial, true);
        if (!file) {
            why = sd_card::openError();
            return nullptr;
        }
        static ImageDisk iso(*file);  // always the same file object, reopened
        if (!iso.sectors()) why = "empty";
        return iso.sectors() ? &iso : nullptr;
    }
    static char text[cue_sheet::MAX_TEXT];
    size_t len = 0;
    bool truncated = false;
    if (!sd_card::readFileStart(name, text, sizeof text, len, truncated, why, Serial)) return nullptr;
    if (truncated)  // a long sheet: up to its last whole line
        while (len && text[len - 1] != '\n') --len;
    cue_sheet::Sheet parsed;
    if (!cue_sheet::parse(text, len, parsed)) {
        why = parsed.error;
        return nullptr;
    }
    // Kept only now: the card code holds on to the open BIN's name (to reopen
    // it after the card comes back), and a sheet that failed mustn't blank it.
    static cue_sheet::Sheet sheet;
    sheet = parsed;
    ImageFile* file = open(sheet.file, Serial, true);
    if (!file) {
        static char bin_why[cue_sheet::MAX_FILE_NAME + 24];
        std::snprintf(bin_why, sizeof bin_why, "%s %s", sheet.file, sd_card::openError());
        why = bin_why;
        return nullptr;
    }
    static RawTrackDisk raw(*file);
    raw.setTrack(sheet.track);
    if (!raw.sectors()) {
        why = "its data track is empty";
        return nullptr;
    }
    Serial.printf("# %s: %s, track 1 of %u%s (%lu-byte frames from byte %lu), %lu frames\n", name, sheet.file,
                  unsigned(sheet.tracks), truncated ? "+" : "", (unsigned long)sheet.track.stride, (unsigned long)sheet.track.data_offset,
                  (unsigned long)(raw.sectors() / atapi::SECTORS_PER_FRAME));
    return &raw;
}

// The geometry an image's partition table was made with, packed as
// g_geometry is, or 0 (then 16 x 63).
static uint32_t partitionGeometry(BlockSource& disk) {
    static uint16_t sector0[256];  // bus order: as bytes, as on disk
    if (!disk.read(0, sector0)) return 0;
    Geometry geo = geometryFromPartitions(reinterpret_cast<const uint8_t*>(sector0), disk.sectors());
    if (!geo.cylinders) return 0;
    return uint32_t(geo.cylinders) << 16 | uint32_t(geo.heads) << 8 | geo.sectors_per_track;
}

// A CD-ROM drive as device 1 (`device1 = cdrom`, or an ISO): its disc, as
// chooseCdrom0 chooses device 0's. The generated test disc after console `T`
// or with `cdrom = none` (read-only, so the disk's RAM overlay isn't shared);
// else the ISO; else a drive with no disc in it.
static GeneratedDisk g_test_disc;
static const char* g_device1_disc = "no disc";

static void chooseCdrom1() {
    g_device1_cdrom = true;
    g_device1_kind = "cdrom";
    const char* name = drive_config::device1Disc(g_config);
    if (testDiskRequested() || (sd_card::mounted() && !drive_config::servesDisc(g_config))) {
        g_device1_disk = &g_test_disc;
        g_device1_disc = "test disc";
    } else if (!sd_card::mounted()) {
        std::snprintf(g_device1_note, sizeof g_device1_note, "%s: no card", name);
    } else {
        const char* why = "";
        if ((g_device1_disk = openCdDisc(name, why, 1)))
            g_device1_disc = name;
        else
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: %s", name, why);
    }
    g_device1_sectors.store(g_device1_disk ? g_device1_disk->sectors() : 0, std::memory_order_relaxed);
}

// A CD-ROM drive alone as device 0 (`device0 = cdrom`, or an ISO), as the
// CD-ROM firmware (bb48_cdrom, up to 0.31.0) served it: the generated test
// disc after console `T` or with `cdrom = none`; else the disc named; else
// a drive with no disc in it. With no card: no disc.
static void chooseCdrom0() {
    g_device0_cdrom = true;
    g_disk = &g_no_disc;
    g_disk_name = "no disc";
    const char* name = drive_config::device0Disc(g_config);
    const char* why = "";
    if (testDiskRequested() || (sd_card::mounted() && !drive_config::servesDisc(g_config))) {
        g_disk = &g_overlay;  // read-only here, so the overlay stays empty
        useTestDisk();
    } else if (!sd_card::mounted()) {
        std::snprintf(g_image_note, sizeof g_image_note, "%s: no card", name);
    } else if (BlockSource* disc = openCdDisc(name, why, 0)) {  // a disc: read-only
        g_disk = disc;
        g_image_name = name;
        g_disk_name = "disc";
    } else {
        std::snprintf(g_image_note, sizeof g_image_note, "%s: %s", name, why);
    }
    if (drive_config::hasDevice1(g_config))
        std::snprintf(g_device1_note, sizeof g_device1_note, "%s: device 0 is a CD-ROM drive, alone",
                      g_config.device1);
}

// Device 1's disk, from `device1` (docs/two-drives.md): a folder, laid out as
// a read-only FAT16 volume, or an image. Not with the test or bench disk
// asked for from the console, which are for the rig's tests.
static void chooseDevice1() {
    if (drive_config::device1IsCdrom(g_config)) {
        chooseCdrom1();
        return;
    }
    if (!drive_config::hasDevice1(g_config) || !sd_card::mounted() || testDiskRequested() || benchRequested())
        return;
    const char* name = g_config.device1;
    if (sd_card::isFolder(name)) {
        // The list on the heap, only when a folder is served: as long as RAM
        // allows, leaving cfg::FOLDER_HEAP_LEFT for whatever allocates later.
        // malloc, not new (std::nothrow): Arduino-Pico's libstdc++ is built
        // without exceptions, so a nothrow new that runs out aborts (the board
        // never started with a folder, 3 Nov 2026). An Entry needs no constructor.
        uint16_t capacity = cfg::FOLDER_MAX_ENTRIES;
        folder_disk::Entry* entries = nullptr;
        while (capacity >= 64) {
            entries = static_cast<folder_disk::Entry*>(std::malloc(capacity * sizeof(folder_disk::Entry)));
            if (entries && rp2040.getFreeHeap() >= int(cfg::FOLDER_HEAP_LEFT)) break;
            std::free(entries);
            entries = nullptr;
            capacity /= 2;
        }
        if (!entries) {
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: no RAM for its list", name);
            return;
        }
        if (!g_folder.build(sd_card::folderSource(name), entries, capacity, name)) {
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: can't list it, or nothing fits", name);
            std::free(entries);
            return;
        }
        g_folder_capacity = capacity;
        g_device1_disk = &g_folder_disk;
        g_device1_kind = "folder";
    } else if (strcasecmp(name, g_image_name) == 0) {
        // The image device 0 serves (named, or fallen back to): the PC would see
        // one filesystem as two disks, and writes through either would corrupt it.
        std::snprintf(g_device1_note, sizeof g_device1_note, "%s: device 0 serves it already", name);
        return;
    } else {
        ImageFile* file = sd_card::openDevice1Image(name, Serial);
        if (!file) {
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: %s", name, sd_card::openError());
            return;
        }
        static ImageDisk image(*file);
        if (image.sectors() == 0) {
            std::snprintf(g_device1_note, sizeof g_device1_note, "%s: empty", name);
            return;
        }
        g_device1_disk = &image;
        g_device1_kind = "image";
        g_device1_geometry.store(partitionGeometry(image), std::memory_order_relaxed);
    }
    g_device1_sectors.store(g_device1_disk->sectors(), std::memory_order_relaxed);
}

// The disk: the test disk after console `T`, or HDDBENCH.IMG after `J`; else
// the image HDDSYNTH.INI names (HDDSYNTH.IMG by default), or the test disk
// for `device0 = none`; else HDDSYNTH.IMG; else the welcome disk.
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
    if (drive_config::device0IsCdrom(g_config) && !benchRequested()) {
        chooseCdrom0();  // console `J` still serves the bench image, as a hard disk
    } else if (testDiskRequested()) {
        useTestDisk();  // the card, if any, left alone
    } else if (!sd_card::mounted()) {
        // no card: the welcome disk, without waiting for the card again
    } else if (benchRequested()) {
        if (useImage(sd_card::BENCH_IMAGE_NAME, why)) g_disk_name = "bench";
    } else if (!drive_config::servesImage(g_config)) {
        useTestDisk();  // device0 = none: the card left alone
    } else if (useImage(g_config.device0, why)) {
        g_disk_name = "image";
    } else {
        std::snprintf(g_image_note, sizeof g_image_note, "%s: %s", g_config.device0, why);
        if (strcasecmp(g_config.device0, drive_config::DEFAULT_IMAGE) != 0 &&
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
    // isa = on keeps its own program (no early answers, iordy_watchdog.pio):
    // the one the ICL 386 was proven on
    g_early.store(g_config.early_answers && !g_config.isa);
    switch (busRequested()) {  // console `P`
        case 'e': g_isa.store(false); g_early.store(true); break;
        case 't': g_isa.store(false); g_early.store(false); g_table.store(true); break;
        case 'l': g_isa.store(false); g_early.store(false); break;
        case 'i': g_isa.store(true); g_early.store(false); break;
        default: break;
    }
    g_iordy.store(g_config.iordy);
    // The geometry setting, for the image only (the welcome and generated disks
    // lay theirs out for the default), its cylinders cut to fit if it's bigger
    const bool image = strcmp(g_disk_name, "image") == 0;
    Geometry geo = image ? g_config.geometry : Geometry{};
    g_geometry_from = geo.cylinders ? "set" : "auto";
    if (image && !geo.cylinders) {  // auto: the geometry the image's partition table was made with
        static uint16_t sector0[256];  // bus order: as bytes, as on disk
        if (g_disk->read(0, sector0)) {
            geo = geometryFromPartitions(reinterpret_cast<const uint8_t*>(sector0), g_disk->sectors());
            if (geo.cylinders) g_geometry_from = "partition table";
        }
    }
    g_geometry_trimmed = false;
    if (geo.cylinders && geo.sectors() > g_disk->sectors()) {
        geo.cylinders = uint16_t(g_disk->sectors() / (uint32_t(geo.heads) * geo.sectors_per_track));
        g_geometry_trimmed = true;
    }
    g_geometry.store(geo.cylinders ? uint32_t(geo.cylinders) << 16 | uint32_t(geo.heads) << 8 | geo.sectors_per_track : 0,
                     std::memory_order_relaxed);
    // A host that ignores IORDY reads whatever is on the bus: check image data
    // on the cable too, with early answers and on the data lane. Not with
    // `isa = on`: the ISA card that needs it strobes DIOR# with no chip select,
    // which the observer takes for data.
    bus_observer::setImageChecks(!g_isa.load() && !g_test_disk);
    if (!g_device0_cdrom) chooseDevice1();  // a CD-ROM as device 0 is alone on the cable
    // Core 1 shares out the PIO blocks next, by whether the card holds one: so
    // after device 1's image, folder or disc is open (with device 0 on the
    // test disk the card would otherwise be parked under them).
    sd_card::sharePio();
    // A CD-ROM, as device 0 or 1, runs on ide_bus (as console `P i`), never the
    // data lane (written for the hard disk's 256-word blocks). On early answers
    // hdcd::Device answers too slowly (Status held 1.8-2.4 us): the USB
    // adapter's register check after RESET# never passed, so it never detected
    // the drive and dropped off USB (device 1: 4 Nov, 0.30.0-dev.10; device 0:
    // 9 Nov, 0.32.0-dev.2, where bb48_cdrom's own atapi::Device had managed).
    // Console `P e` still picks early answers, to reproduce it.
    // `isa` stays as set: forcing it on here (up to 0.32.1-dev.4) also started
    // iordy_watchdog on PCI hosts, which released IORDY mid-read on the
    // Evesham about once in 2.5 million data words, so the host latched DD
    // before the drive drove it (docs/atapi.md).
    if (g_device0_cdrom || g_device1_cdrom) g_table.store(false);  // the register table is the hard disk's, so far
    if ((g_device0_cdrom || g_device1_cdrom) && busRequested() != 'e') {
        g_no_lane.store(true);
        g_early.store(false);
    }
    g_disk_sectors.store(g_disk->sectors(), std::memory_order_relaxed);
    g_disk_chosen.store(true, std::memory_order_release);  // after the mode and device 1: core 1 reads them
}

// Console `M`: a disc change, step by step from core 0's loop (serviceDiscChange).
enum class DiscStep : uint8_t { None, Ejecting, Inserting };
static DiscStep g_disc_step = DiscStep::None;
static char g_disc_next[drive_config::MAX_NAME + 1];

static void requestDisc(uint32_t sectors) {
    g_disc_sectors.store(sectors, std::memory_order_relaxed);
    g_disc_seq.fetch_add(1, std::memory_order_release);
}

// The disc named, as the CD-ROM's disk: `none` (no disc), `test` (the
// generated test disc), or an ISO on the card, read-only. Returns its sectors,
// 0 for none (or an ISO that can't be used, which it says why).
static uint32_t openDisc(const char* name) {
    const bool none = strcasecmp(name, "none") == 0, test = strcasecmp(name, "test") == 0;
    if (g_device0_cdrom) {
        g_test_disk = false;
        g_disk = &g_no_disc;
        g_disk_name = "no disc";
        const char* why = "";
        if (test) {
            g_disk = &g_overlay;  // read-only here, so the overlay stays empty
            useTestDisk();
        } else if (BlockSource* disc = none ? nullptr : openCdDisc(name, why, 0)) {
            g_disk = disc;
            g_image_name = g_disc_next;
            g_disk_name = "disc";
        } else if (!none) {
            Serial.printf("# DISC %s: %s\n", name, why);
        }
        return g_disk->sectors();
    }
    g_device1_disk = nullptr;
    g_device1_disc = "no disc";
    if (test) {
        g_device1_disk = &g_test_disc;
        g_device1_disc = "test disc";
    } else if (!none) {
        const char* why = "";
        if ((g_device1_disk = openCdDisc(name, why, 1)))
            g_device1_disc = g_disc_next;
        else
            Serial.printf("# DISC %s: %s\n", name, why);
    }
    uint32_t sectors = g_device1_disk ? g_device1_disk->sectors() : 0;
    g_device1_sectors.store(sectors, std::memory_order_relaxed);
    return sectors;
}

static void startDiscChange(const char* name) {
    if (!g_device0_cdrom && !g_device1_cdrom) {
        Serial.println("# DISC: no CD-ROM drive (device0 = cdrom or device1 = cdrom gives one)");
        return;
    }
    if (g_disc_step != DiscStep::None) {
        Serial.println("# DISC: a change is already under way");
        return;
    }
    std::snprintf(g_disc_next, sizeof g_disc_next, "%s", name);
    requestDisc(0);  // out first: core 1 confirms once nothing for the old disc is under way
    g_disc_step = DiscStep::Ejecting;
    Serial.printf("# DISC ejecting, then %s\n", g_disc_next);
}

static void serviceDiscChange() {
    if (g_disc_step == DiscStep::None) return;
    if (g_disc_done.load(std::memory_order_acquire) != g_disc_seq.load(std::memory_order_relaxed)) return;
    if (g_disc_step == DiscStep::Ejecting) {
        uint32_t sectors = openDisc(g_disc_next);
        if (sectors) {
            requestDisc(sectors);
            g_disc_step = DiscStep::Inserting;
            return;
        }
        Serial.println("DISC none");
    } else {
        Serial.printf("DISC %s frames=%lu\n", g_disc_next,
                      (unsigned long)(g_disc_sectors.load(std::memory_order_relaxed) / atapi::SECTORS_PER_FRAME));
    }
    g_disc_step = DiscStep::None;
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
        Serial.println("# E: nothing changed; type key = value, e.g. device0 = GAMES.IMG");
        return;
    }
    static char text[drive_config::MAX_FILE], edited[drive_config::MAX_FILE];
    size_t len = 0;
    bool truncated = false;
    if (!sd_card::readConfig(text, sizeof text, len, truncated, Serial)) {
        if (!sd_card::mounted()) return;  // no card: readConfig said so
        if (sd_card::configExists()) {  // there, but unreadable: never overwrite it with the defaults
            Serial.printf("# E: can't read %s; nothing changed\n", drive_config::FILE_NAME);
            return;
        }
        len = strlen(drive_config::defaultFile());  // no file yet: start from the defaults
        memcpy(text, drive_config::defaultFile(), len);
    } else if (truncated) {
        Serial.printf("# E: %s is longer than %u bytes: edit it on a computer\n", drive_config::FILE_NAME,
                      unsigned(drive_config::MAX_FILE));
        return;
    }
    size_t n = drive_config::setKey(text, len, key, value, edited, sizeof edited);
    if (n == 0) {
        Serial.printf("# E: '%s' isn't a setting (image, mode, dma, write_behind, read_ahead, activity_link, seek_timing, isa, early_answers, iordy, geometry), "
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
    Serial.printf("CONFIG file=%s device0=%s mode=%s mode_used=%s pio_offered=%u mdma_offered=%s serving=%s contiguous=%s extents=%u parts=%u\n",
                  g_config_state,
                  g_config.device0, drive_config::modeName(g_config.mode), drive_config::modeName(used),
                  unsigned(g_pio_mode.load()), mdma_text, g_image_name,
                  sd_card::imageContiguous() ? "yes" : "no", sd_card::imageExtents(), sd_card::imageParts());
    if (g_device0_cdrom) {
        if (g_image_note[0]) Serial.printf("CONFIG cdrom not used: %s\n", g_image_note);
        Serial.printf("CONFIG device0=%s kind=cdrom disc=%s frames=%lu\n", g_config.device0,
                      g_disk_name[0] == 'g' ? "test disc" : g_disk_name[0] == 'n' ? "no disc" : g_image_name,
                      (unsigned long)(g_disk->sectors() / atapi::SECTORS_PER_FRAME));
    } else if (g_image_note[0]) {
        Serial.printf("CONFIG device0 not used: %s\n", g_image_note);
    }
    if (g_device1_cdrom)
        Serial.printf("CONFIG device1=%s kind=cdrom disc=%s frames=%lu\n", g_config.device1, g_device1_disc,
                      (unsigned long)(g_device1_sectors.load() / atapi::SECTORS_PER_FRAME));
    if (g_device1_disk && g_device1_kind[0] != 'c') {
        Serial.printf("CONFIG device1=%s kind=%s sectors=%lu\n", g_config.device1, g_device1_kind,
                      (unsigned long)g_device1_disk->sectors());
        if (g_device1_disk == &g_folder_disk) {
            static const char* why[] = {"", ": too many entries", ": the root folder is full", ": over 2 GB"};
            Serial.printf("CONFIG folder entries=%u/%u cluster_bytes=%lu read_only=yes%s%s\n",
                          unsigned(g_folder.count()), unsigned(g_folder_capacity),
                          (unsigned long)g_folder.clusterBytes(),
                          g_folder.truncated() == folder_disk::Truncated::No ? "" : " cut_short",
                          why[int(g_folder.truncated())]);
        }
    } else if (g_device1_note[0]) {
        Serial.printf("CONFIG device1 not used: %s\n", g_device1_note);
    }
    Serial.printf("CONFIG dmarq=%s dma=%s write_behind=%s read_ahead=%s activity_link=%s isa=%s early_answers=%s iordy=%s\n",
                  g_dmarq_wired.load() ? "wired" : "absent", drive_config::onOff(g_config.dma),
                  drive_config::onOff(g_config.write_behind), drive_config::onOff(g_config.read_ahead),
                  drive_config::onOff(g_config.activity_link), drive_config::onOff(g_config.isa),
                  drive_config::onOff(g_config.early_answers), drive_config::onOff(g_config.iordy));
    if (g_pio_planned.load(std::memory_order_acquire)) {
        const pio_plan::Plan& p = g_pio_plan;
        const bool diag = !sd_card::keptDiagnosticsBlock();  // a disc put in later may have taken their block
        Serial.printf("CONFIG pio card=%s dma=%s iocs16=%s iordy_timer=%s bus_observer=%s bus_timing=%s%s%s%s\n",
                      sd_card::busName(), drive_config::onOff(p.ide_dma), drive_config::onOff(p.iocs16),
                      drive_config::onOff(p.iordy_timer && diag), drive_config::onOff(p.bus_observer && diag),
                      drive_config::onOff(p.bus_timing), p.why ? ": " : "", p.why ? p.why : "",
                      diag ? "" : " (the card took the diagnostics' block for a disc, until a restart)");
    }
    if (busRequested())
        Serial.printf("CONFIG bus program set from the console (P%c) until a power cycle\n", busRequested());
    if (g_device0_cdrom || g_device1_cdrom)
        Serial.printf(g_early.load() ? "CONFIG with a CD-ROM as device %d on early answers (console P e): the USB adapter can't detect it\n"
                                     : "CONFIG with a CD-ROM as device %d, ide_bus\n",
                      g_device0_cdrom ? 0 : 1);
    {
        Geometry g = startGeometry(driveIdentity());
        Serial.printf("CONFIG geometry=%u/%u/%u (%s)%s\n", unsigned(g.cylinders), unsigned(g.heads),
                      unsigned(g.sectors_per_track), g_geometry_from,
                      g_geometry_trimmed ? ", cylinders cut to fit the disk" : "");
    }
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
    g_seek.setGeometry(sectors, startGeometry(driveIdentity()).sectors_per_track);
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
static uint32_t g_release_ms = 0;
static uint64_t g_no_cs_at_release = 0;
static bool g_no_cs_reported = false;

// The last few commands, for the NOIORDY line: which one an unwaited read
// fell in or followed (iordy_timer reports those separately).
static CommandHistory g_commands;

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
        BlockSource* disk = fetch.unit ? g_device1_disk : g_disk;  // device 1's, or device 0's
        bool ok = !disk ? false
                  : fetch.store ? disk->write(fetch.lba, fetch.buffer)
                  : fetch.flush ? true  // FLUSH CACHE: nothing to move
                                : disk->read(fetch.lba, fetch.buffer);
        if (ok && fetch.flush) ok = disk->flush();
        if (ok && !fetch.store && !fetch.flush) bus_observer::expect(fetch.lba, fetch.buffer);  // before core 1 can send it
        if (ok && !fetch.flush) ++g_sectors_served;  // auto_bus: past detection?
        uint64_t ready = now;
        if (fetch.store || !fetch.flush) {  // a sector read or written
            seek_timing::Release r = g_seek.onSector(now, fetch.command_lba, fetch.lba);
            ready = r.ready_us;
            if (r.first)
                postHeadTarget(fetch.command_lba, fetch.command_sectors,
                               fetch.store ? activity_link::Op::Write : activity_link::Op::Read, r.seek_start_us);
        }
#ifdef HDDPRO_FETCH_DELAY_US  // bench: every result held at least this long, as a real drive's access time
        if (ready < now + HDDPRO_FETCH_DELAY_US) ready = now + HDDPRO_FETCH_DELAY_US;
#endif
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
// different card (or the image changed) comes back, which isn't served; and
// when the card stopped answering and was restarted between two looks here
// (counted, so a loss and return within one pass isn't missed: 4 Nov, two
// went unreported on the Evesham).
static void printCardChanges() {
    static media_watch::State shown = sd_card::media().state();
    static uint32_t shown_losses = sd_card::media().losses(), shown_restarts = sd_card::restarts();
    const media_watch::Watch& card = sd_card::media();
    const uint32_t losses = card.losses(), restarts = sd_card::restarts();
    const bool moved = losses != shown_losses || restarts != shown_restarts;
    if (card.state() == shown && !moved) return;
    char line[160];
    int n;
    switch (card.state()) {
        case media_watch::State::Present:
            n = card.state() == shown
                    ? std::snprintf(line, sizeof line, "CARD lost and back %s: it stopped answering and was restarted (%lu in all)\n",
                                    g_image_name, (unsigned long)restarts)
                    : std::snprintf(line, sizeof line, "CARD back %s\n", g_image_name);
            break;
        case media_watch::State::Changed:
            n = std::snprintf(line, sizeof line,
                              "CARD changed: %s; not served (eject the disk on the computer, then restart the drive)\n",
                              card.why());
            break;
        case media_watch::State::None:  // device 0 isn't on the card: device 1's disk, `S` or `M` restarted it
            n = std::snprintf(line, sizeof line, "CARD restarted: it stopped answering (%lu in all)\n",
                              (unsigned long)restarts);
            break;
        default:
            n = std::snprintf(line, sizeof line, "CARD lost: the image's reads and writes fail until it's back\n");
            break;
    }
    if (printLine(line, size_t(n))) {
        shown = card.state();
        shown_losses = losses;
        shown_restarts = restarts;
    }
}

// auto_bus's evidence, as counted since boot (never cleared: `r` and the
// IORDY timer's reset at RESET# release clear the other counters).
static auto_bus::Counters autoBusCounters() {
    auto_bus::Counters c;
    c.dma_offered = g_mdma_mode.load(std::memory_order_relaxed) != NO_DMA;
    c.no_cs_known = g_pio_plan.bus_observer && !c.dma_offered;
    c.dior_no_cs = bus_observer::dmaWordsTotal();
    c.iordy_known = g_pio_plan.iordy_timer;
    c.iordy_ignored = iordy_timer::ignoredTotal();
    c.long_reads = iordy_timer::longTotal();
    c.sectors = g_sectors_served;
    c.cdrom = g_device0_cdrom || g_device1_cdrom;
    return c;
}

// An AUTOBUS line when the advice changes: the program the drive would run
// now, why, and when since the reset. Every 100 ms is plenty.
static void printAutoBus() {
    static uint32_t last_ms = 0;
    if (!g_pio_planned.load(std::memory_order_acquire)) return;
    uint32_t now = millis();
    if (!g_auto_bus_started) {
        g_auto_bus.reset(autoBusCounters(), now);
        g_auto_bus_started = true;
    }
    if (now - last_ms < 100) return;
    last_ms = now;
    if (!g_auto_bus.update(autoBusCounters(), now)) return;
    Serial.printf("AUTOBUS %s (%s) %lu ms after reset: %lu sectors, %lu long reads waited; running %s\n",
                  g_auto_bus.choice() == auto_bus::Choice::Lane ? "lane" : "early",
                  auto_bus::whyName(g_auto_bus.why()), (unsigned long)g_auto_bus.sinceResetMs(now),
                  (unsigned long)g_auto_bus.sectors(), (unsigned long)g_auto_bus.longReads(), (ide_table::running() ? "table" : ide_bus::busName()));
}

// A NOCS line the first time after each RESET# release (or boot) that the host
// strobes DIOR# with no chip select, while the drive offers no DMA: how soon
// an ISA host shows itself (g_release_ms above).
static void printNoChipSelect() {
    if (g_no_cs_reported || g_mdma_mode.load(std::memory_order_relaxed) != NO_DMA) return;
    if (bus_observer::dmaWords() == g_no_cs_at_release) return;
    g_no_cs_reported = true;
    Serial.printf("NOCS %lu ms after RESET# release: DIOR# with no chip select (an ISA card?)\n",
                  (unsigned long)(millis() - g_release_ms));
}

// A NOIORDY line when the host ends reads without waiting for IORDY, at most
// once a second: such a host reads garbage whenever core 1 is slower than its
// strobe (iordy_check.pio). It names the command the last one fell in or
// followed, its device, and how long after it started (and, if it had
// finished, after its DONE): to tell a block's first word, after a fetch,
// from the rest (docs/atapi.md, the Evesham's CD reads). The times are when
// core 0 collected the report, within a loop pass of the read (up to about
// 0.5 ms behind it while it stores a sector).
static void printIgnoredIordy() {
    static uint32_t shown = 0, last_ms = 0;
    uint32_t n = iordy_timer::ignoredReads();
    if (n == shown || millis() - last_ms < 1000) return;
    char context[64] = "";
    uint32_t at = iordy_timer::ignoredAtUs();
    if (const CommandHistory::Entry* c = g_commands.at(at)) {
        const char* name = c->command == 0xA0 ? mmc::name(c->packet) : commandName(c->command);
        int k = std::snprintf(context, sizeof context, " after=%s%s +%luus", name, c->unit ? " d1" : "",
                              (unsigned long)(at - c->start_us));
        if (c->ended && int32_t(at - c->end_us) >= 0 && k > 0 && size_t(k) < sizeof context)
            std::snprintf(context + k, sizeof context - size_t(k), " done+%luus", (unsigned long)(at - c->end_us));
    }
    char line[176];
    int len = std::snprintf(line, sizeof line,
                            "NOIORDY %lu reads=%lu data=%lu last=%s%s: the host didn't wait for IORDY\n",
                            (unsigned long)at, (unsigned long)n, (unsigned long)iordy_timer::ignoredDataReads(),
                            iordy_timer::ignoredRegister(), context);
    if (len > 0 && printLine(line, size_t(len) < sizeof line ? size_t(len) : sizeof line - 1)) {
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
        int n = pending.ns ? std::snprintf(line, sizeof line, "SLOWREAD %lu %s %lu\n", (unsigned long)pending.at_us,
                                           pending.reg, (unsigned long)pending.ns)
                           : std::snprintf(line, sizeof line, "EARLYDATA %lu\n", (unsigned long)pending.at_us);
        if (n <= 0 || !printLine(line, size_t(n) < sizeof line ? size_t(n) : sizeof line - 1)) return;
        has_pending = false;
    }
}

// HDDPRO_ACCESS_LOG (access_log.h): core 1's work before a late read, as
// ACCLOG lines, oldest first, each with how long before the late answer it
// began. A line at a time, so a busy USB link never holds core 0 up.
static void printAccessLog() {
    if constexpr (access_log::ON) {
        static unsigned next = 0;
        if (!access_log::g_frozen_ready) return;
        const access_log::Entry& last = access_log::g_frozen[access_log::SIZE - 1];
        const uint32_t ns_per_10_cycles = 10000 / (clock_get_hz(clk_sys) / 1000000);
        for (; next < access_log::SIZE; ++next) {
            const access_log::Entry& e = access_log::g_frozen[next];
            if (e.cycles == 0 && e.value == 0 && e.kind == 0) continue;  // not filled yet
            char what[40];
            switch (e.kind) {
                case access_log::ACCESS: {
                    bool write = bus::op(e.value) == bus::Op::Write;
                    std::snprintf(what, sizeof what, "%s %s %04x", write ? "WR" : "RD",
                                  bus::registerName(bus::block(e.value), bus::address(e.value), write),
                                  bus::data(e.value));
                    break;
                }
                case access_log::EMPTY: std::snprintf(what, sizeof what, "fifo empty"); break;
                case access_log::COMMAND: std::snprintf(what, sizeof what, e.value ? "command done" : "command run"); break;
                case access_log::STEP:
                    std::snprintf(what, sizeof what, "step %lu took %luns", (unsigned long)(e.value & 0xFF),
                                  (unsigned long)((e.value >> 8) * ns_per_10_cycles / 10));
                    break;
                default:
                    std::snprintf(what, sizeof what, "LATE answer, busy %luns",
                                  (unsigned long)(e.value * ns_per_10_cycles / 10));
                    break;
            }
            char line[80];
            int n = std::snprintf(line, sizeof line, "ACCLOG -%luns %s\n",
                                  (unsigned long)((last.cycles - e.cycles) * ns_per_10_cycles / 10), what);
            if (n <= 0 || !printLine(line, size_t(n) < sizeof line ? size_t(n) : sizeof line - 1)) return;
        }
        next = 0;
        access_log::g_frozen_ready = false;  // core 1 may freeze the next one
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
    // Which diagnostics run (pio_plan.h): their counts below stay 0 if off.
    if (g_pio_planned.load(std::memory_order_acquire))
        Serial.printf(" card_bus=%s diag=%s", sd_card::busName(),
                      sd_card::keptDiagnosticsBlock()                    ? "off"
                      : g_pio_plan.iordy_timer && g_pio_plan.bus_observer ? "on"
                      : g_pio_plan.iordy_timer                          ? "iordy_timer"
                                                                        : "off");
    iordy_timer::poll();
    Serial.printf(" iordy_slow=%lu iordy_max_ns=%lu iordy_max_reg=%s iordy_max_at_us=%lu",
                  (unsigned long)iordy_timer::slowReads(), (unsigned long)iordy_timer::maxNs(),
                  iordy_timer::maxRegister(), (unsigned long)iordy_timer::maxAtUs());
    Serial.printf(" iordy_early=%lu iordy_early_reg=%s iordy_early_data=%lu", (unsigned long)iordy_timer::earlyReads(),
                  iordy_timer::earlyRegister(), (unsigned long)iordy_timer::earlyDataReads());
    Serial.printf(" iordy_ignored=%lu iordy_ignored_reg=%s iordy_ignored_data=%lu", (unsigned long)iordy_timer::ignoredReads(),
                  iordy_timer::ignoredRegister(), (unsigned long)iordy_timer::ignoredDataReads());
    Serial.printf(" iordy_data_slow=%lu/%lu/%lu/%lu iordy_data_max_ns=%lu", (unsigned long)iordy_timer::dataSlowReads(0),
                  (unsigned long)iordy_timer::dataSlowReads(1), (unsigned long)iordy_timer::dataSlowReads(2),
                  (unsigned long)iordy_timer::dataSlowReads(3), (unsigned long)iordy_timer::dataMaxNs());
    static uint32_t s_passes_then, s_ms_then;
    uint32_t passes = g_c1_passes, ms = millis();  // stays 0 without C1_PROFILE: nothing printed
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
    Serial.printf(" bus=%s iordy_stall_released=%lu", ide_table::running() ? "table" : ide_bus::busName(), (unsigned long)ide_bus::iordyStallReleases());
    if (ide_table::running()) {
        uint32_t d[6];
        ide_table::debug(d);
        Serial.printf(" table_in=%lu table_out=%lu table_dpc=%lu table_dtx=%lu table_left=%lu table_ctrl=%08lx table_dinstr=%04lx table_r0e0=%08lx",
                      (unsigned long)ide_table::blocksIn(), (unsigned long)ide_table::blocksOut(), (unsigned long)d[0],
                      (unsigned long)d[1], (unsigned long)d[2], (unsigned long)d[3], (unsigned long)d[4], (unsigned long)d[5]);
    }
    Serial.printf(" autobus=%s autobus_why=%s autobus_sectors=%lu autobus_long=%lu",
                  g_auto_bus.choice() == auto_bus::Choice::Lane ? "lane" : "early", auto_bus::whyName(g_auto_bus.why()),
                  (unsigned long)g_auto_bus.sectors(), (unsigned long)g_auto_bus.longReads());
    if (ide_bus::dataLaneOn())
        Serial.printf(" lane_blocks=%lu lane_switch_max_ns=%lu", (unsigned long)ide_bus::laneSwitches(),
                      (unsigned long)ide_bus::laneSwitchMaxNs());
    {
        char early_profile[900];
        if (ide_bus::formatEarlyProfile(early_profile, sizeof early_profile)) Serial.print(early_profile);
    }
    Serial.printf(" intrq_rises=%lu commands=%lu", (unsigned long)(ide_table::running() ? ide_table::intrqRises() : ide_bus::intrqRises()),
                  (unsigned long)g_command_starts.load());
    if (C1_PROFILE) Serial.print(" c1_gap_ns=");
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
    // The same words, as the ISA signature (printNoChipSelect): only while the
    // drive offers no DMA, since otherwise they may be DMA.
    if (g_mdma_mode.load(std::memory_order_relaxed) == NO_DMA)
        Serial.printf(" dior_no_cs=%llu", (unsigned long long)bus_observer::dmaWords());
    else
        Serial.print(" dior_no_cs=n/a");
    Serial.printf(" bus_ring_max=%lu/%lu bus_dropped=%llu bus_lost=%lu bus_checks=%s",
                  (unsigned long)bus_observer::ringMax(), (unsigned long)bus_observer::ringSize(),
                  (unsigned long long)bus_observer::dropped(), (unsigned long)bus_observer::lost(),
                  bus_observer::full() ? "full" : "spot");
    Serial.printf(" bus_identify=%lu bus_identify_bad=%lu", (unsigned long)bus_observer::identifyChecked(),
                  (unsigned long)bus_observer::identifyBad());
    Serial.printf(" bus_image=%s bus_expect_missed=%lu", bus_observer::imageChecks() ? "on" : "off",
                  (unsigned long)bus_observer::expectMissed());
    Serial.printf(" c1_irq=%08lx,%08lx,primask:%lu", (unsigned long)g_c1_iser[0], (unsigned long)g_c1_iser[1],
                  (unsigned long)g_c1_primask);
    Serial.printf(" bus_contested=%lu bus_stalls=%lu", (unsigned long)iordy_timer::contestedAccesses(),
                  (unsigned long)iordy_timer::stallCycles());
#if CMD_PROFILE
    {
        uint32_t what = g_cmd_run_what;  // command, and a packet's opcode
        Serial.printf(" cmd_run_max_ns=%lu cmd_run_max=%02x/%02x",
                      (unsigned long)(uint64_t(g_cmd_run_max) * 1000000000ull / clock_get_hz(clk_sys)),
                      unsigned(what & 0xFF), unsigned((what >> 8) & 0xFF));
    }
#endif
    Serial.printf(" clk_sys_mhz=%lu clk_peri_mhz=%lu", (unsigned long)(clock_get_hz(clk_sys) / 1000000),
                  (unsigned long)(clock_get_hz(clk_peri) / 1000000));
    Serial.printf(" disk=%s sectors=%lu disk_ms=%lu", g_disk_name, (unsigned long)g_disk->sectors(),
                  (unsigned long)g_disk_ms);
    Serial.printf(" image_name=%s config=%s", g_image_name, g_config_state);
    Serial.printf(" device1=%s device1_sectors=%lu", g_device1_kind,
                  (unsigned long)(g_device1_disk ? g_device1_disk->sectors() : 0));
    const media_watch::Watch& card = sd_card::media();
    Serial.printf(" card=%s card_lost=%lu card_back=%lu card_restarts=%lu", media_watch::stateName(card.state()),
                  (unsigned long)card.losses(), (unsigned long)card.returns(), (unsigned long)sd_card::restarts());
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

// Restarts the board, keeping the watchdog scratch registers (J, T, P). Waits
// for the reset rather than returning, so nothing feeds the watchdog meanwhile
// (with RIG_WATCHDOG_MS, loop() would put the restart off for good).
[[maybe_unused]] static void restart() {  // the console's, not in the listen build
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

// Bench (12 Nov 2026, the Innostor bridge): core 0 logs every edge of the bus's
// control lines for 2 ms from the end of the next read command's Command write
// (20h, C4h, C8h), into `ev` (cycles in bits 0-24; the lines in 25-31).
static void edgeLog(uint32_t* ev, uint32_t cap) {
    static constexpr unsigned LINES[8] = {cfg::PIN_INTRQ,  cfg::PIN_DIOR_N,  cfg::PIN_DIOW_N,  cfg::PIN_IORDY,
                                          cfg::PIN_CS0_N,  cfg::PIN_DA0,     cfg::PIN_DA0 + 1, cfg::PIN_DA0 + 2};
    uint32_t mask = 0;
    for (unsigned l : LINES) mask |= 1u << l;
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;  // core 0's cycle counter
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
    const uint32_t cmd = (7u << cfg::PIN_DA0) | (1u << cfg::PIN_CS1_N);  // DA 7, CS0# low, CS1# high
    const uint32_t amask = (7u << cfg::PIN_DA0) | (1u << cfg::PIN_CS0_N) | (1u << cfg::PIN_CS1_N);
    Serial.printf("# waiting for a read command\n");
    Serial.flush();
    uint32_t start = time_us_32();
    uint8_t command = 0;
    for (;;) {
        if (time_us_32() - start > 20000000) { Serial.printf("# none\n"); return; }
        uint32_t g = sio_hw->gpio_in;
        if ((g & (1u << cfg::PIN_DIOW_N)) || (g & amask) != cmd) continue;
        uint32_t last = g;
        while (!((g = sio_hw->gpio_in) & (1u << cfg::PIN_DIOW_N))) last = g;
        command = uint8_t(last >> cfg::PIN_DD0);
        if (command == 0x20 || command == 0xC4 || command == 0xC8) break;
    }
    auto pack = [&](uint32_t c, uint32_t g) {
        uint32_t v = c & 0xFFFFFF;
        for (unsigned k = 0; k < 8; ++k) v |= ((g >> LINES[k]) & 1) << (24 + k);
        return v;
    };
    // The last 16 edges kept in a ring until the address leaves Status (DA 7),
    // then everything from there: the change from Status to Data.
    uint32_t n = 0, last = sio_hw->gpio_in & mask, t0 = m33_hw->dwt_cyccnt;
    const uint32_t da_mask = 7u << cfg::PIN_DA0;
    uint32_t ring = 0;
    ev[ring++ % 16] = pack(0, last);
    for (uint32_t spin = 0;; ++spin) {
#if FIRMWARE_SLICE != 0
        if (spin % 256 == 255) serveFetches();  // the read's sector, or it never comes
#endif
        uint32_t g = sio_hw->gpio_in & mask;
        if (g != last) ev[ring++ % 16] = pack(m33_hw->dwt_cyccnt - t0, g), last = g;
        if ((g & da_mask) != da_mask || m33_hw->dwt_cyccnt - t0 > 200000000u) break;  // or 1 s
    }
    {
        uint32_t keep = ring < 16 ? ring : 16, tmp[16];
        for (uint32_t k = 0; k < keep; ++k) tmp[k] = ev[(ring - keep + k) % 16];
        for (uint32_t k = 0; k < keep; ++k) ev[k] = tmp[k];
        n = keep;
    }
    uint32_t t1 = m33_hw->dwt_cyccnt;
    // Data register strobes left out (they'd fill the buffer in two sectors), counted instead
    const uint32_t data_addr = 0, data_mask = da_mask | (1u << cfg::PIN_CS0_N);
    uint32_t data_reads = 0;
    bool was_low = false;
    for (uint32_t spin = 0; m33_hw->dwt_cyccnt - t1 < 2000000u && n < cap; ++spin) {  // 10 ms at 200 MHz
#if FIRMWARE_SLICE != 0
        if (spin % 256 == 255) serveFetches();
#endif
        uint32_t g = sio_hw->gpio_in & mask;
        if ((g & data_mask) == data_addr) {
            bool low = !(g & (1u << cfg::PIN_DIOR_N));
            if (low && !was_low) ++data_reads;
            was_low = low;
            g |= 1u << cfg::PIN_DIOR_N;
        }
        if (g != last) ev[n++] = pack(m33_hw->dwt_cyccnt - t0, g), last = g;
    }
    Serial.printf("# data reads %lu\n", (unsigned long)data_reads);
    Serial.printf("EDGES n=%lu command=%02x (ns intrq dior diow iordy cs0 da0 da1 da2)\n", (unsigned long)n, command);
    for (uint32_t i = 0; i < n; ++i) {
        Serial.printf("E %lu", (unsigned long)((ev[i] & 0xFFFFFF) * 5));
        for (unsigned k = 0; k < 8; ++k) Serial.printf(" %d", int((ev[i] >> (24 + k)) & 1));
        Serial.printf("\n");
    }
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
        static bool disc_line = false;  // `M`, then a disc's name and Enter
        static char disc_buf[drive_config::MAX_NAME + 1];
        static size_t disc_len = 0;
        if (disc_line) {
            if (c == '\r' || c == '\n') {
                disc_line = false;
                disc_buf[disc_len] = '\0';
                char* name = disc_buf;
                while (*name == ' ') ++name;
                for (size_t n = strlen(name); n && name[n - 1] == ' '; --n) name[n - 1] = '\0';
                if (*name)
                    startDiscChange(name);
                else
                    Serial.println("# DISC: nothing changed");
            } else if (disc_len + 1 < sizeof disc_buf) {
                disc_buf[disc_len++] = char(c);
            }
            continue;
        }
        static bool bus_letter_next = false;  // `P` then e, l, i or 0
        if (bus_letter_next) {
            bus_letter_next = false;
            if (c == 'e' || c == 'l' || c == 'i' || c == 't' || c == '0') {
                watchdog_hw->scratch[1] = c == '0' ? 0 : BUS_MAGIC | uint32_t(c);
                Serial.printf("# restarting with %s\n", c == 'e'   ? "early answers"
                                                        : c == 'l' ? "the data lane"
                                                        : c == 'i' ? "ide_bus"
                                                        : c == 't' ? "the register table"
                                                                   : "the settings' bus program");
                Serial.flush();
                delay(50);
                restart();
                continue;
            }
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
                               "H seek timing on/off, P then e/l/i/0 bus program (restarts), "
                               "M then a disc and Enter: the CD-ROM's disc (device0 or device1 = cdrom), "
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
            case 'G': {  // bench: edges from the next read command (edgeLog)
                static uint32_t ev[2048];
                edgeLog(ev, 2048);
                break;
            }
#endif
#if FIRMWARE_SLICE != 0
            case 'r':
#if CMD_PROFILE
                g_cmd_run_max = 0;
#endif
                iordy_timer::reset();
                bus_timing::reset();
                for (auto& g : g_c1_gap) g = 0;
                bus_observer::resetCounts();
                ide_bus::resetEarlyProfile();
                ide_bus::resetLaneStats();
                break;
            case 'G': {  // bench: edges from the next read command, into the table's capture buffer
                const uint32_t* cap_pins; const uint16_t* cap_d; ide_table::capture(cap_pins, cap_d);
                edgeLog(const_cast<uint32_t*>(cap_pins), 1024);
                break;
            }
            case 'D': delay_digit_next = true; break;  // debug: delay data reads
            case 'P': bus_letter_next = true; break;  // a bus program until a power cycle
            case 'Z':  // bench: the register table's timeline from the next Command write
#ifdef HDDPRO_OBS_REGS
                { bus_observer::g_obs_rearm = true; Serial.printf("# obs rearmed\n"); }
#else
                if (ide_table::running()) ide_table::armCommandCapture(), Serial.printf("# armed\n");
#endif
                break;
            case 'R':  // bench: the register table's records (no tests)
                if (ide_table::running()) {
                    {
                        uint32_t n = g_wlog_n, from = n > 64 ? n - 64 : 0;
                        Serial.printf("WLOG n=%lu stop=%lu (reg data drq queued)\n", (unsigned long)n, (unsigned long)g_wlog_stop);
                        for (uint32_t i = from; i < n; ++i) {
                            uint32_t s = g_wlog[i & 63];
                            bus::Block b = bus::block(s);
                            Serial.printf("W out=%u @%u raw=%08lx %d %04x %d%d\n", unsigned(g_wlog_pos[i & 63] >> 31), unsigned(g_wlog_pos[i & 63] & 0x7FFFFFFF), (unsigned long)s, int(ata::regFor(b == bus::Block::Command, b == bus::Block::Control, bus::address(s))), unsigned(bus::data(s)), int(s >> 31), int((s >> 30) & 1));
                        }
                    }
                    {
                        uint32_t n = g_elog_n, from = n > 64 ? n - 64 : 0;
                        Serial.printf("ELOG n=%lu (1 cmd, 2 intrq a+2d, 3 ack, 4 fetched, 5 block)\n", (unsigned long)n);
                        for (uint32_t i = from; i < n; ++i)
                            Serial.printf("EV1 @%lu k=%lu v=%lu\n", (unsigned long)((g_elog[i & 63] & 0xFFFFFF) % bus_observer::ringSize()),
                                          (unsigned long)((g_elog[i & 63] >> 24) & 0xF), (unsigned long)(g_elog[i & 63] >> 28));
                    }
                    if (g_obs_snapped) {
                        Serial.printf("OBSSNAP end=%lu (address dd)\n", (unsigned long)g_obs_snap_end);
                        for (unsigned i = 0; i < 160; ++i) {
                            Serial.printf("O @%lu a=%02lx dd=%04lx raw=%08lx\n", (unsigned long)((g_obs_snap_end + bus_observer::ringSize() * 4 - 160 + i) % bus_observer::ringSize()),
                                          (unsigned long)((g_obs_snap[i] >> 16) & 0xFF), (unsigned long)(g_obs_snap[i] & 0xFFFF), (unsigned long)g_obs_snap[i]);
                            if (i % 32 == 0) delay(5);
                        }
                    }
                    {
                        const uint32_t* pins;
                        const uint16_t* d;
                        uint32_t n = ide_table::capture(pins, d);
                        Serial.printf("TABLECAP n=%lu (100 ns steps; dior cs0 cs1 da dd | d_pc d_lvl)\n", (unsigned long)n);
                        uint64_t last = ~0ull;
                        for (uint32_t i = 0; i < n; ++i) {
                            uint32_t p = pins[i];
                            uint32_t key = ((p >> 4) & 0xFFFFFF) | ((p >> cfg::PIN_INTRQ) & 1) << 24;  // DD, DA, CS, DIOR#, DIOW#, INTRQ
                            uint64_t v = uint64_t(key) << 12 | d[i];
                            if (v == last) continue;
                            last = v;
                            Serial.printf("CAP %4lu intrq=%lu dior=%lu diow=%lu cs0=%lu cs1=%lu da=%lu dd=%04lx | pc=%2u lvl=%u\n", (unsigned long)i,
                                          (unsigned long)((p >> cfg::PIN_INTRQ) & 1),
                                          (unsigned long)((p >> cfg::PIN_DIOR_N) & 1), (unsigned long)((p >> cfg::PIN_DIOW_N) & 1),
                                          (unsigned long)((p >> cfg::PIN_CS0_N) & 1),
                                          (unsigned long)((p >> cfg::PIN_CS1_N) & 1), (unsigned long)((p >> cfg::PIN_DA0) & 7),
                                          (unsigned long)((p >> cfg::PIN_DD0) & 0xFFFF), unsigned(d[i] & 0x1F), unsigned(d[i] >> 8));
                            if (i % 64 == 0) delay(5);
                        }
                    }
                    {
                        uint32_t b[81];
                        ide_table::blockRecords(b);
                        Serial.printf("TABLETEST blocks=%lu", (unsigned long)b[0]);
                        for (unsigned i = 0; i < b[0] && i < 16; ++i)
                            Serial.printf(" [start_level=%lu fetched=%lu end_level=%lu taken=%ld first=%04lx count=%lu why=%lu]",
                                          (unsigned long)b[1 + i * 5], (unsigned long)b[2 + i * 5], (unsigned long)b[3 + i * 5],
                                          long(b[2 + i * 5]) - long(b[3 + i * 5]),  // words D gave the host
                                          (unsigned long)(b[4 + i * 5] & 0xFFFF), (unsigned long)(b[4 + i * 5] >> 16),
                                          (unsigned long)b[5 + i * 5]);
                        Serial.printf("\n");
                    }
                }
                break;
            case 'Q':  // bench: the register table's D drives a word
                if (ide_table::running()) {
                    static const uint32_t waits[] = {0, 20, 100, 400};
                    for (uint32_t w : waits) {
                        uint32_t r = ide_table::busyTest(w);
                        Serial.printf("TABLETEST busy after %lu cycles: status %02lx -> %02lx (C1 fired=%lu)\n", (unsigned long)w,
                                      (unsigned long)(r & 0xFF), (unsigned long)((r >> 8) & 0xFF), (unsigned long)(r >> 16));
                    }
                    for (int k = 0; k < 5; ++k)
                        Serial.printf("TABLETEST busy latency %lu cycles\n", (unsigned long)ide_table::busyLatency());
                    {
                        uint32_t f = ide_table::flushTest();
                        Serial.printf("TABLETEST flush: level %lu -> %lu\n", (unsigned long)(f & 0xFF), (unsigned long)(f >> 8));
                    }
                    {
                        ide_table::fakeCycles(200);
                        const uint32_t* f = ide_table::fakeReads();
                        Serial.printf("TABLETEST fake data reads at 200 cycles (want b000 ... b027):");
                        for (unsigned k = 0; k < 40; ++k)
                            Serial.printf(" %04lx%s%s", (unsigned long)(f[k] & 0xFFFF), (f[k] >> 17) & 1 ? "" : "(not driven)",
                                          (f[k] >> 16) & 1 ? "" : "(held after)");
                        Serial.printf(" padout=%04lx funcsel=%lu pio=%lu stream_left=%08lx\n", (unsigned long)ide_table::fakeInfo(0),
                                      (unsigned long)ide_table::fakeInfo(1), (unsigned long)ide_table::fakeInfo(2),
                                      (unsigned long)ide_table::fakeInfo(3));
                    }
                    {
                        ide_table::fakeCycles(40);
                        const uint32_t* f = ide_table::fakeReads();
                        Serial.printf("TABLETEST fake data reads at 40 cycles (want b000 ... b027):");
                        for (unsigned k = 0; k < 40; ++k)
                            Serial.printf(" %04lx%s%s", (unsigned long)(f[k] & 0xFFFF), (f[k] >> 17) & 1 ? "" : "(not driven)",
                                          (f[k] >> 16) & 1 ? "" : "(held after)");
                        Serial.printf(" padout=%04lx funcsel=%lu pio=%lu stream_left=%08lx\n", (unsigned long)ide_table::fakeInfo(0),
                                      (unsigned long)ide_table::fakeInfo(1), (unsigned long)ide_table::fakeInfo(2),
                                      (unsigned long)ide_table::fakeInfo(3));
                    }
                    {
                        ide_table::fakeCycles(16);
                        const uint32_t* f = ide_table::fakeReads();
                        Serial.printf("TABLETEST fake data reads at 16 cycles (want b000 ... b027):");
                        for (unsigned k = 0; k < 40; ++k)
                            Serial.printf(" %04lx%s%s", (unsigned long)(f[k] & 0xFFFF), (f[k] >> 17) & 1 ? "" : "(not driven)",
                                          (f[k] >> 16) & 1 ? "" : "(held after)");
                        Serial.printf(" padout=%04lx funcsel=%lu pio=%lu stream_left=%08lx\n", (unsigned long)ide_table::fakeInfo(0),
                                      (unsigned long)ide_table::fakeInfo(1), (unsigned long)ide_table::fakeInfo(2),
                                      (unsigned long)ide_table::fakeInfo(3));
                    }
                    {
                        const uint32_t* r = ide_table::streamTest();
                        Serial.printf("TABLETEST stream: ctrl=%08lx count0=%lu", (unsigned long)r[0], (unsigned long)r[1]);
                        for (unsigned k = 0; k < 4; ++k)
                            Serial.printf(" [read=%lu left=%08lx]", (unsigned long)r[2 + 2 * k], (unsigned long)r[3 + 2 * k]);
                        Serial.printf(" level=%lu fdebug=%08lx\n", (unsigned long)r[10], (unsigned long)r[11]);
                    }
                    Serial.printf("TABLETEST d_y=%04lx d_x=%04lx d_isr=%04lx\n", (unsigned long)ide_table::readD(pio_y),
                                  (unsigned long)ide_table::readD(pio_x), (unsigned long)ide_table::readD(pio_isr));
                    uint32_t r1 = ide_table::selfTest(0xA55A), r2 = ide_table::selfTest(0x3CC3);
                    Serial.printf("TABLETEST released=%d a55a -> seen %04lx oe %04lx; 3cc3 -> seen %04lx oe %04lx\n",
                                  int(ide_table::selfTestReleased()),
                                  (unsigned long)(r1 & 0xFFFF), (unsigned long)(r1 >> 16), (unsigned long)(r2 & 0xFFFF),
                                  (unsigned long)(r2 >> 16));
                }
                break;
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
            case 'M':  // the CD-ROM's disc: a name, then Enter
                disc_line = true;
                disc_len = 0;
                Serial.println("# type the disc: an ISO's name, test (the test disc) or none (eject), then Enter");
                break;
            case 'E':  // set one setting in HDDSYNTH.INI: key = value, then Enter
                edit_line = true;
                edit_len = 0;
                Serial.println("# type key = value (e.g. device0 = GAMES.IMG or seek_timing = voice97), then Enter");
                break;
            case '!':  // restart, to apply HDDSYNTH.INI (keeps a `J` or `T` choice)
                Serial.println("# restarting");
                Serial.flush();
                delay(50);
                restart();
                break;
            case 'I': sd_card::createImage(sd_card::IMAGE_NAME, gen_disk::DEFAULT_SECTORS, Serial); break;
            case 'K': sd_card::createImage(sd_card::BENCH_IMAGE_NAME, gen_disk::DEFAULT_SECTORS, Serial); break;
            case 'J': {  // serve the other image after a restart (replug the adapter afterwards)
                bool bench = !benchRequested();
                watchdog_hw->scratch[0] = bench ? BENCH_MAGIC : 0;
                Serial.printf("# restarting to serve %s\n", bench ? sd_card::BENCH_IMAGE_NAME : sd_card::IMAGE_NAME);
                Serial.flush();
                delay(50);
                restart();
                break;
            }
            case 'T': {  // serve the test disk after a restart, or back (replug the adapter afterwards)
                bool test = !testDiskRequested();
                watchdog_hw->scratch[0] = test ? TEST_DISK_MAGIC : 0;
                Serial.printf("# restarting to serve %s\n", test ? "the test disk" : "the card's image or the welcome disk");
                Serial.flush();
                delay(50);
                restart();
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
        Serial.printf("DROP %lu\n", (unsigned long)(drops - g_reported_drops));
        g_reported_drops = drops;
    }
}
