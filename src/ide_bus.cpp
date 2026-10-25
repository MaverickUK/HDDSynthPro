#include "ide_bus.h"

#include <atomic>

#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>

#include "ata_device.h"
#include "bus_sample.h"
#include "config.h"
#include "core1_ram.h"
#include "fetch_mailbox.h"
#include "ide_bus.pio.h"

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
static std::atomic<uint32_t> s_data_delay_cycles{0};  // setDataReadDelay

static unsigned regIndex(uint32_t sample) { return (sample >> REG_INDEX_SHIFT) & 31u; }

// A read of the data register, as the PIO samples it: DIOR# low, DIOW# high,
// CS0# low, CS1# high, DA2-0 = 0.
static constexpr uint32_t ACCESS_BITS = (31u << REG_INDEX_SHIFT) | bus::maskOf(cfg::PIN_DIOR_N) |
                                        bus::maskOf(cfg::PIN_DIOW_N);
static constexpr uint32_t DATA_READ = bus::maskOf(cfg::PIN_CS1_N) | bus::maskOf(cfg::PIN_DIOW_N);

void begin() {
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t sample = i << REG_INDEX_SHIFT;
        bus::Block block = bus::block(sample);
        s_reg_for[i] = ata::regFor(block == bus::Block::Command, block == bus::Block::Control,
                                   bus::address(sample));
    }
    for (unsigned pin = cfg::BUS_FIRST_PIN; pin <= cfg::BUS_LAST_PIN; ++pin) {
        if (pin == cfg::PIN_BOARD_LED) continue;
        gpio_init(pin);           // input until claimed below
        gpio_disable_pulls(pin);  // never internal pull-downs (RP2350 erratum E9)
    }
    // INTRQ: SIO, output value high but released until asserted.
    gpio_put(cfg::PIN_INTRQ, 1);
    gpio_set_dir(cfg::PIN_INTRQ, false);

    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &ide_bus_program, &s_pio, &s_sm, &s_offset, cfg::BUS_FIRST_PIN,
        cfg::BUS_LAST_PIN - cfg::BUS_FIRST_PIN + 1, true);
    hard_assert(ok);

    // DD0-15 and IORDY are driven by PIO. Both start as inputs (released);
    // IORDY's output value is 0, so making it an output pulls it low.
    for (unsigned i = 0; i < 16; ++i) pio_gpio_init(s_pio, cfg::PIN_DD0 + i);
    pio_gpio_init(s_pio, cfg::PIN_IORDY);
    pio_sm_set_pins_with_mask(s_pio, s_sm, 0, 1u << cfg::PIN_IORDY);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 0, DRIVEN_PINS);

    pio_sm_config c = ide_bus_program_get_default_config(s_offset);
    sm_config_set_in_pins(&c, cfg::PIN_DD0);
    sm_config_set_out_pins(&c, cfg::PIN_DD0, 16);
    sm_config_set_sideset_pins(&c, cfg::PIN_IORDY);
    sm_config_set_jmp_pin(&c, cfg::PIN_DIOR_N);
    sm_config_set_out_shift(&c, true, false, 32);  // `out` takes the low bits first
    sm_config_set_mov_status(&c, STATUS_RX_LESSTHAN, 4);  // STATUS: room in the 4-deep RX FIFO
    pio_sm_init(s_pio, s_sm, s_offset, &c);

    // DIOR# skips the 2-cycle input synchroniser, so DD is released sooner
    // after DIOR# rises: about 10 ns instead of 24 (ATA allows 30, t6z), which
    // leaves margin for faster PIO modes. It is a clean strobe, only ever
    // compared with a fixed level, so metastability is harmless.
    s_pio->input_sync_bypass |= 1u << (cfg::PIN_DIOR_N - pio_get_gpio_base(s_pio));

    pio_sm_set_enabled(s_pio, s_sm, true);
}

// Inlined into both callers: a call here costs every access its register
// saves and restores, about 100 ns (0.8.1's worst read).
__attribute__((always_inline)) static inline void serve(ata::Device& dev, uint32_t sample) {
    ata::Reg reg = s_reg_for[regIndex(sample)];
    if (bus::op(sample) == bus::Op::Write) {
        // A command only sets BSY here; core 1 runs it next (Device::queueCommand).
        if (reg == ata::Reg::StatusCommand)
            dev.queueCommand(uint8_t(bus::data(sample)));
        else
            dev.write(reg, bus::data(sample));
    } else {
        // Every read must be answered, or the PIO holds IORDY low forever.
        // The host waits until it is, so answer first and catch up after.
        uint32_t delay = s_data_delay_cycles.load(std::memory_order_relaxed);
        if (delay && reg == ata::Reg::Data) busy_wait_at_least_cycles(delay);
        pio_sm_put(s_pio, s_sm, dev.peek(reg));
        dev.consumeRead(reg);
    }
    updateIntrq(dev.intrq());
}

CORE1_RAM bool serviceOnce(ata::Device& dev, uint32_t& sample) {
    if (pio_sm_is_rx_fifo_empty(s_pio, s_sm)) return false;
    sample = pio_sm_get(s_pio, s_sm);
    serve(dev, sample);
    return true;
}

// The lane's rare case: an access mid-block that isn't a data read. Kept out
// of line so the lane's loop keeps its values in registers (0.8.12 inline:
// a third of data words held past 400 ns, against 2%).
__attribute__((noinline)) CORE1_RAM static void serveOther(ata::Device& dev, uint32_t sample, uint16_t served) {
    if (bus::op(sample) == bus::Op::Read) {
        // A status poll (every sector boundary, with read-ahead): answer
        // first. Catching the device up can't change it: the block isn't
        // finished, so it's still DRQ.
        ata::Reg reg = s_reg_for[regIndex(sample)];
        pio_sm_put(s_pio, s_sm, dev.peek(reg));
        dev.consumeDataWords(served);
        dev.consumeRead(reg);
        updateIntrq(dev.intrq());
        return;
    }
    dev.consumeDataWords(served);  // a write: catch the device up first
    serve(dev, sample);
}

CORE1_RAM bool serveDataIn(ata::Device& dev, bool reset_low, FetchMailbox& mailbox, uint32_t& posted_fetch) {
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
        while (pio_sm_is_rx_fifo_empty(s_pio, s_sm)) {
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
        pio_sm_put(s_pio, s_sm, next[served]);  // answer first, as serve() does
        if (served++ == 0) to_post = dev.pendingFetch(fetch) && fetch.id != posted_fetch;
    }
    dev.consumeDataWords(served);  // the block may be done: BSY for the next fetch
    updateIntrq(dev.intrq());
    return !reset_changed;
}

CORE1_RAM bool dataReadDelayed() { return s_data_delay_cycles.load(std::memory_order_relaxed) != 0; }

void restart() {
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 0, DRIVEN_PINS);  // release DD and IORDY
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_offset));
    pio_sm_set_enabled(s_pio, s_sm, true);
}

void setDataReadDelay(uint32_t ns) {
    s_data_delay_cycles.store(uint32_t(uint64_t(ns) * clock_get_hz(clk_sys) / 1000000000ull),
                              std::memory_order_relaxed);
}

CORE1_RAM void updateIntrq(bool asserted) { gpio_set_dir(cfg::PIN_INTRQ, asserted); }

}  // namespace ide_bus
