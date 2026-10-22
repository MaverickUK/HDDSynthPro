// Decoding a 32-bit pin sample taken by PIO with IN_BASE = PIN_DD0.
// Bit n of a sample is GPIO (PIN_DD0 + n). Pure C++.
#pragma once

#include <cstdint>

#include "config.h"

namespace bus {

constexpr unsigned bitOf(unsigned pin) { return pin - cfg::PIN_DD0; }
constexpr uint32_t maskOf(unsigned pin) { return 1u << bitOf(pin); }

// Lines that identify an access. Two samples with the same masked value are
// the same access (IORDY, INTRQ, RESET# and the LED are left out).
constexpr uint32_t ACCESS_MASK = 0xFFFFu | (7u << bitOf(cfg::PIN_DA0)) | maskOf(cfg::PIN_CS0_N) |
                                 maskOf(cfg::PIN_CS1_N) | maskOf(cfg::PIN_DIOR_N) |
                                 maskOf(cfg::PIN_DIOW_N);

inline uint16_t data(uint32_t s) { return static_cast<uint16_t>(s & 0xFFFFu); }
inline unsigned address(uint32_t s) { return (s >> bitOf(cfg::PIN_DA0)) & 7u; }
// Active-low lines: true means asserted (pin low).
inline bool asserted(uint32_t s, unsigned pin) { return (s & maskOf(pin)) == 0; }
inline bool high(uint32_t s, unsigned pin) { return (s & maskOf(pin)) != 0; }

enum class Op { None, Read, Write, Both };

inline Op op(uint32_t s) {
    bool rd = asserted(s, cfg::PIN_DIOR_N);
    bool wr = asserted(s, cfg::PIN_DIOW_N);
    if (rd && wr) return Op::Both;  // never valid: the host must not do this
    if (rd) return Op::Read;
    if (wr) return Op::Write;
    return Op::None;
}

// Which register block the chip selects pick.
enum class Block { None, Command, Control, Both };

inline Block block(uint32_t s) {
    bool cs0 = asserted(s, cfg::PIN_CS0_N);
    bool cs1 = asserted(s, cfg::PIN_CS1_N);
    if (cs0 && cs1) return Block::Both;  // invalid in PIO transfers
    if (cs0) return Block::Command;
    if (cs1) return Block::Control;
    return Block::None;
}

// ATA register name for an access; reads and writes of the same address
// can be different registers (STATUS vs COMMAND).
inline const char* registerName(Block b, unsigned addr, bool write) {
    if (b == Block::Command) {
        static const char* const reads[8] = {"DATA",   "ERROR",   "SECCOUNT", "LBALOW",
                                             "LBAMID", "LBAHIGH", "DEVICE",   "STATUS"};
        static const char* const writes[8] = {"DATA",   "FEATURES", "SECCOUNT", "LBALOW",
                                              "LBAMID", "LBAHIGH",  "DEVICE",   "COMMAND"};
        return write ? writes[addr & 7] : reads[addr & 7];
    }
    if (b == Block::Control) {
        if (addr == 6) return write ? "DEVCTRL" : "ALTSTATUS";
        if (addr == 7 && !write) return "DEVADDR";  // obsolete
    }
    return "-";
}

}  // namespace bus
