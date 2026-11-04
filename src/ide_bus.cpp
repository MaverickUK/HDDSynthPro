#include "ide_bus.h"

#include <atomic>
#include <cstdio>

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/structs/m33.h>

#include "ata_device.h"
#include "drive_device.h"
#if HDDPRO_CDROM
#include "atapi_device.h"
#elif HDDPRO_HDCD
#include "hdcd_device.h"
#endif
#include "bus_sample.h"
#include "config.h"
#include "core1_ram.h"
#include "fetch_mailbox.h"
#include "ide_bus.pio.h"
#include "iordy_watchdog.pio.h"

namespace ide_bus {

static PIO s_pio;
static uint s_sm;
static uint s_offset;

static constexpr uint32_t DRIVEN_PINS = (0xFFFFu << cfg::PIN_DD0) | (1u << cfg::PIN_IORDY);

// The register an access is for, looked up from its sample's DA2-0, CS0# and
// CS1# bits (5 consecutive pins), so serviceOnce() needn't decode them.
static_assert(cfg::PIN_CS0_N == cfg::PIN_DA0 + 3 && cfg::PIN_CS1_N == cfg::PIN_DA0 + 4,
              "DA2-0, CS0#, CS1# must be consecutive");
static constexpr unsigned REG_INDEX_SHIFT = bus::bitOf(cfg::PIN_DA0);
static ata::Reg s_reg_for[32];
// The upper half of each read's answer: which DD lines the bus program drives
// (ide_bus.pio). All for the drive's own registers, none for anything else.
static constexpr uint32_t DRIVE_DD = 0xFFFF0000u;
static uint32_t s_drive_for[32];
static std::atomic<uint32_t> s_data_delay_cycles{0};  // setDataReadDelay
static bool s_early = false;    // ide_bus_early: answers asked for at the chip select (begin)
static const char* s_bus_name = "ide_bus";  // which program serves the bus (busName)
static uint s_idle_offset = 0;  // where the program's `idle` is, for restart()

// ide_bus_early's answer requests have bits 21-31 zero, so both strobe bits
// read as asserted, which no real access shows.
static constexpr uint32_t STROBE_BITS = bus::maskOf(cfg::PIN_DIOR_N) | bus::maskOf(cfg::PIN_DIOW_N);

static unsigned regIndex(uint32_t sample) { return (sample >> REG_INDEX_SHIFT) & 31u; }

// ide_bus_early: the address (DA2-0, CS0#, CS1#) of the last answer request.
// A strobe only starts for the address the program asked about (it asks
// again if the address changes first), so a read's report or a write's
// sample is that address's, whatever its own sample shows: an Amiga 1200's
// Gayle drops the chip select early in the strobe, so a report taken 50 ns in
// showed none, and data reads went uncounted (2026-10-31).
static constexpr uint32_t ADDRESS_BITS = 31u << REG_INDEX_SHIFT;
static uint32_t s_asked = 0;

// ide_bus_early's IORDY held with no strobe (cfg::IORDY_STALL_US): when core 1
// answered the last request, whether a report or write has come since, and
// the limit in cycles. Not for ide_bus_noiordy, which never drives IORDY.
static uint32_t s_armed_at = 0;
static bool s_armed = false;
static bool s_stall_release = false;  // ide_bus_early (begin)
static uint32_t s_stall_cycles = 0;
static volatile uint32_t s_stall_releases = 0;

// A sample from ide_bus_early with the address it belongs to: requests set
// s_asked, everything else takes it. A request with neither chip select
// asserted can't be an access (ide_bus_noiordy asks again after Gayle's lines
// change, and a write starting then finds them garbage), so it leaves s_asked.
static constexpr uint32_t CS_BITS = bus::maskOf(cfg::PIN_CS0_N) | bus::maskOf(cfg::PIN_CS1_N);
__attribute__((always_inline)) static inline uint32_t earlyAddress(uint32_t sample) {
    if ((sample & STROBE_BITS) == 0) {
        if ((sample & CS_BITS) != CS_BITS) s_asked = sample & ADDRESS_BITS;
        s_armed_at = m33_hw->dwt_cyccnt;
        s_armed = true;
        return sample;
    }
    s_armed = false;
    return (sample & ~ADDRESS_BITS) | s_asked;
}

// IORDY held too long with no strobe: one instruction run in the program
// sets its side-set (IORDY's direction) to released, without moving it on.
// A strobe after it pulls IORDY low again for the moment the answer takes.
__attribute__((always_inline)) static inline void stallCheck() {
    if (s_armed && m33_hw->dwt_cyccnt - s_armed_at > s_stall_cycles) {
        s_armed = false;
        if (s_stall_release) {
            pio_sm_exec(s_pio, s_sm, pio_encode_nop() | pio_encode_sideset_opt(1, 0));
            s_stall_releases = s_stall_releases + 1;
        }
    }
}

CORE1_RAM void checkIordyStall() { stallCheck(); }
uint32_t iordyStallReleases() { return s_stall_releases; }

// ---- The data lane (ide_bus_data and ide_data_in, docs/data-lane.md) ----------
//
// The default bus (`early_answers = off`, `isa = off`): data register reads
// are answered by a second state machine from its TX FIFO, which a DMA channel
// fills from the device's block, so they never reach core 1. Core 1's part is
// once per block:
//  1. Start: when the device has a data-in block (IDENTIFY, a sector), start
//     the channel on it, before the host can see DRQ: laneSync runs before
//     every register read is answered and before INTRQ is updated.
//  2. Move on early: once the channel has moved the block's last word, every
//     word the host has still to read is in ide_data_in's FIFO, so the buffer
//     is free and the device can move on (the next sector's DRQ, BSY, or the
//     end) while the host reads the last few: on the USB adapter about 4 us,
//     time core 1 would otherwise spend after the block with the host's status
//     read waiting (0.29.0-2d.4: up to 1.1 us, which held reads past 1.25 us).
//     Until the host has read the last word, STATUS still shows the block's
//     DRQ, reading it acknowledges nothing, and INTRQ waits (s_lane_ahead).
//  3. Finish: when ide_data_in has taken the spare word the channel adds after
//     the block (it does only once the last word's strobe has ended), start
//     the device's next block, if it has one: a few register writes.
static bool s_lane = false;            // begin
static uint s_data_sm = 0;             // ide_data_in's state machine, in s_pio
static uint s_data_offset = 0;
static uint32_t s_data_start = 0;      // `jmp` to its start, for laneDataSmReset
static uint s_lane_dma = 0;            // the channel feeding it
static const uint16_t* s_lane_words = nullptr;  // the block being served, or nullptr
bool lane_busy = false;                // s_lane_words != nullptr, for core 1's loop (ide_bus.h)
static uint16_t s_lane_count = 0;
static bool s_lane_ahead = false;      // 2 done: the device has moved on, the host hasn't
static uint8_t s_lane_shown = 0;       // STATUS meanwhile: the block's own
static constexpr uint LANE_IRQ = 4;    // ide_bus_data -> ide_data_in: a data read
// The data register's address as ide_bus_data reads it (DA2-0, CS0#, CS1#),
// for its Y register: DA2-0 = 0, CS0# low, CS1# high.
static constexpr uint LANE_DATA_ADDRESS = bus::maskOf(cfg::PIN_CS1_N) >> REG_INDEX_SHIFT;
static_assert(LANE_DATA_ADDRESS == 16, "set y takes 5 bits");

// The longest core 1 took over step 3 (a status read arriving meanwhile
// waits for it), in cycles, and the blocks served: `s`'s lane_switch_max_ns
// and lane_blocks; `r` clears the first.
static volatile uint32_t s_lane_switch_max = 0, s_lane_blocks = 0;

// ide_data_in back at its start, waiting for a word, its FIFO empty and IRQ 4
// clear: before each block, and when one is cut short. The register writes
// the SDK's pio_sm_set_enabled, _clear_fifos, _restart and _exec make, written
// out: GCC may leave those out of line, in flash, as code around them moves.
__attribute__((always_inline)) static inline void laneDataSmReset() {
    hw_clear_bits(&s_pio->ctrl, 1u << s_data_sm);                               // stop it
    hw_xor_bits(&s_pio->sm[s_data_sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);  // changing the FIFO
    hw_xor_bits(&s_pio->sm[s_data_sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);  // join empties both
    hw_set_bits(&s_pio->ctrl, 1u << (PIO_CTRL_SM_RESTART_LSB + s_data_sm));     // OSR, stalls cleared
    s_pio->sm[s_data_sm].instr = s_data_start;                                   // jmp to its start
    s_pio->irq = 1u << LANE_IRQ;                                                 // IRQ 4 clear (write 1)
    hw_set_bits(&s_pio->ctrl, 1u << s_data_sm);                                 // and go
}

__attribute__((always_inline)) static inline bool laneDmaBusy() {
    return dma_hw->ch[s_lane_dma].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS;
}

// ide_data_in has taken the spare word: the host has read the whole block.
__attribute__((always_inline)) static inline bool laneFifoEmpty() {
    return s_pio->fstat & (1u << (PIO_FSTAT_TXEMPTY_LSB + s_data_sm));
}

__attribute__((always_inline)) static inline void laneStop() {
    // dma_channel_abort(), inline: the channel stops once in-flight transfers retire
    dma_hw->abort = 1u << s_lane_dma;
    while (laneDmaBusy()) {
    }
    laneDataSmReset();
    s_lane_words = nullptr;
    lane_busy = false;
    s_lane_ahead = false;
}

__attribute__((always_inline)) static inline void laneStart(const uint16_t* words, uint16_t count) {
    laneDataSmReset();
    dma_channel_set_read_addr(s_lane_dma, words, false);
    dma_channel_set_trans_count(s_lane_dma, uint32_t(count) + 1, true);  // and the spare word; go
    s_lane_words = words;
    lane_busy = true;
    s_lane_count = count;
    s_lane_blocks = s_lane_blocks + 1;
}

// Something for laneSync to do: step 2 or 3 is due (cheap: one register read).
__attribute__((always_inline)) static inline bool laneDue() {
    return s_lane_ahead ? laneFifoEmpty() : !laneDmaBusy();
}

// Keeps the lane in step with the device: steps 1-3 as they fall due, and a
// block the device has dropped (a reset, an aborted command) is dropped too.
CORE1_RAM __attribute__((noinline)) static void laneSyncSlow(DriveDevice& dev) {
    const uint16_t* next;
    uint16_t left;
    if (s_lane_words && !s_lane_ahead) {
        if (!dev.dataInRemaining(next, left) || next != s_lane_words || left != s_lane_count) {
            laneStop();  // the device dropped the block
        } else {
            if (laneDmaBusy()) return;  // still moving words into the FIFO
            // Step 2: every word is queued, so the device can move on.
            s_lane_shown = uint8_t(dev.peek(ata::Reg::StatusCommand));
            dev.consumeDataWords(left);
            s_lane_ahead = true;
        }
    }
    if (s_lane_ahead) {
        if (!laneFifoEmpty()) return;  // the host is reading the last words
        s_lane_ahead = false;           // step 3: it has read them
        s_lane_words = nullptr;
        lane_busy = false;
    }
    if (s_lane_words || !dev.dataInRemaining(next, left)) return;
    const uint32_t t0 = m33_hw->dwt_cyccnt;
    laneStart(next, left);  // step 1 (or 3): the device's next block
    const uint32_t took = m33_hw->dwt_cyccnt - t0;
    if (took > s_lane_switch_max) s_lane_switch_max = took;
}

// Before a register read is answered: a step that has fallen due (the block
// done, or one to start), and nothing else, so it costs a flag test or one
// register read. A block the device has dropped is noticed by laneSyncFull,
// after the command that dropped it; until then its status shows as it is.
// (Checking for that here too held status reads past 1.25 us: 0.29.0-2d.4,
// about 1,400 a speed run, the adapter's first poll after each READ.)
__attribute__((always_inline)) static inline void laneSync(DriveDevice& dev) {
    if (!s_lane) return;
    if (s_lane_words) {
        // Only a block the host has read (step 3, or 2 and 3 if core 1 hadn't
        // got to step 2): step 2 alone isn't needed for the answer, since the
        // device still shows the block's DRQ, and doing it here held the read
        // (Device register reads during IDENTIFY, 1.5 us, 0.29.0-2d.4).
        if (laneFifoEmpty() && !laneDmaBusy()) laneSyncSlow(dev);
        return;
    }
    // A block to start needs DRQ, so test that first: one load when idle,
    // where dataInRemaining() takes four (this runs on every register read;
    // the lane's idle status polls sat 10 ns over 1.25 us, 0.29.0-2d.4). DRQ
    // alone isn't enough: a write's data-out phase has it too, and calling
    // laneSyncSlow on its status polls held them 1.44 us.
    const uint16_t* next;
    uint16_t left;
    if ((dev.status() & ata::ST_DRQ) && dev.dataInRemaining(next, left)) laneSyncSlow(dev);
}

// After a command or a fetch: everything, a dropped block included.
__attribute__((always_inline)) static inline void laneSyncFull(DriveDevice& dev) {
    if (!s_lane) return;
    const uint16_t* next;
    uint16_t left;
    if (s_lane_words || dev.dataInRemaining(next, left)) laneSyncSlow(dev);
}

// A register read while the device has moved on (step 2) but the host hasn't
// finished the block: STATUS and Alternate Status show the block's DRQ, and
// reading them changes nothing. Returns false for anything else.
__attribute__((always_inline)) static inline bool laneShownStatus(ata::Reg reg, uint32_t& value) {
    if (!s_lane_ahead || (reg != ata::Reg::StatusCommand && reg != ata::Reg::AltStatusDevCtrl)) return false;
    value = s_lane_shown;
    return true;
}

uint32_t laneSwitchMaxNs() { return uint32_t(uint64_t(s_lane_switch_max) * 1000000000ull / clock_get_hz(clk_sys)); }
uint32_t laneSwitches() { return s_lane_blocks; }
void resetLaneStats() { s_lane_switch_max = 0; }

CORE1_RAM bool dataLaneOn() { return s_lane; }
const char* busName() { return s_bus_name; }

CORE1_RAM bool syncDataLane(DriveDevice& dev) {
    laneSyncFull(dev);
    updateIntrq(dev);
    return s_lane_words != nullptr;
}

// Diagnostic, built in only when true: where core 1 was when an early answer
// came late, meaning the host's DIOR# was already low as core 1 answered the
// request, so it was waiting on IORDY. Each late answer is charged to the
// last job core 1 started since it last found the RX FIFO empty, with how
// long ago that was (an upper bound on the request's wait). `s` prints it
// (early_late=); `r` clears it.
constexpr bool EARLY_PROFILE = false;
enum Site : uint8_t {
    SITE_MAIN,        // loop1's main loop, outside the tight loops
    SITE_POLL,        // polling the FIFO, nothing else
    SITE_SERVE,       // serving the access before
    SITE_TRACE,       // recording an access (raw trace)
    SITE_COMMAND,     // running a command just written
    SITE_FETCH_POST,  // posting a fetch to core 0
    SITE_FETCH_DONE,  // taking core 0's result (fetchDone, INTRQ)
    SITE_TIMERS,      // the BSY timers and DONE lines
    SITE_STALL,       // the IORDY stall check
    SITE_LANE_DATA,   // the data lane, after a data read (read-ahead, results)
    SITE_LANE_OTHER,  // the data lane, another access (catching up, serving it)
    SITE_COUNT
};
static const char* const SITE_NAMES[SITE_COUNT] = {"main", "poll", "serve", "trace", "command", "fetch_post",
                                                   "fetch_done", "timers", "stall", "lane_data", "lane_other"};
static volatile uint8_t s_site = SITE_MAIN;
static volatile uint32_t s_empty_at = 0;
static volatile uint32_t s_requests = 0, s_late[SITE_COUNT], s_late_max[SITE_COUNT];
static volatile uint32_t s_late_over[SITE_COUNT];  // late, and over 1 us since the FIFO was last empty
static volatile uint32_t s_reg_late[SITE_COUNT];   // late answers for registers other than DATA
static volatile uint32_t s_reg_requests = 0;
// How long each job ran: from its site() to the next site() or fifoEmpty().
static volatile uint32_t s_site_at = 0;
static volatile uint32_t s_job_over[SITE_COUNT], s_job_max[SITE_COUNT];  // over 1 us; the longest

__attribute__((always_inline)) static inline void nextJob(Site s, uint32_t now) {
    uint8_t was = s_site;
    uint32_t took = now - s_site_at;
    if (took > s_job_max[was]) s_job_max[was] = took;
    if (took > 200) s_job_over[was] = s_job_over[was] + 1;  // 1 us at 200 MHz
    s_site = s;
    s_site_at = now;
}
__attribute__((always_inline)) static inline void site(Site s) {
    if constexpr (EARLY_PROFILE) nextJob(s, m33_hw->dwt_cyccnt);
}
__attribute__((always_inline)) static inline void fifoEmpty(Site s) {
    if constexpr (EARLY_PROFILE) {
        uint32_t now = m33_hw->dwt_cyccnt;
        s_empty_at = now;
        nextJob(s, now);
    }
}
// Just after answering a request.
__attribute__((always_inline)) static inline void answered(bool data = true) {
    if constexpr (EARLY_PROFILE) {
        s_requests = s_requests + 1;
        if (!data) s_reg_requests = s_reg_requests + 1;
        if (gpio_get(cfg::PIN_DIOR_N)) return;  // no strobe yet: in time
        uint8_t at = s_site;
        if (!data) s_reg_late[at] = s_reg_late[at] + 1;
        uint32_t waited = m33_hw->dwt_cyccnt - s_empty_at;
        s_late[at] = s_late[at] + 1;
        if (waited > s_late_max[at]) s_late_max[at] = waited;
        if (waited > 200) s_late_over[at] = s_late_over[at] + 1;  // 1 us at 200 MHz
    }
}

// Core 1's own interrupt set-up (core-local registers), captured on core 1.
static volatile uint32_t s_c1_iser0 = 0, s_c1_iser1 = 0, s_c1_systick = 0, s_c1_primask = 0, s_c1_captured = 0;
__attribute__((always_inline)) static inline void captureCore1Irqs() {
    if constexpr (EARLY_PROFILE) {
        if (s_c1_captured) return;
        s_c1_iser0 = m33_hw->nvic_iser[0];
        s_c1_iser1 = m33_hw->nvic_iser[1];
        s_c1_systick = m33_hw->syst_csr;
        uint32_t primask;
        __asm volatile("mrs %0, primask" : "=r"(primask));
        s_c1_primask = primask;
        s_c1_captured = 1;
    }
}

size_t formatEarlyProfile(char* buf, size_t size) {
    if constexpr (!EARLY_PROFILE) return 0;
    {
        int k = snprintf(buf, size, " c1_irq=iser0:%08lx,iser1:%08lx,systick:%08lx,primask:%lu",
                         (unsigned long)s_c1_iser0, (unsigned long)s_c1_iser1, (unsigned long)s_c1_systick,
                         (unsigned long)s_c1_primask);
        if (k <= 0 || size_t(k) >= size) return 0;
        buf += k;
        size -= size_t(k);
    }
    const uint32_t mhz = clock_get_hz(clk_sys) / 1000000;
    int n = snprintf(buf, size, " early_requests=%lu early_reg_requests=%lu early_reg_late=",
                     (unsigned long)s_requests, (unsigned long)s_reg_requests);
    bool any_reg = false;
    for (unsigned i = 0; i < SITE_COUNT && n > 0 && size_t(n) < size; ++i) {
        if (!s_reg_late[i]) continue;
        n += snprintf(buf + n, size - n, "%s%s:%lu", any_reg ? "," : "", SITE_NAMES[i], (unsigned long)s_reg_late[i]);
        any_reg = true;
    }
    if (n > 0 && size_t(n) < size) n += snprintf(buf + n, size - n, " early_late=");
    bool any = false;
    for (unsigned i = 0; i < SITE_COUNT && n > 0 && size_t(n) < size; ++i) {
        if (!s_late[i]) continue;
        n += snprintf(buf + n, size - n, "%s%s:%lu/%lu/%luns", any ? "," : "", SITE_NAMES[i],
                      (unsigned long)s_late[i], (unsigned long)s_late_over[i],
                      (unsigned long)(s_late_max[i] * 1000 / mhz));
        any = true;
    }
    if (!any && n > 0 && size_t(n) < size) n += snprintf(buf + n, size - n, "none");
    if (n > 0 && size_t(n) < size) n += snprintf(buf + n, size - n, " early_jobs=");
    any = false;
    for (unsigned i = 0; i < SITE_COUNT && n > 0 && size_t(n) < size; ++i) {
        if (!s_job_max[i]) continue;
        n += snprintf(buf + n, size - n, "%s%s:%lu/%luns", any ? "," : "", SITE_NAMES[i],
                      (unsigned long)s_job_over[i], (unsigned long)(s_job_max[i] * 1000 / mhz));
        any = true;
    }
    return n > 0 && size_t(n) < size ? size_t(n) : 0;
}

void resetEarlyProfile() {
    s_requests = 0;
    for (unsigned i = 0; i < SITE_COUNT; ++i)
        s_late[i] = s_late_over[i] = s_late_max[i] = s_job_over[i] = s_job_max[i] = s_reg_late[i] = 0;
    s_reg_requests = 0;
}

// A read of the data register, as the PIO samples it: DIOR# low, DIOW# high,
// CS0# low, CS1# high, DA2-0 = 0.
static constexpr uint32_t ACCESS_BITS = (31u << REG_INDEX_SHIFT) | bus::maskOf(cfg::PIN_DIOR_N) |
                                        bus::maskOf(cfg::PIN_DIOW_N);
static constexpr uint32_t DATA_READ = bus::maskOf(cfg::PIN_CS1_N) | bus::maskOf(cfg::PIN_DIOW_N);

void begin(bool isa, bool early, bool iordy) {
    s_early = early;
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t sample = i << REG_INDEX_SHIFT;
        bus::Block block = bus::block(sample);
        s_reg_for[i] = ata::regFor(block == bus::Block::Command, block == bus::Block::Control,
                                   bus::address(sample));
        s_drive_for[i] = s_reg_for[i] == ata::Reg::None ? 0 : DRIVE_DD;
    }
    for (unsigned pin = cfg::BUS_FIRST_PIN; pin <= cfg::BUS_LAST_PIN; ++pin) {
        if (pin == cfg::PIN_BOARD_LED) continue;
        gpio_init(pin);           // input until claimed below
        gpio_disable_pulls(pin);  // never internal pull-downs (RP2350 erratum E9)
    }
    // INTRQ: SIO, released until the device drives it (updateIntrq).
    gpio_put(cfg::PIN_INTRQ, 1);
    gpio_set_dir(cfg::PIN_INTRQ, false);

    // early_answers with iordy = off: ide_bus_noiordy, for hosts whose address
    // lines change as the strobe starts (an Amiga 1200), and that never use IORDY
    const bool noiordy = early && !iordy;
    // The data lane, unless early answers or an ISA card's watchdog need the block
    const bool lane = !early && !isa;
    s_lane = lane;
    s_lane_words = nullptr;
    s_stall_release = early && iordy;
    s_armed = false;
    s_stall_cycles = cfg::IORDY_STALL_US * (clock_get_hz(clk_sys) / 1000000);
    const pio_program* program = noiordy ? &ide_bus_noiordy_program
                                 : early ? &ide_bus_early_program
                                 : lane  ? &ide_bus_data_program
                                         : &ide_bus_program;
    s_bus_name = noiordy ? "noiordy" : early ? "early" : lane ? "lane" : "ide_bus";
    s_idle_offset = noiordy ? ide_bus_noiordy_offset_idle
                    : early ? ide_bus_early_offset_idle
                    : lane  ? ide_bus_data_offset_idle
                            : ide_bus_offset_idle;
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        program, &s_pio, &s_sm, &s_offset, cfg::BUS_FIRST_PIN,
        cfg::BUS_LAST_PIN - cfg::BUS_FIRST_PIN + 1, true);
    hard_assert(ok);

    // DD0-15 and IORDY are driven by PIO. Both start as inputs (released);
    // IORDY's output value is 0, so making it an output pulls it low.
    for (unsigned i = 0; i < 16; ++i) pio_gpio_init(s_pio, cfg::PIN_DD0 + i);
    if (iordy) pio_gpio_init(s_pio, cfg::PIN_IORDY);  // else an SIO input: the side-set can't reach it
    // early_answers: DD at 8 mA. A host that ignores IORDY latches the first
    // word of a sector from lines it left floating high, and on the Pentium
    // DD12 sometimes hadn't fallen at 4 mA (0.23.13: word 0, bit 12 set).
    // Not otherwise: on the USB adapter's DMA, 8 mA with fast slew made the
    // lines noisier (0.17.1).
    if (early)
        for (unsigned i = 0; i < 16; ++i) gpio_set_drive_strength(cfg::PIN_DD0 + i, GPIO_DRIVE_STRENGTH_8MA);
    pio_sm_set_pins_with_mask(s_pio, s_sm, 0, 1u << cfg::PIN_IORDY);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 0, DRIVEN_PINS);

