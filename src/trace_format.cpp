#include "trace_format.h"

#include <cstdio>

#include "bus_sample.h"

static bool sameAccess(const TraceEvent& a, const TraceEvent& b) {
    return a.kind == TraceKind::Bus && b.kind == TraceKind::Bus &&
           (a.pins & bus::ACCESS_MASK) == (b.pins & bus::ACCESS_MASK);
}

bool TraceCoalescer::add(const TraceEvent& e, TraceRun& done) {
    if (has_run_ && sameAccess(run_.first, e)) {
        ++run_.count;
        run_.last_us = e.time_us;
        return false;
    }
    bool ended = has_run_;
    if (ended) done = run_;
    run_ = TraceRun{e, 1, e.time_us};
    has_run_ = true;
    return ended;
}

bool TraceCoalescer::flush(TraceRun& done) {
    if (!has_run_) return false;
    done = run_;
    has_run_ = false;
    return true;
}

static const char* opName(bus::Op op) {
    switch (op) {
        case bus::Op::Read: return "RD";
        case bus::Op::Write: return "WR";
        case bus::Op::Both: return "BOTH";
        default: return "NONE";
    }
}

static const char* blockName(bus::Block b) {
    switch (b) {
        case bus::Block::Command: return "CMD";
        case bus::Block::Control: return "CTL";
        case bus::Block::Both: return "BOTH";
        default: return "NONE";
    }
}

size_t formatRun(const TraceRun& run, char* buf, size_t size) {
    const TraceEvent& e = run.first;
    int n = 0;
    switch (e.kind) {
        case TraceKind::Bus: {
            bus::Op op = bus::op(e.pins);
            bus::Block block = bus::block(e.pins);
            unsigned addr = bus::address(e.pins);
            n = std::snprintf(buf, size, "EV %lu %s %s %u %s %04x x%lu %lu %08lx\n",
                              (unsigned long)e.time_us, opName(op), blockName(block), addr,
                              bus::registerName(block, addr, op == bus::Op::Write),
                              (unsigned)bus::data(e.pins), (unsigned long)run.count,
                              (unsigned long)(run.last_us - e.time_us), (unsigned long)e.pins);
            break;
        }
        case TraceKind::ResetAssert:
            n = std::snprintf(buf, size, "RESET %lu ASSERT\n", (unsigned long)e.time_us);
            break;
        case TraceKind::ResetRelease:
            n = std::snprintf(buf, size, "RESET %lu RELEASE\n", (unsigned long)e.time_us);
            break;
        case TraceKind::Stall:
            n = std::snprintf(buf, size, "STALL %lu\n", (unsigned long)e.time_us);
            break;
    }
    if (n < 0) return 0;
    return (size_t)n < size ? (size_t)n : size - 1;
}
