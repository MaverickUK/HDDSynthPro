// The register table (docs/register-table.md): setup, and core 1's calls.
#include "ide_table.h"

#include <initializer_list>

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>

#include "bus_observer.h"
#include "bus_sample.h"
#include "config.h"
#include "core1_ram.h"
#include "ide_table.pio.h"
#include "ide_table_write.pio.h"

namespace ide_table {
namespace {

// The address bits as the programs read them (DA2-0, CS0#, CS1#), for Y.
constexpr unsigned DATA_ADDRESS = 16;     // DA 0, CS0# low, CS1# high
constexpr unsigned COMMAND_ADDRESS = 23;  // DA 7, CS0# low
// R0-R2's claims: DA2, CS0#, CS1#, as the program's `out x, 3` reads them.
constexpr unsigned CLAIM_R0 = 4, CLAIM_R1 = 5, CLAIM_R2 = 3;
constexpr uint32_t DRIVE = 0xFFFF0000;  // every DD line
constexpr uint32_t BUSY = DRIVE | 0x80;
constexpr uint32_t DRIVEN_PINS = (0xFFFFu << cfg::PIN_DD0) | (1u << cfg::PIN_IORDY);
constexpr uint32_t PULL_NOBLOCK = 0x8080;  // pio_encode_pull(false, false), for core 1
constexpr uint32_t SET_PINS_0 = 0xe000, SET_PINS_1 = 0xe001;        // pio_encode_set(pio_pins, v)
constexpr uint32_t SET_PINDIRS_0 = 0xe080, SET_PINDIRS_1 = 0xe081;  // pio_encode_set(pio_pindirs, v)

PIO s_pio = nullptr;      // block 0: the read side
uint s_r0, s_r1, s_r2, s_d;
PIO s_wpio = nullptr;     // block 1: the write side
uint s_w, s_wd, s_c;
bool s_running = false;
uint32_t s_d_start = 0;  // `jmp` to D's start, for dReset

// DMA channels (the design note's table)
uint s_stream;            // a data-in block (and a spare word) into D's TX FIFO; chains Ka
uint s_c1;                // C's word: starts the busy writes
uint s_ka, s_kb;          // the busy word into R1's Status entry, then R2's Alternate Status
uint s_wdma;              // WD's words into the sector buffer
uint s_wring;             // W's samples into the ring

// The busy word, for Ka and Kb; a dummy for C1's word.
uint32_t s_busy = BUSY;
uint32_t s_dummy = 0;

// W's ring: 256 samples, aligned for the channel's address wrap.
constexpr unsigned WRING_BITS = 10;  // bytes, as a power of two
constexpr uint32_t WRING_WORDS = (1u << WRING_BITS) / 4;
alignas(1u << WRING_BITS) uint32_t s_wring_buf[WRING_WORDS];
uint32_t s_wring_read = 0;

// bus_observer's ring, read from here for Status reads (its own position).
const uint32_t* s_obs_ring = nullptr;
uint32_t s_obs_words = 0;
uint s_obs_dma = 0;
uint32_t s_obs_read = 0;

// What core 1 has armed, and whether the busy write it makes is accounted for.
bool s_c1_armed = false;
bool s_in_active = false, s_out_active = false;
volatile uint32_t s_blocks_in = 0, s_blocks_out = 0;
// Bench: the first blocks: at the start, D's FIFO level after the wait; at
// the stop, words fetched and the FIFO level.
const uint16_t* s_in_words = nullptr;
volatile uint32_t s_rec[16][5];
volatile uint32_t s_rec_n = 0;
volatile bool s_seen_on = false;
volatile bool s_cap_arm = false;
volatile bool s_cap_cmd = false;

uint claimDma() { return uint(dma_claim_unused_channel(true)); }

// A channel moving one word per trigger, reloaded each time it's triggered.
void oneShot(uint ch, enum dma_channel_transfer_size size, bool read_inc, uint dreq, uint chain_to,
             volatile void* write, const volatile void* read) {
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, size);
    channel_config_set_read_increment(&c, read_inc);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, dreq);
    channel_config_set_chain_to(&c, chain_to);  // itself: no chain
    dma_channel_configure(ch, &c, write, read, 1, false);
}


}  // namespace