    pio_sm_config c = noiordy ? ide_bus_noiordy_program_get_default_config(s_offset)
                      : early ? ide_bus_early_program_get_default_config(s_offset)
                      : lane  ? ide_bus_data_program_get_default_config(s_offset)
                              : ide_bus_program_get_default_config(s_offset);
    sm_config_set_in_pins(&c, cfg::PIN_DD0);
    sm_config_set_out_pins(&c, cfg::PIN_DD0, 16);
    sm_config_set_sideset_pins(&c, cfg::PIN_IORDY);
    sm_config_set_jmp_pin(&c, cfg::PIN_DIOR_N);
    sm_config_set_out_shift(&c, true, false, 32);  // `out` takes the low bits first
    sm_config_set_in_shift(&c, true, true, 32);    // autopush: a read's `in pins, 32` is its sample
    sm_config_set_mov_status(&c, STATUS_RX_LESSTHAN, 4);  // STATUS: room in the 4-deep RX FIFO
    pio_sm_init(s_pio, s_sm, s_offset + s_idle_offset, &c);  // `idle`, not the first instruction

    // DIOR# skips the 2-cycle input synchroniser, so DD is released sooner
    // after DIOR# rises: about 10 ns instead of 24 (ATA allows 30, t6z), which
    // leaves margin for faster PIO modes. It is a clean strobe, only ever
    // compared with a fixed level, so metastability is harmless.
    s_pio->input_sync_bypass |= 1u << (cfg::PIN_DIOR_N - pio_get_gpio_base(s_pio));

