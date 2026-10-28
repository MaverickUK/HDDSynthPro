#include "ide_dma.h"

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/structs/io_bank0.h>

#include "ata_device.h"
#include "config.h"
#include "core1_ram.h"
#include "ide_dma.pio.h"

namespace ide_dma {

static PIO s_pio;
static uint s_sm;
static uint s_read_offset;
static uint s_write_offset;
static pio_sm_config s_read_config;
static pio_sm_config s_write_config;
static int s_dma;
static dma_channel_config s_dma_to_fifo;    // reads: the buffer into the TX FIFO
static dma_channel_config s_dma_from_fifo;  // writes: the RX FIFO into the buffer
static uint32_t s_bus_funcsel;   // DD0-15's function: ide_bus's PIO block
static uint32_t s_dma_funcsel;   // this block's
static bool s_writing = false;   // which program the state machine runs
static bool s_running = false;   // a burst under way
static bool s_active = false;    // DMARQ driven: a DMA command under way
static bool s_read_burst = false;

// GPIO output enable override (IO_BANK0 GPIOn_CTRL.OEOVER): 0 = as the
// peripheral says, 2 = always off.
constexpr uint32_t OE_FROM_PERIPHERAL = 0;
constexpr uint32_t OE_OFF = 2;

CORE1_RAM static void setOutputEnable(uint pin, uint32_t over) {
    hw_write_masked(&io_bank0_hw->io[pin].ctrl, over << IO_BANK0_GPIO0_CTRL_OEOVER_LSB,
                    IO_BANK0_GPIO0_CTRL_OEOVER_BITS);
}

// Hands DD0-15 to `funcsel`'s PIO block: the only one whose outputs reach them.
CORE1_RAM static void dataLinesTo(uint32_t funcsel) {
    for (uint pin = cfg::PIN_DD0; pin < cfg::PIN_DD0 + 16; ++pin)
        hw_write_masked(&io_bank0_hw->io[pin].ctrl, funcsel << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB,
                        IO_BANK0_GPIO0_CTRL_FUNCSEL_BITS);
}

static pio_sm_config makeConfig(pio_sm_config c, bool join_tx) {
    sm_config_set_in_pins(&c, cfg::PIN_DD0);
    sm_config_set_out_pins(&c, cfg::PIN_DD0, 16);
    sm_config_set_sideset_pins(&c, cfg::PIN_DMARQ);
    sm_config_set_in_shift(&c, true, false, 32);   // `in` shifts right; pushes are explicit
    sm_config_set_out_shift(&c, true, false, 32);  // `out` takes the low bits first; pulls are explicit
    if (join_tx) sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);  // reads: 8 words queued ahead
    return c;
}

// Points the state machine at one program's start, FIFOs empty (it waits there
// for a burst's count). Inline SDK calls only: core 1 runs this per command.
CORE1_RAM static void useProgram(bool write) {
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_set_config(s_pio, s_sm, write ? &s_write_config : &s_read_config);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    pio_interrupt_clear(s_pio, 0);
    uint offset = write ? s_write_offset : s_read_offset;
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(offset + (write ? ide_dma_write_offset_start : ide_dma_read_offset_start)));
    pio_sm_set_enabled(s_pio, s_sm, true);
    s_writing = write;
}