void begin() {
    // The bus pins as ide_bus leaves them: inputs with no pulls (never pull-
    // downs: RP2350 erratum E9), INTRQ released for updateIntrq.
    for (unsigned pin = cfg::BUS_FIRST_PIN; pin <= cfg::BUS_LAST_PIN; ++pin) {
        if (pin == cfg::PIN_BOARD_LED) continue;
        gpio_init(pin);
        gpio_disable_pulls(pin);
    }
    gpio_put(cfg::PIN_INTRQ, 1);
    gpio_set_dir(cfg::PIN_INTRQ, false);

    // ---- Block 0: R0-R2 (ide_table_reg) and D (ide_table_data) ----
    uint reg_offset;
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(&ide_table_reg_program, &s_pio, &s_r0, &reg_offset,
                                                               cfg::BUS_FIRST_PIN,
                                                               cfg::BUS_LAST_PIN - cfg::BUS_FIRST_PIN + 1, true);
    hard_assert(ok);
    hard_assert(pio_get_gpio_base(s_pio) == 0);  // `wait gpio 26` is DIOR#
    hard_assert(pio_can_add_program(s_pio, &ide_table_data_program));
    uint data_offset = pio_add_program(s_pio, &ide_table_data_program);
    s_r1 = uint(pio_claim_unused_sm(s_pio, true));
    s_r2 = uint(pio_claim_unused_sm(s_pio, true));
    s_d = uint(pio_claim_unused_sm(s_pio, true));

    for (unsigned i = 0; i < 16; ++i) pio_gpio_init(s_pio, cfg::PIN_DD0 + i);
    pio_gpio_init(s_pio, cfg::PIN_IORDY);
    pio_sm_set_pins_with_mask(s_pio, s_d, 0, 1u << cfg::PIN_IORDY);  // IORDY's value 0: an output pulls it low
    pio_sm_set_pindirs_with_mask(s_pio, s_d, 0, DRIVEN_PINS);       // everything released
    // DIOR# skips the input synchroniser, so D pulls IORDY low inside tA (35 ns);
    // everything else goes through it, and R0-R2 wait two cycles for the address
    // to catch up (ide_table.pio).
    s_pio->input_sync_bypass &= ~(0xFFFFFFFFu >> (32 - (cfg::BUS_LAST_PIN + 1)));
    s_pio->input_sync_bypass |= 1u << (cfg::PIN_DIOR_N - pio_get_gpio_base(s_pio));

    const uint claims[3] = {CLAIM_R0, CLAIM_R1, CLAIM_R2};
    const uint regs[3] = {s_r0, s_r1, s_r2};
    for (unsigned k = 0; k < 3; ++k) {
        pio_sm_config c = ide_table_reg_program_get_default_config(reg_offset);  // .fifo txget
        sm_config_set_in_pins(&c, cfg::PIN_DD0);
        sm_config_set_out_pins(&c, cfg::PIN_DD0, 16);
        sm_config_set_out_shift(&c, true, false, 32);
        pio_sm_init(s_pio, regs[k], reg_offset + ide_table_reg_offset_rise, &c);
        pio_sm_exec(s_pio, regs[k], pio_encode_set(pio_x, claims[k]));  // its claim, in ISR
        pio_sm_exec(s_pio, regs[k], pio_encode_mov(pio_isr, pio_x));
    }
    // The register image before anything reads it: nothing driven, BSY. After
    // pio_sm_init, which turns the FIFO into registers: written before, the
    // entries kept whatever they held, and R0's entry 0 answered every data
    // read over D (12 Nov 2026).
    for (uint sm : {s_r0, s_r1, s_r2})
        for (unsigned i = 0; i < 4; ++i) s_pio->rxf_putget[sm][i] = 0;
    s_pio->rxf_putget[s_r1][3] = BUSY;
    s_pio->rxf_putget[s_r2][2] = BUSY;
    {
        pio_sm_config c = ide_table_data_program_get_default_config(data_offset);
        sm_config_set_in_pins(&c, cfg::PIN_DD0);
        sm_config_set_out_pins(&c, cfg::PIN_DD0, 16);
        sm_config_set_sideset_pins(&c, cfg::PIN_IORDY);
        sm_config_set_out_shift(&c, true, false, 32);  // `out pins, 16` takes the low half
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);  // 8 deep, as the data lane's ide_data_in
        pio_sm_init(s_pio, s_d, data_offset, &c);
        pio_sm_exec(s_pio, s_d, pio_encode_set(pio_y, DATA_ADDRESS));
        s_d_start = pio_encode_jmp(data_offset);
    }

    // ---- Block 1: W, WD and C (ide_table_write.pio); bus_observer joins them ----
    uint w_offset;
    ok = pio_claim_free_sm_and_add_program_for_gpio_range(&ide_table_write_program, &s_wpio, &s_w, &w_offset,
                                                          cfg::BUS_FIRST_PIN,
                                                          cfg::BUS_LAST_PIN - cfg::BUS_FIRST_PIN + 1, true);
    hard_assert(ok && s_wpio != s_pio && pio_get_gpio_base(s_wpio) == 0);
    hard_assert(pio_can_add_program(s_wpio, &ide_table_data_out_program));
    uint wd_offset = pio_add_program(s_wpio, &ide_table_data_out_program);
    hard_assert(pio_can_add_program(s_wpio, &ide_table_command_program));
    uint c_offset = pio_add_program(s_wpio, &ide_table_command_program);
    s_wd = uint(pio_claim_unused_sm(s_wpio, true));
    s_c = uint(pio_claim_unused_sm(s_wpio, true));
    s_wpio->input_sync_bypass &= ~(0xFFFFFFFFu >> (32 - (cfg::BUS_LAST_PIN + 1)));
    {
        pio_sm_config c = ide_table_write_program_get_default_config(w_offset);
        sm_config_set_in_pins(&c, cfg::PIN_DD0);
        sm_config_set_in_shift(&c, true, true, 32);  // autopush: one sample per write
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
        pio_sm_init(s_wpio, s_w, w_offset, &c);
    }
    {
        pio_sm_config c = ide_table_data_out_program_get_default_config(wd_offset);
        sm_config_set_in_pins(&c, cfg::PIN_DD0);
        sm_config_set_out_shift(&c, true, false, 32);
        sm_config_set_in_shift(&c, false, true, 16);  // shift left, autopush at 16: the word in the low half
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
        pio_sm_init(s_wpio, s_wd, wd_offset + ide_table_data_out_offset_top, &c);
        pio_sm_exec(s_wpio, s_wd, pio_encode_set(pio_y, DATA_ADDRESS));
    }
    {
        pio_sm_config c = ide_table_command_program_get_default_config(c_offset);
        sm_config_set_in_pins(&c, cfg::PIN_DD0);
        sm_config_set_sideset_pins(&c, cfg::PIN_INTRQ);  // low as a Command write starts
        sm_config_set_set_pins(&c, cfg::PIN_INTRQ, 1);   // setIntrq's forced SETs
        sm_config_set_out_shift(&c, true, false, 32);
        pio_sm_init(s_wpio, s_c, c_offset + ide_table_command_offset_top, &c);
        pio_sm_exec(s_wpio, s_c, pio_encode_set(pio_y, COMMAND_ADDRESS));
        pio_gpio_init(s_wpio, cfg::PIN_INTRQ);  // C owns INTRQ now
        pio_sm_exec(s_wpio, s_c, pio_encode_set(pio_pins, 0));     // low,
        pio_sm_exec(s_wpio, s_c, pio_encode_set(pio_pindirs, 0));  // and released
    }

    // ---- DMA ----
    s_stream = claimDma();
    s_c1 = claimDma();
    s_ka = claimDma();
    s_kb = claimDma();
    s_wdma = claimDma();
    s_wring = claimDma();
    {  // The data-in stream: 16-bit words into D's TX FIFO (both halves), paced by its
       // DREQ, as the data lane's; each block sets the source and count, and its
       // completion starts the busy write.
        dma_channel_config c = dma_channel_get_default_config(s_stream);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_d, true));
        channel_config_set_chain_to(&c, s_ka);
        dma_channel_configure(s_stream, &c, &s_pio->txf[s_d], nullptr, 0, false);
    }
    // The busy writes: Ka into R1's Status entry, Kb into R2's Alternate Status.
    oneShot(s_ka, DMA_SIZE_32, false, DREQ_FORCE, s_kb, &s_pio->rxf_putget[s_r1][3], &s_busy);
    oneShot(s_kb, DMA_SIZE_32, false, DREQ_FORCE, s_kb, &s_pio->rxf_putget[s_r2][2], &s_busy);
    // C1: C's word at a Command write.
    oneShot(s_c1, DMA_SIZE_32, false, pio_get_dreq(s_wpio, s_c, false), s_kb, &s_pio->rxf_putget[s_r1][3],
            &s_wpio->rxf[s_c]);  // C's busy word straight into R1's Status entry: one hop, then Kb
    {  // WD: a block's words into the sector buffer, then the busy writes
        dma_channel_config c = dma_channel_get_default_config(s_wdma);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(s_wpio, s_wd, false));
        channel_config_set_chain_to(&c, s_ka);
        dma_channel_configure(s_wdma, &c, nullptr, &s_wpio->rxf[s_wd], 0, false);
    }
    {  // W: every write sample into the ring, for ever
        dma_channel_config c = dma_channel_get_default_config(s_wring);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_ring(&c, true, WRING_BITS);
        channel_config_set_dreq(&c, pio_get_dreq(s_wpio, s_w, false));
        dma_channel_configure(s_wring, &c, s_wring_buf, &s_wpio->rxf[s_w], dma_encode_endless_transfer_count(),
                              true);
    }
    s_wring_read = 0;

    // Go: the write side first, so no write is missed once reads are answered.
    pio_sm_set_enabled(s_wpio, s_w, true);
    pio_sm_set_enabled(s_wpio, s_wd, true);
    pio_sm_set_enabled(s_wpio, s_c, true);
    armCommandBusy();
    pio_sm_set_enabled(s_pio, s_d, true);
    for (uint sm : {s_r0, s_r1, s_r2}) pio_sm_set_enabled(s_pio, sm, true);
    s_running = true;
}

