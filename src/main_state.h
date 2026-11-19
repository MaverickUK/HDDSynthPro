// What main.cpp and console.cpp share. main.cpp defines the state (it holds
// both cores' loops, the disk choice and the services core 0 runs each pass);
// console.cpp holds the console and the lines core 0 prints. Included by those
// two only, after their other headers, which declare the types used here.
#pragma once

// ---- Every build ------------------------------------------------------------
extern SpscRing<TraceEvent, cfg::TRACE_RING_EVENTS> g_trace;
extern std::atomic<uint32_t> g_events;   // recorded by core 1
extern std::atomic<uint32_t> g_dropped;  // lost because the ring was full
extern std::atomic<uint32_t> g_stalls;   // PIO FIFO filled: accesses missed

// console.cpp
bool printLine(const char* line, size_t n);  // only if USB serial has room for it now
__attribute__((format(printf, 1, 2))) bool printfLine(const char* format, ...);  // the same, formatted
void printReady();
void handleConsole();
[[noreturn]] void restart();  // keeps `J`, `T` and `P`

#if FIRMWARE_SLICE == 0
// ---- The listen build -------------------------------------------------------
extern std::atomic<uint32_t> g_window;
extern std::atomic<uint32_t> g_skipped;  // accesses outside the window
#else
// ---- The drive builds -------------------------------------------------------

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
#if CMD_PROFILE
extern volatile uint32_t g_cmd_run_max;   // cycles
extern volatile uint32_t g_cmd_run_what;  // command | packet << 8
#endif

// Core 1's longest loop passes, in cycles (DWT CYCCNT), by what the pass did,
// for console `s` (`c1_gap_ns`): a pass that takes long is a register read
// left waiting. Racy across cores, as diagnostics may be; `r` clears it.
enum : uint8_t { GAP_ACCESS = 1, GAP_LANE, GAP_COMMAND, GAP_BUS_ONLY, GAP_STEP0 };  // steps 0-4 follow
constexpr unsigned GAP_KINDS = GAP_STEP0 + 5;
extern const char* const GAP_NAMES[GAP_KINDS];
extern volatile uint32_t g_c1_gap[GAP_KINDS];
extern volatile uint32_t g_c1_fifo_cycles, g_c1_fifo_max;  // the FIFO check alone (diagnostic)
extern volatile uint32_t g_c1_passes;
extern volatile uint32_t g_c1_iser[2], g_c1_primask;  // core 1's interrupt set-up, captured in setup1
extern uint32_t g_bench_pio, g_bench_ram, g_bench_sio, g_bench_nop;  // cycles per 1000, measured in setup1
extern uint32_t g_bench_pio_ram, g_bench_nop_ram;  // the same loops, from core 1's RAM code

// Flags and settings core 1 reads (SharedFlag, core1_ram.h) and the console sets.
extern SharedFlag g_trace_bus;     // console `t`
extern SharedFlag g_fast_lane;     // console `F` (debug)
extern SharedFlag g_read_ahead;    // HDDSYNTH.INI read_ahead; console `A` (debug)
extern SharedFlag g_write_behind;  // HDDSYNTH.INI write_behind; console `W`
extern SharedFlag g_absent;        // console `V`: the drive off the bus
extern std::atomic<uint32_t> g_command_starts;
extern std::atomic<uint8_t> g_mdma_mode;  // set by core 1 at start-up (setup1)
extern pio_plan::Plan g_pio_plan;
extern std::atomic<bool> g_pio_planned;  // start-up only

