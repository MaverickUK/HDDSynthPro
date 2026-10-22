#include "ide_bus.h"

#include <hardware/gpio.h>
#include <hardware/pio.h>

#include "ata_device.h"
#include "bus_sample.h"
#include "config.h"
#include "ide_bus.pio.h"

namespace ide_bus {

static PIO s_pio;
static uint s_sm;

void begin() {
    for (unsigned pin = cfg::BUS_FIRST_PIN; pin <= cfg::BUS_LAST_PIN; ++pin) {
        if (pin == cfg::PIN_BOARD_LED) continue;
        gpio_init(pin);           // input until claimed below
        gpio_disable_pulls(pin);  // never internal pull-downs (RP2350 erratum E9)
    }
    // INTRQ: SIO, output value high but released until asserted.
    gpio_put(cfg::PIN_INTRQ, 1);
    gpio_set_dir(cfg::PIN_INTRQ, false);

    uint offset;
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &ide_bus_program, &s_pio, &s_sm, &offset, cfg::BUS_FIRST_PIN,
        cfg::BUS_LAST_PIN - cfg::BUS_FIRST_PIN + 1, true);
    hard_assert(ok);

    // DD0-15 and IORDY are driven by PIO. Both start as inputs (released);
    // IORDY's output value is 0, so making it an output pulls it low.
    for (unsigned i = 0; i < 16; ++i) pio_gpio_init(s_pio, cfg::PIN_DD0 + i);
    pio_gpio_init(s_pio, cfg::PIN_IORDY);
    pio_sm_set_pins_with_mask(s_pio, s_sm, 0, 1u << cfg::PIN_IORDY);
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 0, (0xFFFFu << cfg::PIN_DD0) | (1u << cfg::PIN_IORDY));

    pio_sm_config c = ide_bus_program_get_default_config(offset);
    sm_config_set_in_pins(&c, cfg::PIN_DD0);
    sm_config_set_out_pins(&c, cfg::PIN_DD0, 16);
    sm_config_set_sideset_pins(&c, cfg::PIN_IORDY);
    sm_config_set_jmp_pin(&c, cfg::PIN_DIOR_N);
    sm_config_set_out_shift(&c, true, false, 32);  // `out` takes the low bits first
    pio_sm_init(s_pio, s_sm, offset, &c);

    // DIOR# skips the 2-cycle input synchroniser, so DD is released sooner
    // after DIOR# rises: about 10 ns instead of 24 (ATA allows 30, t6z), which
    // leaves margin for faster PIO modes. It is a clean strobe, only ever
    // compared with a fixed level, so metastability is harmless.
    s_pio->input_sync_bypass |= 1u << (cfg::PIN_DIOR_N - pio_get_gpio_base(s_pio));

    pio_sm_set_enabled(s_pio, s_sm, true);
}

bool serviceOnce(ata::Device& dev, uint32_t& sample) {
    if (pio_sm_is_rx_fifo_empty(s_pio, s_sm)) return false;
    sample = pio_sm_get(s_pio, s_sm);
    bus::Block block = bus::block(sample);
    ata::Reg reg = ata::regFor(block == bus::Block::Command, block == bus::Block::Control,
                               bus::address(sample));
    if (bus::op(sample) == bus::Op::Write) {
        dev.write(reg, bus::data(sample));
    } else {
        // Every read must be answered, or the PIO holds IORDY low forever.
        pio_sm_put(s_pio, s_sm, dev.read(reg));
    }
    updateIntrq(dev.intrq());
    return true;
}

void updateIntrq(bool asserted) { gpio_set_dir(cfg::PIN_INTRQ, asserted); }

}  // namespace ide_bus