bool running() { return s_running; }

void attachObserver() {
    if (!bus_observer::running()) return;
    s_obs_words = bus_observer::ringWords();
    s_obs_dma = bus_observer::ringChannel();
    s_obs_read = (dma_hw->ch[s_obs_dma].write_addr - uint32_t(uintptr_t(bus_observer::ring()))) / 4;
    s_obs_ring = bus_observer::ring();
}

// ---- Core 1 ----------------------------------------------------------------------

namespace {

__attribute__((always_inline)) inline bool dmaBusy(uint ch) {
    return dma_hw->ch[ch].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS;
}

// dma_channel_abort(), inline: stops once in-flight transfers retire.
__attribute__((always_inline)) inline void dmaAbort(uint ch) {
    dma_hw->abort = 1u << ch;
    while (dma_hw->abort & (1u << ch)) {
    }
}

// Triggers a one-word channel again (its count reloads).
__attribute__((always_inline)) inline void dmaRetrigger(uint ch) { dma_hw->multi_channel_trigger = 1u << ch; }

// D back at its start with an empty FIFO: before each data-in block, as the
// data lane resets ide_data_in (written out: no SDK calls on core 1).
__attribute__((always_inline)) inline void dReset() {
    hw_clear_bits(&s_pio->ctrl, 1u << s_d);
    // Empty the FIFO by pulling its words, as D itself would. (Toggling FJOIN_RX,
    // as pio_sm_clear_fifos does, leaves a TX-joined FIFO as it was: the old
    // block's last words came first in the next. Toggling FJOIN_TX empties it,
    // but behind the DMA's back: the stream's channel then put in eight words
    // and no more. 12 Nov 2026.)
    for (unsigned n = 0; n < 8 && ((s_pio->flevel >> (s_d * 8)) & 0xF); ++n) s_pio->sm[s_d].instr = PULL_NOBLOCK;
    hw_set_bits(&s_pio->ctrl, 1u << (PIO_CTRL_SM_RESTART_LSB + s_d));
    s_pio->sm[s_d].instr = s_d_start;  // jmp to its start
    hw_set_bits(&s_pio->ctrl, 1u << s_d);
}

// Whether a busy write the DMA made isn't accounted for yet: C1 fired for a
// command core 1 hasn't taken, or a block's busy write fired before core 1
// finished the block. Then Status must stay BSY whatever the device says.
__attribute__((always_inline)) inline bool busyOwed() {
    return (s_c1_armed && !dmaBusy(s_c1)) || (s_in_active && !dmaBusy(s_stream)) || (s_out_active && !dmaBusy(s_wdma));
}

}  // namespace