    if (lane) pio_sm_exec(s_pio, s_sm, pio_encode_set(pio_y, LANE_DATA_ADDRESS));
    pio_sm_set_enabled(s_pio, s_sm, true);

    if (lane) {
        // ide_data_in, in the same block (only that block's outputs reach DD):
        // the two programs fill its 32 instructions.
        hard_assert(pio_can_add_program(s_pio, &ide_data_in_program));
        s_data_offset = pio_add_program(s_pio, &ide_data_in_program);
        s_data_start = pio_encode_jmp(s_data_offset);
        s_data_sm = uint(pio_claim_unused_sm(s_pio, true));
        pio_sm_config d = ide_data_in_program_get_default_config(s_data_offset);
        sm_config_set_out_pins(&d, cfg::PIN_DD0, 16);
        sm_config_set_out_shift(&d, true, false, 32);  // `out pins, 16` takes the low half
        // An 8-deep TX FIFO (it has no use for RX): the channel is done, and the
        // device can move on, 8 words before the block's end, not 4.
        sm_config_set_fifo_join(&d, PIO_FIFO_JOIN_TX);
        pio_sm_init(s_pio, s_data_sm, s_data_offset, &d);
        pio_sm_set_enabled(s_pio, s_data_sm, true);

        // Its channel: 16-bit words from the block into its TX FIFO, paced
        // by the FIFO's DREQ. Each block sets the source and count.
        s_lane_dma = uint(dma_claim_unused_channel(true));
        dma_channel_config dc = dma_channel_get_default_config(s_lane_dma);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_16);
        channel_config_set_read_increment(&dc, true);
        channel_config_set_write_increment(&dc, false);
        channel_config_set_dreq(&dc, pio_get_dreq(s_pio, s_data_sm, true));
        dma_channel_configure(s_lane_dma, &dc, &s_pio->txf[s_data_sm], nullptr, 0, false);
        return;  // no room for iordy_watchdog: `isa = on` keeps ide_bus
    }

    // The IORDY watchdog (iordy_watchdog.pio), in this block because it
    // writes IORDY's direction too. Loaded whatever `isa` says, so nothing
    // else lands in this block: the diagnostics' iordy_check would see DIOR#
    // through the synchroniser bypass above and misjudge the host.
    // ide_bus_early fills the block itself, so there's no watchdog beside it.
    if (early) return;
    hard_assert(pio_can_add_program(s_pio, &iordy_watchdog_program));
    uint watchdog_offset = pio_add_program(s_pio, &iordy_watchdog_program);
    if (isa) {
        uint sm = uint(pio_claim_unused_sm(s_pio, true));
        pio_sm_config w = iordy_watchdog_program_get_default_config(watchdog_offset);
        sm_config_set_jmp_pin(&w, cfg::PIN_IORDY);
        sm_config_set_set_pins(&w, cfg::PIN_IORDY, 1);
        pio_sm_init(s_pio, sm, watchdog_offset, &w);
        pio_sm_set_enabled(s_pio, sm, true);
    }
}

