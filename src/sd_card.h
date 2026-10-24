// The microSD card (slice 5), on core 0. For now only a check that the card
// answers: console `S` starts it over SPI and reports what it found.
#pragma once

#include <Arduino.h>

namespace sd_card {

// Starts the card and prints one line: "SD ok type=... size_mb=... fs=..."
// or "SD error ...". Blocks core 0 for up to about a second.
void printInfo(Print& out);

}  // namespace sd_card