CORE1_RAM_TABLE void publish(const Image& image) {
    PIO pio = s_pio;
    pio->rxf_putget[s_r0][1] = image.error;
    pio->rxf_putget[s_r0][2] = image.sector_count;
    pio->rxf_putget[s_r0][3] = image.lba_low;
    pio->rxf_putget[s_r1][0] = image.lba_mid;
    pio->rxf_putget[s_r1][1] = image.lba_high;
    pio->rxf_putget[s_r1][2] = image.device;
    // Status last, and checked again after: a busy write that lands between the
    // check and the write would otherwise be overwritten.
    uint32_t status = busyOwed() ? BUSY : image.status;
    pio->rxf_putget[s_r1][3] = status;
    pio->rxf_putget[s_r2][2] = status;
    if (status != BUSY && busyOwed()) {
        pio->rxf_putget[s_r1][3] = BUSY;
        pio->rxf_putget[s_r2][2] = BUSY;
    }
}

CORE1_RAM_TABLE bool popWrite(uint32_t& sample) {
    uint32_t written = (dma_hw->ch[s_wring].write_addr - uint32_t(uintptr_t(s_wring_buf))) / 4;
    if (written == s_wring_read) return false;
    sample = s_wring_buf[s_wring_read];
    s_wring_read = (s_wring_read + 1) % WRING_WORDS;
    return true;
}

CORE1_RAM_TABLE bool statusReadSeen() {
    if (!s_obs_ring) return false;  // the observer isn't running
    uint32_t written = (dma_hw->ch[s_obs_dma].write_addr - uint32_t(uintptr_t(s_obs_ring))) / 4;
    bool seen = false;
    while (s_obs_read != written) {
        uint32_t word = s_obs_ring[s_obs_read];
        s_obs_read = (s_obs_read + 1) % s_obs_words;
        if (((word >> 16) & 0x1F) == COMMAND_ADDRESS) seen = true;  // Status: DA 7 under CS0#
    }
    return seen;
}

