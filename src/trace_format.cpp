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

const char* commandName(uint8_t command) {
    switch (command) {
        case 0x10: return "RECALIBRATE";
        case 0x20: return "READ_SECTORS";
        case 0x40: return "READ_VERIFY";
        case 0x41: return "READ_VERIFY_NR";
        case 0x70: return "SEEK";
        case 0xE0: case 0x94: return "STANDBY_IMMED";
        case 0xE1: case 0x95: return "IDLE_IMMED";
        case 0xE2: case 0x96: return "STANDBY";
        case 0xE3: case 0x97: return "IDLE";
        case 0x21: return "READ_SECTORS_NR";
        case 0x90: return "EXEC_DIAGNOSTIC";
        case 0xEC: return "IDENTIFY";
        case 0xA1: return "IDENTIFY_PACKET";
        case 0xEF: return "SET_FEATURES";
        case 0x91: return "INIT_PARAMS";
        case 0xC6: return "SET_MULTIPLE";
        case 0xC4: return "READ_MULTIPLE";
        case 0xC8: return "READ_DMA";
        case 0x30: return "WRITE_SECTORS";
        case 0x31: return "WRITE_SECTORS_NR";
        case 0xE7: return "FLUSH_CACHE";
        case 0xE5: case 0x98: return "CHECK_POWER";
        default: return "?";
    }
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
        case TraceKind::CommandStart: {
            uint8_t command = uint8_t(e.extra);
            n = std::snprintf(buf, size, "CMD %lu %02x %s %lu %lu\n", (unsigned long)e.time_us,
                              (unsigned)command, commandName(command), (unsigned long)e.pins,
                              (unsigned long)(e.extra >> 8));
            break;
        }
        case TraceKind::CommandEnd:
            n = std::snprintf(buf, size, "DONE %lu %02x %02x %02x %lu\n", (unsigned long)e.time_us,
                              (unsigned)(e.extra & 0xFF), (unsigned)(e.pins & 0xFF),
                              (unsigned)((e.pins >> 8) & 0xFF), (unsigned long)(e.extra >> 8));
            break;
    }
    if (n < 0) return 0;
    return (size_t)n < size ? (size_t)n : size - 1;
}