// What core 0 chooses at boot and core 1 waits for (g_disk_chosen, last): the
// disks' sizes and geometries, the modes offered, the bus program's switches.
extern std::atomic<uint32_t> g_disk_sectors;
extern std::atomic<bool> g_disk_chosen;  // set after g_disk_sectors: a CD-ROM with no disc has 0
extern std::atomic<uint32_t> g_geometry;          // cylinders << 16 | heads << 8 | sectors, or 0 for auto
extern std::atomic<uint32_t> g_device1_sectors;   // 0: no device 1
extern std::atomic<uint32_t> g_device1_geometry;
extern bool g_geometry_trimmed;       // the cylinders were cut to fit the disk (CONFIG says so)
extern const char* g_geometry_from;   // "set", "partition table" or "auto", for CONFIG
extern std::atomic<uint8_t> g_pio_mode;
extern std::atomic<uint8_t> g_mode_asked;  // HDDSYNTH.INI's mode, for core 1
extern SharedFlag g_dmarq_wired;           // found by core 1 at start-up
extern SharedFlag g_dma_auto;              // HDDSYNTH.INI dma: auto may offer DMA
extern SharedFlag g_early;    // HDDSYNTH.INI early_answers
extern SharedFlag g_bus_auto;      // bus = auto: core 1 switches early answers <-> the lane (docs/auto-bus.md)
extern SharedFlag g_bus_want_lane;  // core 0's advice for it
extern std::atomic<uint32_t> g_bus_switches;  // switches made, for `s`
extern std::atomic<uint32_t> g_bus_switch_us;
extern SharedFlag g_dma_when_learned;  // dma = auto
extern SharedFlag g_dma_added;         // multiword DMA added for a learned host
bool offerDmaToLearnedHost();          // main.cpp (core 0): false if it can't, or already has
extern SharedFlag g_bus_trace_switches;
extern SharedFlag g_table;    // HDDSYNTH.INI register_table, or console `P t`
extern SharedFlag g_isa;      // HDDSYNTH.INI isa
extern SharedFlag g_no_lane;  // a CD-ROM: ide_bus, never the data lane
extern SharedFlag g_iordy;    // HDDSYNTH.INI iordy
// A CD-ROM drive as device 0 or 1: plain bools, set before g_disk_chosen and read after it.
extern bool g_device0_cdrom;
extern bool g_device1_cdrom;
// Console `M`'s handshake with core 1 (main.cpp)
extern std::atomic<uint32_t> g_disc_seq;
extern std::atomic<uint32_t> g_disc_sectors;
extern std::atomic<uint32_t> g_disc_done;

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

// disks.cpp
extern bool g_test_disk;    // the generated disk: the bus observer can check its data
void chooseDisk();          // at boot, before core 1 is told (g_disk_chosen)
void serviceDiscChange();   // each pass of loop()

// A new hard disk at the host's hardware reset (docs/settings.md): with
// HDDSYNTH.INI's device0 changed (SET, console E), core 0 arms it; core 1, at
// RESET# asserted, asks for it; core 0 opens the new image (serviceResetSwap)
// and builds its IDENTIFY; core 1 takes it, holding the device in reset (BSY,
// as a drive spinning up) if RESET# is released first. Never at SRST.
enum ResetSwap : uint8_t { RESET_SWAP_NONE, RESET_SWAP_ARMED, RESET_SWAP_REQUESTED, RESET_SWAP_DONE };
extern std::atomic<uint8_t> g_reset_swap;
extern IdentifyInfo g_reset_swap_info;          // core 0 writes, before RESET_SWAP_DONE
extern uint16_t g_reset_swap_identify[256];
void serviceResetSwap();    // each pass of loop()

