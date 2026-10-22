// Trace events (recorded on core 1) and their text form (printed on core 0).
// Pure C++, host-tested.
//
// Every line starts with a keyword so the Python test runner can parse it:
//   EV <t_us> <op> <block> <addr> <reg> <data> x<count> <span_us> <pins>
//   RESET <t_us> ASSERT|RELEASE
//   STALL <t_us>
// op is RD, WR or BOTH; block is CMD, CTL, NONE or BOTH; data and pins are hex.
// A run of identical accesses becomes one EV line: <t_us> is the first one's
// time, x<count> how many, <span_us> the time from the first to the last.
#pragma once

#include <cstddef>
#include <cstdint>

enum class TraceKind : uint8_t { Bus, ResetAssert, ResetRelease, Stall };

struct TraceEvent {
    uint32_t time_us;
    uint32_t pins;  // bus sample (Bus) or the pins at the time (others)
    TraceKind kind;
};

struct TraceRun {
    TraceEvent first;
    uint32_t count;
    uint32_t last_us;
};

// Merges consecutive identical bus accesses (a host polling STATUS can make
// a million a second) into one run, so the serial link keeps up.
class TraceCoalescer {
public:
    // Adds an event. Returns true and fills `done` when this ends a run that
    // should now be printed. Non-bus events are never merged.
    bool add(const TraceEvent& e, TraceRun& done);
    // Ends the current run, if any. Returns true and fills `done` if there was one.
    bool flush(TraceRun& done);

private:
    bool has_run_ = false;
    TraceRun run_{};
};

// Formats a run as one line with a trailing newline. Returns the length
// (truncated to size - 1 if the buffer is too small).
size_t formatRun(const TraceRun& run, char* buf, size_t size);