volatile uint32_t s_cmds_taken = 0, s_cmds_c1_fired = 0;
CORE1_RAM_TABLE void armCommandBusy() {
    s_cmds_taken = s_cmds_taken + 1;
    if (s_c1_armed && !dmaBusy(s_c1)) s_cmds_c1_fired = s_cmds_c1_fired + 1;  // bench: C's busy fired for it
    dmaRetrigger(s_c1);  // waits for C's next word
    s_c1_armed = true;
}

// INTRQ: C lowers it itself as a Command write starts; core 1 sets it here when
// the device's interrupt changes (forced instructions on C, which waits
// meanwhile, with SET pins = INTRQ).
CORE1_RAM_TABLE void setIntrq(bool asserted, bool driven) {
    s_wpio->sm[s_c].instr = asserted ? SET_PINS_1 : SET_PINS_0;
    s_wpio->sm[s_c].instr = driven ? SET_PINDIRS_1 : SET_PINDIRS_0;
}

CORE1_RAM_TABLE bool commandBusyPending() { return s_c1_armed && !dmaBusy(s_c1); }

CORE1_RAM_TABLE void startDataIn(const uint16_t* words, uint16_t count) {
    if (dmaBusy(s_stream)) dmaAbort(s_stream);  // only if running: an idle channel aborted lost its pacing
    dReset();
    dma_hw->ch[s_stream].read_addr = uint32_t(uintptr_t(words));
    dma_hw->ch[s_stream].al1_transfer_count_trig = uint32_t(count) + 1;  // and the spare word; go
    // The first words in D's FIFO before the caller publishes DRQ, so the host
    // can't read word 0 before it's there (D's pull doesn't wait).
    const uint32_t want = count + 1 < 8 ? count + 1 : 8;
    for (uint32_t spins = 0; ((s_pio->flevel >> (s_d * 8)) & 0xF) < want && spins < 2000; ++spins) {
    }
    s_in_active = true;
    s_in_words = words;
    // (bench: set s_cap_arm here to capture a block's timeline)
    {
        uint32_t i = s_rec_n % 16;
        s_rec[i][0] = (s_pio->flevel >> (s_d * 8)) & 0xF;
        s_rec[i][3] = words[0] | uint32_t(count) << 16;
    }
}

CORE1_RAM_TABLE bool dataInActive() { return s_in_active; }

// The host has read the whole block: every word in the FIFO, and only the
// spare word left in it (D never pulls that one).
CORE1_RAM_TABLE bool dataInDone() {
    return s_in_active && !dmaBusy(s_stream) && ((s_pio->flevel >> (s_d * 8)) & 0xF) <= 1;
}

uint8_t s_stop_why = 0;
CORE1_RAM_TABLE void stopWhy(uint8_t why) { s_stop_why = why; }
CORE1_RAM_TABLE void stopDataIn() {
    if (s_in_active) {
        uint32_t i = s_rec_n % 16;
        s_rec[i][4] = s_stop_why;
        s_rec[i][1] = (dma_hw->ch[s_stream].read_addr - uint32_t(uintptr_t(s_in_words))) / 2;
        s_rec[i][2] = (s_pio->flevel >> (s_d * 8)) & 0xF;
        s_rec_n = s_rec_n + 1;
    }
    if (s_in_active && !dmaBusy(s_stream)) s_blocks_in = s_blocks_in + 1;
    if (dmaBusy(s_stream)) dmaAbort(s_stream);  // never an idle channel (see startDataIn)
    s_in_active = false;
}

CORE1_RAM_TABLE void startDataOut(uint16_t* dest, uint16_t count) {
    if (dmaBusy(s_wdma)) dmaAbort(s_wdma);
    dma_hw->ch[s_wdma].write_addr = uint32_t(uintptr_t(dest));
    dma_hw->ch[s_wdma].al1_transfer_count_trig = count;  // and go
    s_out_active = true;
}

CORE1_RAM_TABLE bool dataOutActive() { return s_out_active; }
CORE1_RAM_TABLE bool dataOutDone() { return s_out_active && !dmaBusy(s_wdma); }

CORE1_RAM_TABLE void stopDataOut() {
    if (s_out_active && !dmaBusy(s_wdma)) s_blocks_out = s_blocks_out + 1;
    if (dmaBusy(s_wdma)) dmaAbort(s_wdma);
    s_out_active = false;
}

