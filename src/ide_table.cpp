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
constexpr unsigned ADDRESS_PINS = 5;      // block 0's reads: DA0-2, CS0#, CS1#
// Each entry carries the address it answers in bits 8-15, as a pin read gives
// it (DA0-2, CS0#, CS1#): ide_table.pio's R0-R2 answer only that address.
constexpr uint32_t at(unsigned address) { return address << 8; }
constexpr unsigned ERROR_ADDRESS = 17, SECTOR_COUNT_ADDRESS = 18, LBA_LOW_ADDRESS = 19, LBA_MID_ADDRESS = 20,
                   LBA_HIGH_ADDRESS = 21, DEVICE_ADDRESS = 22, ALT_STATUS_ADDRESS = 14;  // DA 6, CS1# low
// DD0-7 only: the registers are 8 bits, and a drive leaves DD8-15 to the host's pulls.
constexpr uint32_t DRIVE = 0x00FF0000;
// BSY, DRDY, DSC: what a drive shows while busy (the Innostor bridge took a bare 80h as no
// wait needed, 12 Nov 2026).
constexpr uint32_t BUSY = DRIVE | 0xD0;
constexpr uint32_t DRIVEN_PINS = (0xFFFFu << cfg::PIN_DD0) | (1u << cfg::PIN_IORDY);
constexpr uint32_t PULL_NOBLOCK = 0x8080;  // pio_encode_pull(false, false), for core 1
constexpr uint32_t SET_PINS_0 = 0xe000, SET_PINS_1 = 0xe001;        // pio_encode_set(pio_pins, v)
constexpr uint32_t SET_PINDIRS_0 = 0xe080, SET_PINDIRS_1 = 0xe081;  // pio_encode_set(pio_pindirs, v)
constexpr uint32_t MOV_PINDIRS_NULL = 0xa063;  // mov pindirs, null
constexpr uint32_t SIDE_IORDY_RELEASE = 0xb042;  // nop side 0 (D: side-set 1 bit + enable)

PIO s_pio = nullptr;      // block 0: the read side
uint s_r0, s_r1, s_r2, s_d;
PIO s_wpio = nullptr;     // block 1: the write side
uint s_w, s_wd, s_c;
bool s_running = false;
uint32_t s_d_start = 0;  // `jmp` to D's start, for dReset
uint32_t s_d_midread = 0;  // and to where it waits out a read already under way
uint32_t s_reg_start = 0;  // `jmp` to R0-R2's start (sample), for restart

// DMA channels (the design note's table)
uint s_stream;            // a data-in block (and a spare word) into D's TX FIFO; chains Ka
uint s_c1;                // C's word: starts the busy writes
uint s_ka, s_kb;          // the busy word into R1's Status entry, then R2's Alternate Status
uint s_wdma;              // WD's words into the sector buffer
uint s_wring;             // W's samples into the ring

