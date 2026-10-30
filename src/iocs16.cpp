#include "iocs16.h"

#include <hardware/gpio.h>
#include <hardware/pio.h>

#include "config.h"
#include "iocs16.pio.h"

namespace iocs16 {

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
}

}  // namespace iocs16