// The RX FIFO's empty flag, always inlined: the SDK's pio_sm_is_rx_fifo_empty()
// can end up out of line, in flash, once core 1 calls it from several places.
__attribute__((always_inline)) static inline bool rxEmpty(PIO pio, uint sm) {
    return (pio->fstat & (1u << (PIO_FSTAT_RXEMPTY_LSB + sm))) != 0;
}

// Console `D`'s delay, always inlined: the SDK's busy_wait_at_least_cycles()
// can end up out of line, in flash.
__attribute__((always_inline)) static inline void waitCycles(uint32_t cycles) {
    uint32_t start = m33_hw->dwt_cyccnt;
    while (m33_hw->dwt_cyccnt - start < cycles) {
    }
}

// Inlined into both callers: a call here costs every access its register
// saves and restores, about 100 ns (0.8.1's worst read).
__attribute__((always_inline)) static inline void serve(DriveDevice& dev, uint32_t sample) {
    if (s_early) sample = earlyAddress(sample);
    ata::Reg reg = s_reg_for[regIndex(sample)];
    if (s_early) {
        if ((sample & STROBE_BITS) == 0) {  // an answer request: the value, no side effects yet
            pio_sm_put(s_pio, s_sm, s_drive_for[regIndex(sample)] | dev.peek(reg));
            answered(reg == ata::Reg::Data);
            return;
        }
        if (sample & bus::maskOf(cfg::PIN_DIOW_N)) {  // a read happened: its side effects
            dev.consumeRead(reg);
            updateIntrq(dev);
            return;
        }
    }
    if (bus::op(sample) == bus::Op::Write) {
        if (s_lane_ahead) laneStop();  // a write before the block's end: the host has abandoned it
        // A command only sets BSY here; core 1 runs it next (Device::queueCommand).
        if (reg == ata::Reg::StatusCommand)
            dev.queueCommand(uint8_t(bus::data(sample)));
        else
            dev.write(reg, bus::data(sample));
    } else {
        // Every read must be answered, or the PIO holds IORDY low forever.
        // The host waits until it is, so answer first and catch up after.
        uint32_t delay = s_data_delay_cycles.load(std::memory_order_relaxed);
        if (delay && reg == ata::Reg::Data) waitCycles(delay);
        // The data lane: a block the host has finished shows in STATUS, and DRQ
        // for the next shows only once its words are on their way to the lane.
        laneSync(dev);
        uint32_t shown;
        if (laneShownStatus(reg, shown)) {
            pio_sm_put(s_pio, s_sm, s_drive_for[regIndex(sample)] | shown);  // the block's own DRQ
        } else {
            pio_sm_put(s_pio, s_sm, s_drive_for[regIndex(sample)] | dev.peek(reg));
            dev.consumeRead(reg);
        }
    }
    updateIntrq(dev);
}