uint32_t blocksIn() { return s_blocks_in; }
uint32_t blocksOut() { return s_blocks_out; }
// Bench: can D drive DD? Stops it, gives it a word, runs its own drive
// instructions, reads the lines back, releases them and starts it again.
bool s_selftest_released = false;
bool selfTestReleased() { return s_selftest_released; }
// Bench: one of D's registers (Y, X or ISR), put on DD and read back.
uint32_t readD(int reg_) {
    enum pio_src_dest reg = pio_src_dest(reg_);
    hw_clear_bits(&s_pio->ctrl, 1u << s_d);
    pio_sm_exec(s_pio, s_d, pio_encode_mov(pio_osr, reg));
    pio_sm_exec(s_pio, s_d, pio_encode_out(pio_pins, 16));
    pio_sm_exec(s_pio, s_d, pio_encode_mov_not(pio_osr, pio_null));
    pio_sm_exec(s_pio, s_d, pio_encode_out(pio_pindirs, 16));
    busy_wait_us(2);
    uint32_t seen = (sio_hw->gpio_in >> cfg::PIN_DD0) & 0xFFFF;
    pio_sm_exec(s_pio, s_d, pio_encode_mov(pio_osr, pio_null));
    pio_sm_exec(s_pio, s_d, pio_encode_out(pio_pindirs, 16));
    s_pio->sm[s_d].instr = s_d_start;
    hw_set_bits(&s_pio->ctrl, 1u << s_d);
    return seen;
}
uint32_t selfTest(uint16_t word) {
    hw_clear_bits(&s_pio->ctrl, 1u << s_d);
    s_pio->txf[s_d] = word;
    pio_sm_exec(s_pio, s_d, pio_encode_pull(false, false));
    pio_sm_exec(s_pio, s_d, pio_encode_out(pio_pins, 16));
    pio_sm_exec(s_pio, s_d, pio_encode_mov_not(pio_osr, pio_null));  // `out pindirs, 16` of ones, in
    pio_sm_exec(s_pio, s_d, pio_encode_out(pio_pindirs, 16));         // place of `mov pindirs, ~null`
    busy_wait_us(2);
    uint32_t seen = (sio_hw->gpio_in >> cfg::PIN_DD0) & 0xFFFF;
    uint32_t dirs = s_pio->dbg_padoe;
    pio_sm_exec(s_pio, s_d, pio_encode_mov(pio_pindirs, pio_null));
    busy_wait_us(1);
    s_selftest_released = ((s_pio->dbg_padoe >> cfg::PIN_DD0) & 0xFFFF) == 0;
    hw_xor_bits(&s_pio->sm[s_d].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_TX_BITS);
    hw_xor_bits(&s_pio->sm[s_d].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_TX_BITS);
    s_pio->sm[s_d].instr = s_d_start;
    hw_set_bits(&s_pio->ctrl, 1u << s_d);
    return seen | ((dirs >> cfg::PIN_DD0) & 0xFFFF) << 16;
}

// Bench: start a block from a known buffer, D stopped so nothing drains its
// FIFO, and follow the stream: words fetched (read address), count left, busy.
uint32_t s_trace[12];
uint32_t* streamTest() {
    static uint16_t words[256];
    for (unsigned i = 0; i < 256; ++i) words[i] = uint16_t(0x1000 + i);
    hw_set_bits(&s_pio->fdebug, 0xFFFFFFFFu);
    hw_clear_bits(&s_pio->ctrl, 1u << s_d);
    dmaAbort(s_stream);
    dReset();
    hw_clear_bits(&s_pio->ctrl, 1u << s_d);
    dma_hw->ch[s_stream].read_addr = uint32_t(uintptr_t(words));
    s_trace[0] = dma_hw->ch[s_stream].ctrl_trig;
    s_trace[1] = dma_hw->ch[s_stream].transfer_count;
    dma_hw->ch[s_stream].al1_transfer_count_trig = 257;
    for (unsigned k = 0; k < 4; ++k) {
        busy_wait_us(k ? 10 : 0);
        s_trace[2 + 2 * k] = (dma_hw->ch[s_stream].read_addr - uint32_t(uintptr_t(words))) / 2;
        s_trace[3 + 2 * k] = dma_hw->ch[s_stream].transfer_count | (dmaBusy(s_stream) ? 0x80000000u : 0);
    }
    s_trace[10] = (s_pio->flevel >> (s_d * 8)) & 0xF;
    s_trace[11] = s_pio->fdebug;
    dmaAbort(s_stream);
    dReset();
    return s_trace;
}

void blockRecords(uint32_t out[81]) {  // the last 16, oldest first
    out[0] = s_rec_n;
    uint32_t n = s_rec_n < 16 ? s_rec_n : 16, first = s_rec_n - n;
    for (unsigned k = 0; k < n; ++k)
        for (unsigned j = 0; j < 5; ++j) out[1 + k * 5 + j] = s_rec[(first + k) % 16][j];
}

