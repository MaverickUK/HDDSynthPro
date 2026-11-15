#include "stack_watch.h"

extern "C" char __StackTop, __StackOneTop;  // the ends of SCRATCH_Y and SCRATCH_X (the linker script)

namespace stack_watch {

namespace {

constexpr uint32_t PATTERN = 0x57AC4B0D;
constexpr uint32_t WORDS = REGION_BYTES / 4;
bool s_painted[2];

// As numbers: the symbols mark addresses, not objects, and pointer arithmetic
// from them has the compiler warn about bounds.
uintptr_t top(unsigned core) { return reinterpret_cast<uintptr_t>(core ? &__StackOneTop : &__StackTop); }
uint32_t* base(unsigned core) { return reinterpret_cast<uint32_t*>(top(core) - REGION_BYTES); }

}  // namespace

// Not inlined, so the frame address is this function's own, below everything
// the caller has in use; a margin keeps clear of this function's frame too.
__attribute__((noinline)) void paint(unsigned core) {
    core = core ? 1 : 0;
    uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0)) - 64;
    if (sp > top(core) || sp < top(core) - REGION_BYTES) return;  // not on this stack: leave it alone
    volatile uint32_t* words = base(core);
    for (uint32_t i = 0; i < (sp - (top(core) - REGION_BYTES)) / 4; ++i) words[i] = PATTERN;
    s_painted[core] = true;
}

uint32_t used(unsigned core) {
    core = core ? 1 : 0;
    if (!s_painted[core]) return 0;
    const volatile uint32_t* words = base(core);
    uint32_t clean = 0;
    while (clean < WORDS && words[clean] == PATTERN) ++clean;
    return (WORDS - clean) * 4;
}

}  // namespace stack_watch