// While a block is on the lane core 1 has nothing to do on the bus, so it
// waits here, taking the lane's steps as they fall due, so the device has
// moved on before the host's status read after the block (left to that read,
// the change held it past 1.25 us: 0.29.0-2d.4, 1,598 reads in a speed run).
// Meanwhile one small job per pass, as serveDataIn does: post the read-ahead
// to core 0, or take its result. True when an access is waiting or the block
// is done: the caller goes straight back to the bus. False when a command is
// queued or RESET# leaves `reset_low`: the caller carries on with its pass,
// which runs the command or handles the reset. (A command written mid-block,
// as the USB adapter does when it abandons a half-read block, CLAUDE.md, must
// not be left queued: returning true for it kept STATUS at BSY for good.)
CORE1_RAM bool serveLane(DriveDevice& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch) {
    bool posted = false;
    while (s_lane_words) {
        if (dev.commandQueued()) return false;  // the pass runs it
        if (!rxEmpty(s_pio, s_sm)) return true;  // an access: the main loop serves it
        if (gpio_get(cfg::PIN_RESET_N) == reset_low) return false;
        if (laneDue()) {
            laneSyncSlow(dev);  // the lane's next step
            updateIntrq(dev);
            posted = false;    // moving on asks core 0 for the next read-ahead
            continue;
        }
        if (!posted) {  // the read-ahead, once
            posted = true;
            ata::Fetch fetch;
            if (dev.pendingFetch(fetch) && fetch.id != posted_fetch && mailbox.post(fetch)) posted_fetch = fetch.id;
            continue;
        }
        FetchResult result;
        if (mailbox.takeResult(result)) dev.fetchDone(result.id, result.ok);  // the read-ahead is in
    }
    // The block is done (and no next one yet): straight back to the bus, where
    // the host's status read comes next; the pass's bookkeeping can wait (it
    // held that read past 1.25 us, 0.29.0-2d.4).
    return true;
}

