#include "activity_link.h"

#include <cstdio>

namespace activity_link {

Op opOf(uint8_t command) {
    switch (command) {
        // The same reads and writes as activity::isMediaCommand.
        case 0x20: case 0x21: case 0xC4: case 0xC8: case 0xC9:
            return Op::Read;   // READ SECTORS, MULTIPLE, DMA (and no-retry forms)
        case 0x30: case 0x31: case 0xC5: case 0xCA: case 0xCB:
            return Op::Write;  // WRITE SECTORS, MULTIPLE, DMA (and no-retry forms)
        default:
            return Op::None;
    }
}

static char classChar(activity::State s) {
    switch (s) {
        case activity::State::Sequential: return 'S';
        case activity::State::Random: return 'R';
        default: return 'I';
    }
}

static char opChar(Op op) {
    switch (op) {
        case Op::Read: return 'R';
        case Op::Write: return 'W';
        default: return '-';
    }
}

size_t formatSentence(const Fields& f, char* buf, size_t size) {
    char body[16];
    int n = std::snprintf(body, sizeof body, "HDA,1,%d,%c,%c", f.busy ? 1 : 0, classChar(f.state), opChar(f.op));
    if (n <= 0 || size_t(n) >= sizeof body) return 0;
    uint8_t checksum = 0;
    for (int i = 0; i < n; ++i) checksum ^= uint8_t(body[i]);
    int len = std::snprintf(buf, size, "$%s*%02X\r\n", body, unsigned(checksum));
    return (len > 0 && size_t(len) < size) ? size_t(len) : 0;
}

}  // namespace activity_link