// The busy word, for Ka and Kb; a dummy for C1's word.
uint32_t s_busy_status = BUSY | at(COMMAND_ADDRESS), s_busy_alt = BUSY | at(ALT_STATUS_ADDRESS);  // Ka's and Kb's words
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
    // DIOR# skips the input synchroniser, so D pulls IORDY low inside tA (35 ns)
    // and R0-R2 drive DD within about 30 ns; everything else goes through it, and
    // R0-R2 and D first look at the address as of two cycles after the strobe,
    // then follow it while DIOR# is low (ide_table.pio).
    s_pio->input_sync_bypass &= ~(0xFFFFFFFFu >> (32 - (cfg::BUS_LAST_PIN + 1)));
    s_pio->input_sync_bypass |= 1u << (cfg::PIN_DIOR_N - pio_get_gpio_base(s_pio));

    const uint regs[3] = {s_r0, s_r1, s_r2};
    for (unsigned k = 0; k < 3; ++k) {
        pio_sm_config c = ide_table_reg_program_get_default_config(reg_offset);  // .fifo txget
        sm_config_set_in_pins(&c, cfg::PIN_DA0);  // a pin read is the address alone:
        sm_config_set_in_pin_count(&c, ADDRESS_PINS);  // DA0-2, CS0#, CS1#
        sm_config_set_jmp_pin(&c, cfg::PIN_DIOR_N);  // `jmp pin`: spin until the strobe
        sm_config_set_out_pins(&c, cfg::PIN_DD0, 16);
        sm_config_set_out_shift(&c, true, false, 32);
        pio_sm_init(s_pio, regs[k], reg_offset + ide_table_reg_offset_sample, &c);
        s_reg_start = pio_encode_jmp(reg_offset + ide_table_reg_offset_sample);
    }
    // The register image before anything reads it: nothing driven, BSY. After
    // pio_sm_init, which turns the FIFO into registers: written before, the
    // entries kept whatever they held, and R0's entry 0 answered every data
    // read over D (12 Nov 2026).
    for (uint sm : {s_r0, s_r1, s_r2})
        for (unsigned i = 0; i < 4; ++i) s_pio->rxf_putget[sm][i] = 0;
    s_pio->rxf_putget[s_r1][3] = s_busy_status;
    s_pio->rxf_putget[s_r2][2] = s_busy_alt;
    {
        pio_sm_config c = ide_table_data_program_get_default_config(data_offset);
        sm_config_set_in_pins(&c, cfg::PIN_DA0);  // as R0-R2: the address
        sm_config_set_in_pin_count(&c, ADDRESS_PINS);
        sm_config_set_jmp_pin(&c, cfg::PIN_DIOR_N);  // `jmp pin`: DIOR# has risen
        sm_config_set_out_pins(&c, cfg::PIN_DD0, 16);
        sm_config_set_sideset_pins(&c, cfg::PIN_IORDY);
        sm_config_set_out_shift(&c, true, false, 32);  // `out pins, 16` takes the low half
        sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);  // 8 deep, as the data lane's ide_data_in
        pio_sm_init(s_pio, s_d, data_offset + ide_table_data_offset_top, &c);
        pio_sm_exec(s_pio, s_d, pio_encode_set(pio_y, DATA_ADDRESS));
        s_d_start = pio_encode_jmp(data_offset + ide_table_data_offset_top);
        s_d_midread = pio_encode_jmp(data_offset + ide_table_data_offset_isreg);
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
    oneShot(s_ka, DMA_SIZE_32, false, DREQ_FORCE, s_kb, &s_pio->rxf_putget[s_r1][3], &s_busy_status);
    oneShot(s_kb, DMA_SIZE_32, false, DREQ_FORCE, s_kb, &s_pio->rxf_putget[s_r2][2], &s_busy_alt);
    // C1: C's word at a Command write.
    oneShot(s_c1, DMA_SIZE_32, false, pio_get_dreq(s_wpio, s_c, false), s_ka, &s_dummy, &s_wpio->rxf[s_c]);
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
    // To its start; or, with a read under way (the host polling Status for the
    // DRQ this block brings), to where it waits that read out. From its start it
    // took the read as new and pulled IORDY low a second time, mid-strobe, and
    // the Innostor bridge's IORDY takes a microsecond to rise again.
    s_pio->sm[s_d].instr = (sio_hw->gpio_in & (1u << cfg::PIN_DIOR_N)) ? s_d_start : s_d_midread;
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
    pio->rxf_putget[s_r0][1] = image.error | at(ERROR_ADDRESS);
    pio->rxf_putget[s_r0][2] = image.sector_count | at(SECTOR_COUNT_ADDRESS);
    pio->rxf_putget[s_r0][3] = image.lba_low | at(LBA_LOW_ADDRESS);
    pio->rxf_putget[s_r1][0] = image.lba_mid | at(LBA_MID_ADDRESS);
    pio->rxf_putget[s_r1][1] = image.lba_high | at(LBA_HIGH_ADDRESS);
    pio->rxf_putget[s_r1][2] = image.device | at(DEVICE_ADDRESS);
    // Status last, and checked again after: a busy write that lands between the
    // check and the write would otherwise be overwritten.
    uint32_t status = busyOwed() ? BUSY : image.status;
    pio->rxf_putget[s_r1][3] = status | at(COMMAND_ADDRESS);
    pio->rxf_putget[s_r2][2] = status | at(ALT_STATUS_ADDRESS);
    if (status != BUSY && busyOwed()) {
        pio->rxf_putget[s_r1][3] = s_busy_status;
        pio->rxf_putget[s_r2][2] = s_busy_alt;
    }
}

CORE1_RAM_TABLE bool popWrite(uint32_t& sample) {
    uint32_t written = (dma_hw->ch[s_wring].write_addr - uint32_t(uintptr_t(s_wring_buf))) / 4;
    if (written == s_wring_read) return false;
    sample = s_wring_buf[s_wring_read];
    s_wring_read = (s_wring_read + 1) % WRING_WORDS;
    return true;
}

// The observer's ring, read in order: s_obs_count counts every entry taken, so
// a position is absolute (it doesn't wrap as the ring's index does).
uint32_t s_obs_count = 0;

CORE1_RAM_TABLE uint32_t observerPosition() {
    if (!s_obs_ring) return 0;
    uint32_t written = (dma_hw->ch[s_obs_dma].write_addr - uint32_t(uintptr_t(s_obs_ring))) / 4;
    return s_obs_count + (written - s_obs_read + s_obs_words) % s_obs_words;
}