CORE1_RAM bool serviceOnce(DriveDevice& dev, uint32_t& sample) {
    if (rxEmpty(s_pio, s_sm)) {
        fifoEmpty(SITE_MAIN);
        return false;
    }
    sample = pio_sm_get(s_pio, s_sm);
    serve(dev, sample);
    return true;
}

// The lane's rare case: an access mid-block that isn't a data read. Kept out
// of line so the lane's loop keeps its values in registers (0.8.12 inline:
// a third of data words held past 400 ns, against 2%).
__attribute__((noinline)) CORE1_RAM static void serveOther(DriveDevice& dev, uint32_t sample, uint16_t served) {
    if (bus::op(sample) == bus::Op::Read) {
        // A status poll (every sector boundary, with read-ahead): answer
        // first. Catching the device up can't change it: the block isn't
        // finished, so it's still DRQ.
        ata::Reg reg = s_reg_for[regIndex(sample)];
        pio_sm_put(s_pio, s_sm, s_drive_for[regIndex(sample)] | dev.peek(reg));
        dev.consumeDataWords(served);
        dev.consumeRead(reg);
        updateIntrq(dev);
        return;
    }
    dev.consumeDataWords(served);  // a write: catch the device up first
    serve(dev, sample);
}

CORE1_RAM bool serveDataIn(DriveDevice& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch) {
    const uint16_t* next;
    uint16_t left;
    if (!dev.dataInRemaining(next, left)) return true;
    uint16_t served = 0;
    bool reset_changed = false;
    // The read-ahead is posted only once the host is reading data words: at a
    // sector boundary it reads ALTSTATUS then STATUS straight after, and a
    // post started between them held the STATUS read (0.8.8: 1,240 ns).
    ata::Fetch fetch;
    bool to_post = false;
    while (served < left) {
        while (rxEmpty(s_pio, s_sm)) {
            if (gpio_get(cfg::PIN_RESET_N) == reset_low) {  // high when it was low, or low when high
                reset_changed = true;
                break;
            }
            if (to_post) {  // the read-ahead, so core 0 fetches while the host reads
                if (mailbox.post(fetch)) posted_fetch = fetch.id;
                to_post = false;
                continue;
            }
            FetchResult result;
            if (mailbox.takeResult(result)) dev.fetchDone(result.id, result.ok);  // the read-ahead is in
        }
        if (reset_changed) break;
        uint32_t sample = pio_sm_get(s_pio, s_sm);
        if ((sample & ACCESS_BITS) != DATA_READ) {
            serveOther(dev, sample, served);
            return true;
        }
        pio_sm_put(s_pio, s_sm, DRIVE_DD | next[served]);  // answer first, as serve() does
        if (served++ == 0) to_post = dev.pendingFetch(fetch) && fetch.id != posted_fetch;
    }
    dev.consumeDataWords(served);  // the block may be done: BSY for the next fetch
    updateIntrq(dev);
    return !reset_changed;
}

