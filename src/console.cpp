// Core 0's console and the lines it prints unasked: the stats, the event lines
// (CARD, AUTOBUS, NOCS, NOIORDY, SLOWREAD), the solder-bridge test and the
// console's commands. Moved out of main.cpp (0.33.1-dev.4), which keeps the
// state they share (main_state.h), core 1's loops and core 0's loop().
// Nothing here runs on core 1.
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
#endif

#include "main_state.h"

// Prints a line only if USB serial can take it now. A blocked write would
// hold up core 0, and in the drive build core 0 must keep serving fetches;
// lines that can't be printed wait in the trace ring (and are counted as
// dropped if it fills).
bool printLine(const char* line, size_t n) {
    if (Serial && Serial.availableForWrite() < int(n)) return false;
    Serial.write(line, n);
    return true;
}

// The same for a formatted line: false, and nothing printed, if there's no
// room for it now. For lines core 0 prints unasked: Serial.printf waits up to
// a second for a host that has the port open and isn't reading, and fetches
// wait with it.
bool printfLine(const char* format, ...) {
    char line[200];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n <= 0) return true;
    return printLine(line, size_t(n) < sizeof line ? size_t(n) : sizeof line - 1);
}

#if FIRMWARE_SLICE != 0
// A CARD line when the card the image is on is pulled out, comes back, or a
// different card (or the image changed) comes back, which isn't served; and
// when the card stopped answering and was restarted between two looks here
// (counted, so a loss and return within one pass isn't missed: 4 Nov, two
// went unreported on the Evesham).
void printCardChanges() {
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
auto_bus::Counters autoBusCounters() {
    auto_bus::Counters c;
    c.dma_offered = g_mdma_mode.load(std::memory_order_relaxed) != NO_DMA;
    c.no_cs_known = g_pio_plan.bus_observer && !c.dma_offered;
    c.dior_no_cs = bus_observer::dmaWordsTotal();
    c.iordy_known = g_pio_plan.iordy_timer;
    c.iordy_ignored = iordy_timer::ignoredTotal();
    c.long_reads = iordy_timer::longTotal();
    c.sectors = g_sectors_served;
    c.cdrom = g_device0_cdrom || g_device1_cdrom;
    c.dma_by_auto = g_dma_added.load();
    return c;
}

// Console `Z` (debug, bus = auto): the wanted program flips every second,
// whatever the advice, so core 1's live switch is exercised under load.
static bool g_bus_flip = false;
static uint32_t g_dma_offered_ms = 0;  // an AUTODMA line to print (dma = auto)

// An AUTOBUS line when the advice changes: the program the drive would run
// now, why, and when since the reset. Every 100 ms is plenty.
void printAutoBus() {
    static uint32_t last_ms = 0;
    if (!g_pio_planned.load(std::memory_order_acquire)) return;
    uint32_t now = millis();
    if (!g_auto_bus_started) {
        g_auto_bus.reset(autoBusCounters(), now);
        g_auto_bus_started = true;
    }
    static bool unprinted = false;  // a change whose line is waiting for room in USB serial
    static uint32_t changed_ms = 0;
    if (now - last_ms >= 100) {
        last_ms = now;
        if (g_auto_bus.update(autoBusCounters(), now)) {
            unprinted = true;
            changed_ms = now;
        }
        // bus = auto: core 1 switches to what the advice says (main.cpp); and
        // while there's no proof the host waits for IORDY, early answers hold a
        // Status answer now and then so there will be (ide_bus::requestProbe).
        // Debug (console `Z`): a BUSSWITCH line for each switch, with the raw
        // trace on from just before it until 30 ms after.
        static uint32_t seen_switches = 0, switch_ms = 0;
        uint32_t switches = g_bus_switches.load(std::memory_order_relaxed);
        if (switches != seen_switches) {
            seen_switches = switches;
            switch_ms = now;
            if (g_bus_flip)
                Serial.printf("BUSSWITCH %lu to %s at %lu\n", (unsigned long)switches, ide_bus::busName(),
                              (unsigned long)g_bus_switch_us.load(std::memory_order_relaxed));
        }
        if (g_bus_trace_switches.load() && now - switch_ms > 30 && g_trace_bus.load()) g_trace_bus.store(false);
        if (g_bus_auto.load() && g_bus_flip) {  // console `Z`: the switch tested, every second
            static uint32_t flip_ms = 0;
            if (now - flip_ms >= 1000) {
                flip_ms = now;
                g_bus_want_lane.store(!g_bus_want_lane.load());
            }
        } else if (g_bus_auto.load()) {
            g_bus_want_lane.store(g_auto_bus.choice() == auto_bus::Choice::Lane);
            // dma = auto: a host that earned the lane isn't an ISA card
            if (g_dma_when_learned.load() && g_auto_bus.why() == auto_bus::Why::Ok && offerDmaToLearnedHost())
                g_dma_offered_ms = now;

        }
    }
    if (g_dma_offered_ms && printfLine("AUTODMA mdma%u offered %lu ms after reset: the PC learned; IDENTIFY says so from the next time it's read\n",
                                       unsigned(MAX_MDMA_MODE), (unsigned long)g_auto_bus.sinceResetMs(g_dma_offered_ms)))
        g_dma_offered_ms = 0;
    // bus = auto: while there's no proof the host waits for IORDY, early
    // answers hold a Status read now and then so there will be
    // (ide_bus::requestProbe), every IORDY_PROBE_EVERY_MS: 20 long reads in
    // a fraction of a second, so the PC is learned (and DMA offered) before
    // Windows' driver reads IDENTIFY.
    static uint32_t probe_ms = 0;
    if (g_bus_auto.load() && !g_bus_flip && g_auto_bus.why() == auto_bus::Why::NoProof && !ide_bus::dataLaneOn() &&
        !ide_bus::probePending() && now - probe_ms >= cfg::IORDY_PROBE_EVERY_MS) {
        probe_ms = now;
        ide_bus::requestProbe();
    }
    if (!unprinted) return;
    if (printfLine("AUTOBUS %s (%s) %lu ms after reset: %lu sectors, %lu long reads waited; running %s\n",
                   g_auto_bus.choice() == auto_bus::Choice::Lane ? "lane" : "early",
                   auto_bus::whyName(g_auto_bus.why()), (unsigned long)g_auto_bus.sinceResetMs(changed_ms),
                   (unsigned long)g_auto_bus.sectors(), (unsigned long)g_auto_bus.longReads(),
                   (ide_table::running() ? "table" : ide_bus::busName())))
        unprinted = false;
}

// A NOCS line the first time after each RESET# release (or boot) that the host
// strobes DIOR# with no chip select, while the drive offers no DMA: how soon
// an ISA host shows itself (g_release_ms above).
void printNoChipSelect() {
    if (g_no_cs_reported || g_mdma_mode.load(std::memory_order_relaxed) != NO_DMA) return;
    if (bus_observer::dmaWords() == g_no_cs_at_release) return;
    if (!g_no_cs_seen_ms) g_no_cs_seen_ms = millis() | 1;  // the line may wait for room in USB serial
    if (printfLine("NOCS %lu ms after RESET# release: DIOR# with no chip select (an ISA card?)\n",
                   (unsigned long)(g_no_cs_seen_ms - g_release_ms)))
        g_no_cs_reported = true;
}

// A NOIORDY line when the host ends reads without waiting for IORDY, at most
// once a second: such a host reads garbage whenever core 1 is slower than its
// strobe (iordy_check.pio). It names the command the last one fell in or
// followed, its device, and how long after it started (and, if it had
// finished, after its DONE): to tell a block's first word, after a fetch,
// from the rest (docs/atapi.md, the Evesham's CD reads). The times are when
// core 0 collected the report, within a loop pass of the read (up to about
// 0.5 ms behind it while it stores a sector).
void printIgnoredIordy() {
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
bool printSlowReads() {
    static iordy_timer::OverLimit pending;
    static bool has_pending = false;
    if (!has_pending && !iordy_timer::takeOverLimit(pending)) return false;
    has_pending = true;
    char line[48];
    int n = pending.ns ? std::snprintf(line, sizeof line, "SLOWREAD %lu %s %lu\n", (unsigned long)pending.at_us,
                                       pending.reg, (unsigned long)pending.ns)
                       : std::snprintf(line, sizeof line, "EARLYDATA %lu\n", (unsigned long)pending.at_us);
    if (n <= 0 || !printLine(line, size_t(n) < sizeof line ? size_t(n) : sizeof line - 1)) return false;
    has_pending = false;
    return true;
}

// HDDPRO_ACCESS_LOG (access_log.h): core 1's work before a late read, as
// ACCLOG lines, oldest first, each with how long before the late answer it
// began. A line at a time, so a busy USB link never holds core 0 up.
void printAccessLog() {
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

void printReady() {
    Serial.printf("READY HDDSynthPro %s slice%d\n", FIRMWARE_VERSION, FIRMWARE_SLICE);
}

// Current level of every bus line: a wiring check. With the adapter powered
// and idle, expect DIOR#, DIOW#, CS0#, CS1#, RESET# and IORDY high, INTRQ and
// DD7 low (host pull-downs), and the other DD lines floating.
static void printPins() {
    uint32_t s = gpio_get_all() >> cfg::PIN_DD0;  // as core 1 samples the bus
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
    // The most each core's stack has held, of 4,096 bytes (core 1: 2,048 as the SDK set it up).
    Serial.printf(" stack0_max=%lu stack1_max=%lu", (unsigned long)stack_watch::used(0),
                  (unsigned long)stack_watch::used(1));
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
    if (ide_table::running())
        Serial.printf(" table_in=%lu table_out=%lu", (unsigned long)ide_table::blocksIn(), (unsigned long)ide_table::blocksOut());
    Serial.printf(" autobus=%s autobus_why=%s autobus_sectors=%lu autobus_long=%lu",
                  g_auto_bus.choice() == auto_bus::Choice::Lane ? "lane" : "early", auto_bus::whyName(g_auto_bus.why()),
                  (unsigned long)g_auto_bus.sectors(), (unsigned long)g_auto_bus.longReads());
    Serial.printf(" bus_auto=%s bus_switches=%lu iordy_probes=%lu", g_bus_auto.load() ? "on" : "off",
                  (unsigned long)g_bus_switches.load(std::memory_order_relaxed), (unsigned long)ide_bus::probes());
    Serial.printf(" dma_auto=%s", !g_dma_when_learned.load() ? "off" : g_dma_added.load() ? "offered" : "waiting");
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
    printSynthMinderStats(Serial);
    const SeekModel* model = g_seek.active() ? &g_seek.model() : nullptr;
    uint32_t commands = g_seek.commands();
    Serial.printf(" seek_timing=%s seek_active=%d seek_rpm=%ld seek_actuator=%s", seek_timing::settingName(g_config.seek_timing),
                  int(model != nullptr), model ? lroundf(model->rpm) : 0L, model ? seek_timing::actuatorName(*model) : "-");
    Serial.printf(" seek_commands=%lu seek_held=%lu seek_avg_us=%lu seek_max_us=%lu seek_total_us=%llu seek_capped=%lu",
                  (unsigned long)commands, (unsigned long)g_seek.held(),
                  (unsigned long)(commands ? g_seek.totalUs() / commands : 0), (unsigned long)g_seek.maxUs(),
                  (unsigned long long)g_seek.totalUs(), (unsigned long)g_seek.capped());
    Serial.printf(" seek_cache_kb=%lu seek_track_sectors=%lu seek_hits=%lu seek_cached_writes=%lu seek_flushes=%lu seek_pending=%lu",
                  (unsigned long)(model ? g_seek.cacheSectors() / 2 : 0), (unsigned long)(model ? g_seek.trackSectors() : 0),
                  (unsigned long)g_seek.hits(), (unsigned long)g_seek.cachedWrites(), (unsigned long)g_seek.flushes(),
                  (unsigned long)g_seek.pendingSectors());
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
void restart() {
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}


void handleConsole() {
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
                    startDiscChange(name, Serial);
                else
                    Serial.println("# DISC: nothing changed");
            } else if (disc_len + 1 < sizeof disc_buf) {
                disc_buf[disc_len++] = char(c);
            }
            continue;
        }
        static bool bus_letter_next = false;  // `P` then a, e, l, i, t or 0
        if (bus_letter_next) {
            bus_letter_next = false;
            if (c == 'e' || c == 'l' || c == 'i' || c == 't' || c == 'a' || c == '0') {
                watchdog_hw->scratch[1] = c == '0' ? 0 : BUS_MAGIC | uint32_t(c);
                Serial.printf("# restarting with %s\n", c == 'a'   ? "the bus program chosen automatically"
                                                        : c == 'e' ? "early answers"
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
                               "m mark, S SD card and settings, G card read speed, C create HDDSYNTH.INI, E set a setting, ! restart, "
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
            case 'D': delay_digit_next = true; break;  // debug: delay data reads
            case 'P': bus_letter_next = true; break;  // a bus program until a power cycle
            case 'Z':  // debug, bus = auto: flip the program every second
                g_bus_flip = !g_bus_flip;
                Serial.printf("BUSFLIP %s%s\n", g_bus_flip ? "on" : "off",
                              g_bus_auto.load() ? "" : " (needs bus = auto or console P a)");
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
                if (ide_table::running()) {  // the table never waits for core 0: its ring would overrun
                    bus_observer::setFull(false);
                    Serial.printf("OBSERVER spot (full checks need a bus program that waits for core 0)\n");
                    break;
                }
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
            case 'G':  // the card's read speed on the served image
                sd_card::benchReads(Serial);
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