// link.cpp
extern bool g_busy;  // the activity light's state (main.cpp's updateLed); the link sends it
extern uint32_t g_activity_checked_us;
extern bool g_activity_unprinted;
extern activity_link::Op g_link_op;  // the last media command's
void beginLink();
void beginSeekTiming();
void serviceLink();
void serviceLinkRx();
void serviceLoopback();
// SynthMinder (synthminder.h, bay_service.cpp): a sentence for the synth,
// sent by serviceLink() (false with the link off); a line from the synth,
// for the answer being collected; the link and seek timing for STATUS.
bool relayToSynth(const char* line, size_t len);
void synthMinderLinkLine(const char* line, size_t len);
const char* linkState();
void seekTimingSummary(char* out, size_t size);
uint32_t linkRxLost();  // link bytes that found the RX ring full
// serveFetches(): a Fetch with `message` set. True with its result now, or
// false while a RECEIVE waits for the synth; synthMinderReady() then gives
// its id once the reply is in its buffer.
namespace ata { struct Fetch; }
bool synthMinderFetch(const ata::Fetch& fetch, bool& ok);
bool synthMinderReady(uint32_t& id);
extern bool g_synthminder_waiting;  // a RECEIVE is held: worth calling synthMinderReady()
void printSynthMinderStats(Print& out);  // for `s`
void printSeekConfig();  // console `S`'s seek timing lines
const SeekModel* seekModel(uint32_t now_ms);
void postHeadTarget(uint32_t lba, uint32_t sectors, activity_link::Op op, uint64_t at_us);

// Console `J`, `T` and `P`: what to serve, and the bus program, after the
// restart they trigger; in watchdog scratch registers 0 and 1, which a restart
// keeps and a power cycle clears.
constexpr uint32_t BENCH_MAGIC = 0x48444442;      // "HDDB"
constexpr uint32_t TEST_DISK_MAGIC = 0x48444454;  // "HDDT"
constexpr uint32_t BUS_MAGIC = 0x48445000;        // "HDP", then the program's letter
bool benchRequested();
bool testDiskRequested();

// The disks and the settings (core 0)
extern drive_config::Config g_config;
extern const char* g_config_state;  // "none", "present" or "truncated"
extern const char* g_image_name;    // the image being served
extern BlockSource* g_disk;
extern const char* g_disk_name;
extern uint32_t g_disk_ms;  // how long choosing the disk took at boot (console `s`)
extern BlockSource* g_device1_disk;
extern const char* g_device1_kind;  // "none", "image" or "folder"
void editConfig(const char* line);       // console `E`
void printConfig();                      // console `S`
const char* startDiscChange(const char* name, Print& out);  // console `M`: "" or why not
// SynthMinder's requests for the drive itself (bay_service.cpp; synthminder.h).
namespace synthminder { class Reply; }
void bayStatus(synthminder::Reply& reply);
const char* bayChangeDisc(const char* name);              // "" or why not
const char* baySet(const char* key, const char* value);  // "" or why not
void baySettings(synthminder::Reply& reply);           // $SBQ,1,SETTINGS

// The activity classifier, the link and seek timing (core 0)
extern activity::Classifier g_activity;
extern activity_link::Sender g_link;
extern activity_link::HeadPacer g_head;
extern bool g_link_on;  // HDDSYNTH.INI activity_link; console `L`
extern uint32_t g_link_rx_errors;  // framing, parity, break or overrun
extern seek_timing::SynthModel g_synth;
extern seek_timing::Scheduler g_seek;
extern bool g_seek_on;
void startLoopback();  // console `U`

// The bus program's advice and the host's resets (core 0)
extern auto_bus::Advisor g_auto_bus;
extern bool g_auto_bus_started;
extern uint32_t g_sectors_served;
extern uint32_t g_release_ms;
extern uint64_t g_no_cs_at_release;
extern bool g_no_cs_reported;
extern uint32_t g_no_cs_seen_ms;  // when the first was seen (| 1), until its line is printed
extern CommandHistory g_commands;
void setAbsent(bool on, const char* why);  // console `V`

// console.cpp: the lines core 0 prints unasked, once per pass of loop()
auto_bus::Counters autoBusCounters();
void printCardChanges();
void printAutoBus();
void printNoChipSelect();
void printIgnoredIordy();
// One SLOWREAD or EARLYDATA line, if one waits and USB serial has room: true if it printed one.
bool printSlowReads();
void printAccessLog();
#endif
