// PCB rev 0.4's new lines (board.h, docs/pcb-rev04.md). Core 0 only.
#include "board.h"

#include <Arduino.h>
#include <hardware/gpio.h>

#include "config.h"

namespace board {

static bool s_rev04 = false;

// An input with no internal pulls (RP2350 erratum E9 rules out the pull-down;
// the board has its own pull-ups on all of these).
static void plainInput(unsigned pin) {
    gpio_init(pin);  // SIO, input
    gpio_disable_pulls(pin);
}

void begin() {
    // Rev 0.4's 4.7 kOhm pull-down holds SD_PWR# near 0.3 V against the
    // internal pull-up (about 50 kOhm), so the card stays powered while we
    // look. Unconnected (rev 0.3, the BB48), the pull-up takes it high.
    gpio_init(cfg::PIN_SD_PWR_N);
    gpio_pull_up(cfg::PIN_SD_PWR_N);
    busy_wait_us(100);  // the pin's capacitance through 50 kOhm: microseconds
    s_rev04 = !gpio_get(cfg::PIN_SD_PWR_N);
    gpio_disable_pulls(cfg::PIN_SD_PWR_N);
    if (!s_rev04) return;  // nothing else is wired: leave the rest alone

    // SD_PWR#: an input, so the board's pull-down keeps the card on. The
    // output latch is set high, ready for cycleCardPower() to switch the
    // pin to an output.
    gpio_put(cfg::PIN_SD_PWR_N, true);

    // DASP#: open drain by direction. The latch stays low; making the pin
    // an output pulls the line low, an input lets it go. 12 mA, for a case
    // LED fed from the host's 5 V through its resistor (docs/pcb-rev04.md, 5).
    plainInput(cfg::PIN_DASP_N);
    gpio_put(cfg::PIN_DASP_N, false);
    gpio_set_drive_strength(cfg::PIN_DASP_N, GPIO_DRIVE_STRENGTH_12MA);

    plainInput(cfg::PIN_PDIAG_N);  // read only: emulating both devices needs no handshake
    plainInput(cfg::PIN_CSEL);
}

bool rev04() { return s_rev04; }

const char* name() { return s_rev04 ? "rev0.4" : "rev0.3/bb48"; }

bool cycleCardPower() {
    if (!s_rev04) return false;
    // The card's lines as inputs first. Rev 0.4 moved their pull-ups to the
    // switched rail, so nothing feeds an unpowered card, as long as we don't.
    plainInput(cfg::PIN_SD_CLK);
    plainInput(cfg::PIN_SD_CMD);
    for (unsigned pin = cfg::PIN_SD_DAT0; pin <= cfg::PIN_SD_DAT3; ++pin) plainInput(pin);  // DAT0-3, consecutive
    gpio_set_dir(cfg::PIN_SD_PWR_N, true);  // drives the gate high: power off
    // The switched rail has 1.1 uF on it and only the card to drain it: a
    // card idling at 100 uA takes it to 0.5 V in about 30 ms. 250 ms leaves
    // room for one that draws less. To be measured on the first board.
    delay(CARD_OFF_MS);
    gpio_set_dir(cfg::PIN_SD_PWR_N, false);  // let go: the pull-down turns it on
    delay(CARD_ON_MS);
    return true;
}

void setActivity(bool on) {
    if (s_rev04) gpio_set_dir(cfg::PIN_DASP_N, on);
}

static int level(unsigned pin) { return s_rev04 ? int(gpio_get(pin)) : -1; }
int daspLevel() { return level(cfg::PIN_DASP_N); }
int pdiagLevel() { return level(cfg::PIN_PDIAG_N); }
int cselLevel() { return level(cfg::PIN_CSEL); }

}  // namespace board
