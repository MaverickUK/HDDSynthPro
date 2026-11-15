// How deep each core's stack has ever gone, for console `s` (`stack0_max`,
// `stack1_max`, in bytes).
//
// Each core's stack sits at the top of a 4 KB region of its own (SCRATCH_Y for
// core 0, SCRATCH_X for core 1; the linker script), which holds nothing else
// in this firmware. The SDK starts core 1 with a 2 KB stack at the top of its
// region (the 2 KB below it is the SDK's unused `core1_stack` array), so a
// figure over 2,048 for core 1 is past what the SDK set aside, and 4,096 for
// either means it ran out of its region into whatever lies below.
//
// paint() fills the region below the caller's stack pointer with a pattern,
// once, from the core whose stack it is; used() looks for the lowest word that
// no longer holds it. Reading the other core's stack is safe: it only reads.
#pragma once

#include <cstdint>

namespace stack_watch {

constexpr uint32_t REGION_BYTES = 4096;

void paint(unsigned core);     // from that core, early in its setup
uint32_t used(unsigned core);  // the most bytes ever in use; 0 before paint()

}  // namespace stack_watch
