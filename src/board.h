// Which board the firmware runs on, and PCB rev 0.4's new lines
// (docs/pcb-rev04.md): the microSD card's power switch (SD_PWR#), DASP# as
// the PC case's disk light, and PDIAG# and CSEL, read only for now. All on
// core 0; nothing here is in core 1's path.
//
// One build serves rev 0.3 and rev 0.4: begin() tells them apart by SD_PWR#
// (GPIO 43), which rev 0.4 pulls down (4.7 kOhm) and rev 0.3 and the BB48
// leave unconnected. Until begin() has found rev 0.4, nothing here drives a pin.
#pragma once

namespace board {

// At start-up, before the card is started. Reads SD_PWR# with the internal
// pull-up on for a moment: low means rev 0.4's pull-down is there.
void begin();

// Whether this is PCB rev 0.4 (or later): its card power switch and DASP#,
// PDIAG# and CSEL are wired.
bool rev04();
// "rev0.4", or "rev0.3/bb48" (the two can't be told apart by this, and
// neither has the new lines).
const char* name();

// Rev 0.4: switches the card's power off, waits for its rail to drain, and
// on again. The caller must have stopped using the card; this releases the
// card's six lines first (SIO inputs, no pulls), so the card isn't powered
// through them. Blocks core 0 for about CARD_OFF_MS + CARD_ON_MS. False (and
// nothing done) on other boards.
bool cycleCardPower();
constexpr unsigned CARD_OFF_MS = 250;  // the rail has only the card to drain it (1.1 uF): see board.cpp
constexpr unsigned CARD_ON_MS = 20;    // the rail's ramp, then the card's own start (SD spec: 1 ms + 74 clocks)

// Rev 0.4: DASP#, the case's disk light, lit (pulled low) or let go. Open
// drain, never driven high: the host's LED resistor or the board's pull-up
// holds it up. Nothing on other boards.
void setActivity(bool on);

// Rev 0.4: the levels of DASP#, PDIAG# and CSEL as read (1 high, 0 low), for
// console `p`. CSEL low means the cable says device 0. -1 on other boards.
int daspLevel();
int pdiagLevel();
int cselLevel();

}  // namespace board