// Bench: a data read as the programs see it, made with input overrides (no host
// needed): the address of the data register and DIOR# low, then high, for four
// words of a known block. Returns what DD showed during each read (low half)
// and whether it was released after (bit 16), per word.
uint32_t s_fake[40];
bool s_fake_cpu = false, s_fake_zero = false;
volatile bool s_bench_hold = false;
// Bench: core 1's view of the first sector block (count 256 after four IDENTIFY), each pass
volatile uint32_t s_seen[64][3];
volatile uint32_t s_seen_n = 0;
void armCommandCapture() { s_cap_cmd = true; }

// Bench: a timeline of the first sector block, sampled by core 1 once it has
// published the block: the pins (DD, DA, CS, DIOR#...) and D's PC and FIFO level.
constexpr unsigned CAP = 1024;
uint32_t s_cap_pins[CAP];
uint16_t s_cap_d[CAP];
volatile uint32_t s_cap_n = 0;
volatile bool s_cap_done = false;
CORE1_RAM_TABLE void notePass(uint32_t extra) {
    // Armed from the console (Z): wait here for the next Command write's busy DMA
    // to fire (C1 done), then capture at once.
    if (s_cap_cmd) {
        while (s_c1_armed && dmaBusy(s_c1)) {
            if (!s_cap_cmd) return;
        }
        s_cap_cmd = false;
        s_cap_arm = true;
        s_cap_done = false;
    }
    if (s_cap_arm && !s_cap_done) {
        s_cap_done = true;
        for (unsigned i = 0; i < CAP; ++i) {
            uint32_t start = m33_hw->dwt_cyccnt;
            s_cap_pins[i] = sio_hw->gpio_in;
            s_cap_d[i] = uint16_t(s_pio->sm[s_d].addr | ((s_pio->flevel >> (s_d * 8)) & 0xF) << 8);
            while (m33_hw->dwt_cyccnt - start < 20) {
            }
        }
        s_cap_n = CAP;
        s_cap_arm = false;
    }
    if (!s_in_active || !s_seen_on || s_seen_n >= 64) return;
    s_seen[s_seen_n][0] = m33_hw->dwt_cyccnt;
    s_seen[s_seen_n][1] = dma_hw->ch[s_stream].transfer_count | (dmaBusy(s_stream) ? 0x80000000u : 0);
    s_seen[s_seen_n][2] = ((s_pio->flevel >> (s_d * 8)) & 0xF) | extra << 8;
    s_seen_n = s_seen_n + 1;
}
const volatile uint32_t (*seenLog(uint32_t& n))[3] { n = s_seen_n; return s_seen; }
uint32_t capture(const uint32_t*& pins, const uint16_t*& d) { pins = s_cap_pins; d = s_cap_d; return s_cap_n; }
CORE1_RAM_TABLE bool benchHold() { return s_bench_hold; }
void fakeZero(bool on) { s_fake_zero = on; }
void fakeCpu(bool on) { s_fake_cpu = on; }
uint32_t s_fake_left = 0;
uint32_t s_fake_cycles = 200;
void fakeCycles(uint32_t c) { s_fake_cycles = c; }
uint32_t s_fake_padout = 0, s_fake_funcsel = 0, s_fake_pio = 0;
uint32_t fakeInfo(unsigned i) { return i == 0 ? s_fake_padout : i == 1 ? s_fake_funcsel : i == 2 ? s_fake_pio : s_fake_left; }
uint32_t* fakeReads() {
    static uint16_t words[256];
    for (unsigned i = 0; i < 256; ++i) words[i] = uint16_t(0xB000 + i);
    for (unsigned pin = cfg::PIN_DA0; pin < cfg::PIN_DA0 + 3; ++pin) gpio_set_inover(pin, GPIO_OVERRIDE_LOW);
    gpio_set_inover(cfg::PIN_CS0_N, GPIO_OVERRIDE_LOW);
    gpio_set_inover(cfg::PIN_CS1_N, GPIO_OVERRIDE_HIGH);
    gpio_set_inover(cfg::PIN_DIOR_N, GPIO_OVERRIDE_HIGH);
    gpio_set_inover(cfg::PIN_DIOW_N, GPIO_OVERRIDE_HIGH);
    s_bench_hold = true;  // core 1's loop leaves the blocks alone meanwhile
    busy_wait_us(50);
    if (s_fake_zero) s_pio->rxf_putget[s_r0][0] = 0;
    if (s_fake_cpu) {  // no DMA: the CPU puts four words in D's FIFO
        dmaAbort(s_stream);
        dReset();
        for (unsigned k = 0; k < 4; ++k) s_pio->txf[s_d] = 0xC000u + k;
    } else {
        startDataIn(words, 256);
    }
    for (unsigned k = 0; k < 40; ++k) {
        gpio_set_inover(cfg::PIN_DIOR_N, GPIO_OVERRIDE_LOW);
        busy_wait_at_least_cycles(s_fake_cycles);
        uint32_t seen = (sio_hw->gpio_in >> cfg::PIN_DD0) & 0xFFFF;
        uint32_t oe = (s_pio->dbg_padoe >> cfg::PIN_DD0) & 0xFFFF;
        if (k == 0) {
            s_fake_padout = (s_pio->dbg_padout >> cfg::PIN_DD0) & 0xFFFF;
            s_fake_funcsel = (io_bank0_hw->io[cfg::PIN_DD0].ctrl & IO_BANK0_GPIO0_CTRL_FUNCSEL_BITS);
            s_fake_pio = PIO_NUM(s_pio);
        }
        gpio_set_inover(cfg::PIN_DIOR_N, GPIO_OVERRIDE_HIGH);
        busy_wait_at_least_cycles(s_fake_cycles);
        uint32_t released = ((s_pio->dbg_padoe >> cfg::PIN_DD0) & 0xFFFF) == 0;
        s_fake[k] = seen | released << 16 | (oe == 0xFFFF ? 1u : 0u) << 17;
    }
    s_fake_left = dma_hw->ch[s_stream].transfer_count | (dmaBusy(s_stream) ? 0x80000000u : 0);
    if (!s_fake_cpu) stopDataIn();
    s_bench_hold = false;
    for (unsigned pin = cfg::PIN_DA0; pin <= cfg::PIN_DIOW_N; ++pin) gpio_set_inover(pin, GPIO_OVERRIDE_NORMAL);
    return s_fake;
}