// A Status read at or after absolute position `since` (when the interrupt it
// would acknowledge was raised), showing the device not busy: earlier ones,
// and ones answered before the raise, belong to an earlier interrupt.
CORE1_RAM_TABLE bool statusReadSince(uint32_t since) {
    if (!s_obs_ring) return false;
    uint32_t written = (dma_hw->ch[s_obs_dma].write_addr - uint32_t(uintptr_t(s_obs_ring))) / 4;
    bool seen = false;
    while (s_obs_read != written) {
        uint32_t word = s_obs_ring[s_obs_read];
        s_obs_read = (s_obs_read + 1) % s_obs_words;
        // A Status read the host saw BSY in came before the interrupt (the device
        // raises it only once it isn't busy), even if its strobe ended after the
        // raise: the slice 4 writes lost their interrupt to one (12 Nov 2026).
        if (((word >> 16) & 0x1F) == COMMAND_ADDRESS && !(word & 0x80) && int32_t(s_obs_count - since) >= 0)
            seen = true;
        s_obs_count = s_obs_count + 1;
    }
    return seen;
}

// Console V: the drive off the bus, as if unplugged. R0-R2 and D stopped and
// DD, IORDY and INTRQ released; restart() brings them back at their starts.
CORE1_RAM_TABLE void stop() {
    hw_clear_bits(&s_pio->ctrl, (1u << s_r0) | (1u << s_r1) | (1u << s_r2) | (1u << s_d));
    s_pio->sm[s_d].instr = MOV_PINDIRS_NULL;   // DD released (D's OUT mapping is DD0-15)
    s_pio->sm[s_d].instr = SIDE_IORDY_RELEASE;  // IORDY released (D's side-set)
    setIntrq(false, false);
}

CORE1_RAM_TABLE void restart() {
    uint32_t mask = (1u << s_r0) | (1u << s_r1) | (1u << s_r2) | (1u << s_d);
    for (uint sm : {s_r0, s_r1, s_r2}) {
        hw_set_bits(&s_pio->ctrl, 1u << (PIO_CTRL_SM_RESTART_LSB + sm));
        s_pio->sm[sm].instr = s_reg_start;
    }
    dReset();  // D at its start, its FIFO empty (and enabled)
    hw_set_bits(&s_pio->ctrl, mask);
}

CORE1_RAM_TABLE void armCommandBusy() {
    dmaRetrigger(s_c1);  // waits for C's next word
    s_c1_armed = true;
}

// INTRQ: C lowers it itself as a Command write starts; core 1 sets it here when
// the device's interrupt changes (forced instructions on C, which waits
// meanwhile, with SET pins = INTRQ).
volatile uint32_t s_intrq_rises = 0;  // times core 1 drove INTRQ high, for `s`
uint32_t intrqRises() { return s_intrq_rises; }
CORE1_RAM_TABLE void setIntrq(bool asserted, bool driven) {
    if (asserted && driven) s_intrq_rises = s_intrq_rises + 1;
    s_wpio->sm[s_c].instr = asserted ? SET_PINS_1 : SET_PINS_0;
    s_wpio->sm[s_c].instr = driven ? SET_PINDIRS_1 : SET_PINDIRS_0;
}

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
}

CORE1_RAM_TABLE bool dataInActive() { return s_in_active; }

// The host has read the whole block: every word in the FIFO, and only the
// spare word left in it (D never pulls that one).
CORE1_RAM_TABLE bool dataInDone() {
    return s_in_active && !dmaBusy(s_stream) && ((s_pio->flevel >> (s_d * 8)) & 0xF) <= 1;
}

CORE1_RAM_TABLE void stopDataIn() {
    if (s_in_active && !dmaBusy(s_stream)) s_blocks_in = s_blocks_in + 1;
    // A block cut short (a new command, SRST, a reset) leaves words behind: the
    // stream's, and up to 8 in D's FIFO, which a host reading data before the
    // next block's DRQ would get as if they were its answer. Empty it, so such a
    // read gets junk, as outside any block. A block read to its end leaves only
    // the spare word, which startDataIn's reset takes.
    const bool running = dmaBusy(s_stream);
    if (running) dmaAbort(s_stream);  // never an idle channel (see startDataIn)
    if (running || ((s_pio->flevel >> (s_d * 8)) & 0xF) > 1) dReset();
    s_in_active = false;
}

CORE1_RAM_TABLE void startDataOut(uint16_t* dest, uint16_t count) {
    if (dmaBusy(s_wdma)) dmaAbort(s_wdma);
    // Words WD took outside a block (a block cut short by a reset, with the host
    // still writing) would come first in this one: empty its FIFO. The host
    // can't have written any of this block yet: it hasn't seen DRQ.
    while ((s_wpio->flevel >> (s_wd * 8 + 4)) & 0xF) (void)s_wpio->rxf[s_wd];
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

CORE1_RAM_TABLE uint32_t blocksIn() { return s_blocks_in; }
CORE1_RAM_TABLE uint32_t blocksOut() { return s_blocks_out; }

}  // namespace ide_table