// The early loops' FIFO check: up to SPIN_POLLS reads of the RX FIFO's level
// in a row (a few cycles each), true as soon as a word is waiting. Called
// between housekeeping items, so a request waits a few cycles, not an item.
static constexpr unsigned SPIN_POLLS = 32;
__attribute__((always_inline)) static inline bool rxWaiting(PIO pio, uint sm) {
    if (!rxEmpty(pio, sm)) return true;
    fifoEmpty(SITE_POLL);  // the spin is the poll's, not the job's before it
    for (unsigned n = 1; n < SPIN_POLLS; ++n)
        if (!rxEmpty(pio, sm)) return true;
    return false;
}

// ide_bus_early's fast lane for a data-in block: as serveDataIn, but the
// program asks for each word at the chip select (a request) and reports each
// read afterwards, so a request for the data register gets the word after the
// ones read so far, and a data read's report moves on by one. Anything else
// catches the device up and is served as serviceOnce would.
static constexpr uint32_t DATA_ADDRESS = bus::maskOf(cfg::PIN_CS1_N);  // CS0# low, CS1# high, DA2-0 = 0

CORE1_RAM bool serveDataInEarly(DriveDevice& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch) {
    const uint16_t* next;
    uint16_t left;
    if (!dev.dataInRemaining(next, left)) return true;
    uint16_t served = 0;  // reads reported
    bool reset_changed = false;
    ata::Fetch fetch;
    bool to_post = false;
    while (served < left) {
        // A request must be answered within about 200 ns of its chip select (the
        // PIIX's data reads: t1 80 ns, strobes 175 ns), so while waiting only the
        // FIFO and RESET# are watched; the fetch handshake waits until just after
        // a data read, when the next request is a recovery time away.
        while (rxEmpty(s_pio, s_sm)) {
            fifoEmpty(SITE_POLL);
            if (gpio_get(cfg::PIN_RESET_N) == reset_low) {
                reset_changed = true;
                break;
            }
            stallCheck();
        }
        if (reset_changed) break;
        uint32_t sample = earlyAddress(pio_sm_get(s_pio, s_sm));  // a report's address from its request
        if ((sample & ADDRESS_BITS) == DATA_ADDRESS) {
            if ((sample & STROBE_BITS) == 0) {  // the next word, asked for ahead
                pio_sm_put(s_pio, s_sm, DRIVE_DD | next[served]);
                answered();
                continue;
            }
            if (sample & bus::maskOf(cfg::PIN_DIOW_N)) {  // a data read happened
                site(SITE_LANE_DATA);
                if (served++ == 0) to_post = dev.pendingFetch(fetch) && fetch.id != posted_fetch;
                if (to_post) {  // the read-ahead, so core 0 fetches while the host reads
                    if (mailbox.post(fetch)) posted_fetch = fetch.id;
                    to_post = false;
                } else {
                    FetchResult result;
                    if (mailbox.takeResult(result)) dev.fetchDone(result.id, result.ok);  // the read-ahead is in
                }
                continue;
            }
        }
        // Anything else (a status poll, say): catch the device up, serve it here
        // and carry on, rather than leave the lane for the main loop's overhead.
        site(SITE_LANE_OTHER);
        dev.consumeDataWords(served);
        served = 0;
        serve(dev, sample);
        if (dev.commandQueued() || !dev.dataInRemaining(next, left)) {
            site(SITE_MAIN);
            return true;
        }
    }
    dev.consumeDataWords(served);  // the block may be done: BSY for the next fetch
    updateIntrq(dev);
    site(SITE_MAIN);
    return !reset_changed;
}