void begin() {
    // Both programs in one block (they fill its 32 instructions): the first
    // block with room, after ide_bus, the IORDY timer and the bus observer.
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &ide_dma_read_program, &s_pio, &s_sm, &s_read_offset, cfg::PIN_DD0,
        cfg::PIN_DMARQ - cfg::PIN_DD0 + 1, true);
    hard_assert(ok);
    hard_assert(pio_can_add_program(s_pio, &ide_dma_write_program));
    s_write_offset = pio_add_program(s_pio, &ide_dma_write_program);
    s_read_config = makeConfig(ide_dma_read_program_get_default_config(s_read_offset), true);
    s_write_config = makeConfig(ide_dma_write_program_get_default_config(s_write_offset), false);

    // DIOR# and DIOW# skip the input synchroniser, as in ide_bus (here also
    // so a write's DD sample comes from just before DIOW# rises).
    uint base = pio_get_gpio_base(s_pio);
    s_pio->input_sync_bypass |= (1u << (cfg::PIN_DIOR_N - base)) | (1u << (cfg::PIN_DIOW_N - base));

    // DMARQ: this block's output, but forced off until a DMA command
    // (pio_gpio_init rewrites the whole control register, override included).
    pio_gpio_init(s_pio, cfg::PIN_DMARQ);
    setOutputEnable(cfg::PIN_DMARQ, OE_OFF);
    pio_sm_set_pins_with_mask(s_pio, s_sm, 0, 1u << cfg::PIN_DMARQ);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 1u << cfg::PIN_DMARQ, 1u << cfg::PIN_DMARQ);

    s_bus_funcsel = (io_bank0_hw->io[cfg::PIN_DD0].ctrl & IO_BANK0_GPIO0_CTRL_FUNCSEL_BITS) >>
                    IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
    s_dma_funcsel = pio_get_funcsel(s_pio);

    // The DMA channel's two set-ups, made here: the SDK's helpers aren't all
    // inline, and core 1 runs only from RAM.
    s_dma = dma_claim_unused_channel(true);
    s_dma_to_fifo = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&s_dma_to_fifo, DMA_SIZE_16);  // a 16-bit write puts the word in both halves
    channel_config_set_read_increment(&s_dma_to_fifo, true);
    channel_config_set_write_increment(&s_dma_to_fifo, false);
    channel_config_set_dreq(&s_dma_to_fifo, pio_get_dreq(s_pio, s_sm, true));
    s_dma_from_fifo = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&s_dma_from_fifo, DMA_SIZE_16);  // the word, from the low half
    channel_config_set_read_increment(&s_dma_from_fifo, false);
    channel_config_set_write_increment(&s_dma_from_fifo, true);
    channel_config_set_dreq(&s_dma_from_fifo, pio_get_dreq(s_pio, s_sm, false));
    useProgram(false);
}

CORE1_RAM void startBurst(const ata::DmaBurst& burst) {
    bool write = burst.to != nullptr;
    if (write != s_writing) useProgram(write);
    if (!s_active) {
        setOutputEnable(cfg::PIN_DMARQ, OE_FROM_PERIPHERAL);  // low until the program raises it
        s_active = true;
    }
    s_read_burst = !write;
    if (s_read_burst) dataLinesTo(s_dma_funcsel);  // between strobes the program leaves DD released
    s_running = true;

    // The count first, then the words: the program pulls them in that order.
    pio_sm_put(s_pio, s_sm, uint32_t(burst.count - 1));
    if (write)
        dma_channel_configure(s_dma, &s_dma_from_fifo, burst.to, &s_pio->rxf[s_sm], burst.count, true);
    else
        dma_channel_configure(s_dma, &s_dma_to_fifo, &s_pio->txf[s_sm], burst.from, burst.count, true);
}

CORE1_RAM bool burstRunning() { return s_running; }

CORE1_RAM bool burstDone() {
    if (!s_running || !pio_interrupt_get(s_pio, 0)) return false;
    if (dma_channel_is_busy(s_dma)) return false;  // a write's last word still on its way
    pio_interrupt_clear(s_pio, 0);
    if (s_read_burst) dataLinesTo(s_bus_funcsel);
    s_running = false;
    return true;
}

CORE1_RAM bool active() { return s_active; }

CORE1_RAM void endCommand() {
    if (!s_active) return;
    setOutputEnable(cfg::PIN_DMARQ, OE_OFF);
    dma_channel_abort(s_dma);
    dataLinesTo(s_bus_funcsel);
    useProgram(s_writing);  // back at the start, FIFOs empty, DMARQ low
    s_running = false;
    s_active = false;
}

}  // namespace ide_dma
