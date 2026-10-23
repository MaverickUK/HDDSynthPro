// CORE1_RAM: put a function in RAM instead of flash.
//
// Code normally runs from flash through a small cache shared by both cores.
// A cache miss stalls for a flash read, and core 0 (USB, trace printing)
// keeps evicting core 1's code, so a loop pass that should take well under
// 1 us could take tens of us: too slow to answer a read before the host gives
// up on IORDY. Everything core 1 runs per bus access is marked CORE1_RAM.
//
// The Arduino-Pico linker script copies `.time_critical*` sections to RAM at
// boot. On the host (unit tests) the macro is empty.
#pragma once

#if defined(ARDUINO)
#define CORE1_RAM __attribute__((section(".time_critical.core1")))
#else
#define CORE1_RAM
#endif