// early_answers: core 1's tight bus loop. The main loop takes about 150
// cycles a pass, so a request could wait 600-900 ns for it (c1_gap_ns, 0.23.3),
// far past the 400 ns a host that doesn't wait leaves. This serves each access
// inline, a few cycles a poll, and keeps the fetch handshake going (so BSY
// ends while the host polls), until the bus has been quiet for `quiet_cycles`,
// a command is queued, a data-in block starts (the fast lane's job), or
// RESET# changes: the main loop's work. Not while the raw trace is on.
CORE1_RAM void serveBusEarly(DriveDevice& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch,
                             uint32_t quiet_cycles, void (*timers)(DriveDevice&), void (*command)(DriveDevice&),
                             void (*bus)(uint32_t)) {
    PIO pio = s_pio;
    uint sm = s_sm;
    uint32_t last = m33_hw->dwt_cyccnt;
    captureCore1Irqs();
    for (;;) {
        if (rxWaiting(pio, sm)) {
            uint32_t sample = pio_sm_get(pio, sm);
            site(SITE_SERVE);
            serve(dev, sample);
            if (bus && (sample & STROBE_BITS) != 0) {
                site(SITE_TRACE);
                bus(sample);  // the trace, after the answer
            }
            if (dev.commandQueued()) {
                site(SITE_COMMAND);
                command(dev);  // run it here, not in the main loop
            }
            const uint16_t* next;
            uint16_t left;
            // With the raw trace on, data reads stay here, one access at a time, so each is recorded
            if (!bus && dev.dataInRemaining(next, left) && !serveDataInEarly(dev, reset_low, mailbox, posted_fetch)) return;
            if (dev.commandQueued()) {
                site(SITE_COMMAND);
                command(dev);
            }
            last = m33_hw->dwt_cyccnt;
            continue;
        }
        site(SITE_STALL);
        stallCheck();
        if (m33_hw->dwt_cyccnt - last > quiet_cycles) {
            site(SITE_MAIN);
            return;
        }
        if (rxWaiting(pio, sm)) continue;
        if (gpio_get(cfg::PIN_RESET_N) == reset_low) {  // high when it was low, or low when high
            site(SITE_MAIN);
            return;
        }
        if (rxWaiting(pio, sm)) continue;
        site(SITE_FETCH_POST);
        uint32_t id = dev.pendingFetchId();
        if (id != 0 && id != posted_fetch) {
            ata::Fetch fetch;
            if (dev.pendingFetch(fetch) && mailbox.post(fetch)) posted_fetch = fetch.id;
        }
        if (rxWaiting(pio, sm)) continue;
        site(SITE_FETCH_DONE);
        FetchResult result;
        if (mailbox.takeResult(result)) {
            dev.fetchDone(result.id, result.ok);
            updateIntrq(dev);
            const uint16_t* next;
            uint16_t left;
            if (!bus && dev.dataInRemaining(next, left) && !serveDataInEarly(dev, reset_low, mailbox, posted_fetch)) return;
            last = m33_hw->dwt_cyccnt;
        }
        if (rxWaiting(pio, sm)) continue;
        site(SITE_TIMERS);
        timers(dev);
    }
}

CORE1_RAM bool dataReadDelayed() { return s_data_delay_cycles.load(std::memory_order_relaxed) != 0; }

void restart() {
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 0, DRIVEN_PINS);  // release DD and IORDY
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_offset + s_idle_offset));
    if (s_lane) {
        pio_sm_exec(s_pio, s_sm, pio_encode_set(pio_y, LANE_DATA_ADDRESS));
        laneStop();
    }
    pio_sm_set_enabled(s_pio, s_sm, true);
}

void stop() {
    if (s_lane) {
        laneStop();
        pio_sm_set_enabled(s_pio, s_data_sm, false);  // restart() starts it again
    }
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 0, DRIVEN_PINS);  // release DD and IORDY
    gpio_set_dir(cfg::PIN_INTRQ, false);                        // and INTRQ
}

void setDataReadDelay(uint32_t ns) {
    s_data_delay_cycles.store(uint32_t(uint64_t(ns) * clock_get_hz(clk_sys) / 1000000000ull),
                              std::memory_order_relaxed);
}

static volatile uint32_t s_intrq_rises = 0;  // interrupts raised (driven high from low), for `s`
static bool s_intrq_high = false;

CORE1_RAM void updateIntrq(DriveDevice& dev) {
    // The lane's device moved on ahead of the host: its next interrupt waits
    // until the host has read the block.
    const bool asserted = dev.intrq() && !s_lane_ahead;
    bool high = asserted && dev.intrqDriven();
    if (high && !s_intrq_high) s_intrq_rises = s_intrq_rises + 1;
    s_intrq_high = high;
    gpio_put(cfg::PIN_INTRQ, asserted);
    gpio_set_dir(cfg::PIN_INTRQ, dev.intrqDriven());
}

uint32_t intrqRises() { return s_intrq_rises; }

}  // namespace ide_bus
