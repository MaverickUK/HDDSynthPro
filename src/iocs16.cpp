#include "iocs16.h"

#include <hardware/gpio.h>
#include <hardware/pio.h>

#include "config.h"
#include "iocs16.pio.h"

namespace iocs16 {

static PIO s_pio = nullptr;
static uint s_sm = 0, s_offset = 0;

void begin() {
    PIO pio;
    uint sm;
    uint offset;
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &iocs16_program, &pio, &sm, &offset, cfg::PIN_IOCS16_N, cfg::PIN_CS0_N - cfg::PIN_IOCS16_N + 1, true);
    hard_assert(ok);

    // Open drain: the output value is 0, so making the pin an output pulls
    // it low. It starts released.
    gpio_disable_pulls(cfg::PIN_IOCS16_N);  // never internal pull-downs (RP2350 erratum E9)
    pio_gpio_init(pio, cfg::PIN_IOCS16_N);
    pio_sm_set_pins_with_mask(pio, sm, 0, 1u << cfg::PIN_IOCS16_N);
    pio_sm_set_pindirs_with_mask(pio, sm, 0, 1u << cfg::PIN_IOCS16_N);

    pio_sm_config c = iocs16_program_get_default_config(offset);
    sm_config_set_in_pins(&c, cfg::PIN_DA0);
    sm_config_set_sideset_pins(&c, cfg::PIN_IOCS16_N);
    sm_config_set_out_shift(&c, true, false, 32);  // `out` takes the low bits first: DA0-2, CS0#
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
    s_pio = pio, s_sm = sm, s_offset = offset;
}

bool running() { return s_pio != nullptr; }

void end() {
    if (!s_pio) return;
    pio_sm_set_pindirs_with_mask(s_pio, s_sm, 0, 1u << cfg::PIN_IOCS16_N);  // released
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_remove_program(s_pio, &iocs16_program, s_offset);
    pio_sm_unclaim(s_pio, s_sm);
    gpio_init(cfg::PIN_IOCS16_N);  // an input again (the host's pull-up holds it high)
    gpio_disable_pulls(cfg::PIN_IOCS16_N);
    s_pio = nullptr;
}

}  // namespace iocs16
