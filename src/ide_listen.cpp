#include "ide_listen.h"

#include <hardware/gpio.h>
#include <hardware/pio.h>

#include "config.h"
#include "ide_listen.pio.h"

namespace listen {

static PIO s_pio;
static uint s_sm;

void begin() {
    for (unsigned pin = cfg::BUS_FIRST_PIN; pin <= cfg::BUS_LAST_PIN; ++pin) {
        if (pin == cfg::PIN_BOARD_LED) continue;
        // SIO input: PIO can read the pin but cannot drive it.
        gpio_init(pin);
        // Never internal pull-downs (RP2350 erratum E9); the host has its own pulls.
        gpio_disable_pulls(pin);
    }

    uint offset;
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &ide_listen_program, &s_pio, &s_sm, &offset, cfg::BUS_FIRST_PIN,
        cfg::BUS_LAST_PIN - cfg::BUS_FIRST_PIN + 1, true);
    hard_assert(ok);

    pio_sm_config c = ide_listen_program_get_default_config(offset);
    sm_config_set_in_pins(&c, cfg::PIN_DD0);
    sm_config_set_out_shift(&c, true, false, 32);  // `out` takes the low bits first
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);  // 8-deep RX FIFO
    pio_sm_init(s_pio, s_sm, offset, &c);
    pio_sm_set_enabled(s_pio, s_sm, true);
}

bool pop(uint32_t& sample) {
    if (pio_sm_is_rx_fifo_empty(s_pio, s_sm)) return false;
    sample = pio_sm_get(s_pio, s_sm);
    return true;
}

bool takeStall() {
    uint32_t mask = 1u << (PIO_FDEBUG_RXSTALL_LSB + s_sm);
    if (!(s_pio->fdebug & mask)) return false;
    s_pio->fdebug = mask;  // write 1 to clear
    return true;
}

}  // namespace listen