// Bench: four words into D's FIFO, then its reset: the level before and after.
uint32_t flushTest() {
    hw_clear_bits(&s_pio->ctrl, 1u << s_d);
    for (unsigned k = 0; k < 4; ++k) s_pio->txf[s_d] = k;
    uint32_t before = (s_pio->flevel >> (s_d * 8)) & 0xF;
    dReset();
    uint32_t after = (s_pio->flevel >> (s_d * 8)) & 0xF;
    return before | after << 8;
}

// Bench: the Command busy path. Core 1 held; Status shows 0x50; C's word is
// pushed by hand (as a Command write would); Status is read (input overrides)
// after `wait_cycles`. Returns the value read, before (low byte) and after.
uint32_t busyTest(uint32_t wait_cycles) {
    s_bench_hold = true;
    busy_wait_us(50);
    s_pio->rxf_putget[s_r1][3] = DRIVE | 0x50;
    s_pio->rxf_putget[s_r2][2] = DRIVE | 0x50;
    if (!dmaBusy(s_c1)) armCommandBusy();
    auto status = [&]() {
        for (unsigned pin = cfg::PIN_DA0; pin < cfg::PIN_DA0 + 3; ++pin) gpio_set_inover(pin, GPIO_OVERRIDE_HIGH);
        gpio_set_inover(cfg::PIN_CS0_N, GPIO_OVERRIDE_LOW);
        gpio_set_inover(cfg::PIN_CS1_N, GPIO_OVERRIDE_HIGH);
        gpio_set_inover(cfg::PIN_DIOR_N, GPIO_OVERRIDE_LOW);
        busy_wait_us(1);
        uint32_t v = (sio_hw->gpio_in >> cfg::PIN_DD0) & 0xFF;
        gpio_set_inover(cfg::PIN_DIOR_N, GPIO_OVERRIDE_HIGH);
        busy_wait_us(1);
        return v;
    };
    uint32_t before = status();
    s_wpio->sm[s_c].instr = pio_encode_push(false, false);  // C's word, as at a Command write
    uint32_t t0 = m33_hw->dwt_cyccnt;
    while (m33_hw->dwt_cyccnt - t0 < wait_cycles) {
    }
    uint32_t after = status();
    for (unsigned pin = cfg::PIN_DA0; pin <= cfg::PIN_DIOW_N; ++pin) gpio_set_inover(pin, GPIO_OVERRIDE_NORMAL);
    uint32_t fired = !dmaBusy(s_c1);
    armCommandBusy();
    s_bench_hold = false;
    return before | after << 8 | fired << 16;
}

void debug(uint32_t out[6]) {
    out[0] = s_pio->sm[s_d].addr;                                  // D's program counter
    out[1] = (s_pio->flevel >> (s_d * 8)) & 0xF;                  // its TX level
    out[2] = s_cmds_taken | s_cmds_c1_fired << 16;                 // bench: commands taken, and C1 fired for
    out[3] = dma_hw->ch[s_c1].ctrl_trig | (s_c1_armed ? 1u << 31 : 0) | ((s_wpio->flevel >> (s_c * 8 + 4)) & 0xF) << 27;  // C1 (busy bit 26), armed, C's RX level
    out[4] = PIO_NUM(s_pio) | PIO_NUM(s_wpio) << 4;                // bench: the two blocks
    out[5] = s_pio->rxf_putget[s_r0][0];  // must stay 0: the data register is D's
}

}  // namespace ide_table
