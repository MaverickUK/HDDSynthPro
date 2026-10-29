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

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool parseLine(const char* line, size_t len, Fields& out) {
    static const char PREFIX[] = "$HDA,";
    const size_t prefix_len = sizeof PREFIX - 1;
    if (len > MAX_LINE || len < prefix_len) return false;
    for (size_t i = 0; i < prefix_len; ++i)
        if (line[i] != PREFIX[i]) return false;

    // The checksum: two hex digits after the last '*'.
    size_t star = len;
    while (star > 0 && line[star - 1] != '*') --star;
    if (star == 0 || len - star != 2) return false;
    star -= 1;  // the '*' itself
    int hi = hexValue(line[star + 1]), lo = hexValue(line[star + 2]);
    if (hi < 0 || lo < 0) return false;
    uint8_t checksum = 0;
    for (size_t i = 1; i < star; ++i) {
        if (uint8_t(line[i]) >= 0x80) return false;  // ASCII only
        checksum ^= uint8_t(line[i]);
    }
    if (checksum != uint8_t(hi << 4 | lo)) return false;

    // The fields between "$HDA," and '*': version, busy, class, op, then any
    // that later versions add.
    const char* field[4] = {};
    size_t field_len[4] = {};
    size_t count = 0;
    size_t start = prefix_len;
    for (size_t i = prefix_len; i <= star && count < 4; ++i) {
        if (i == star || line[i] == ',') {
            field[count] = line + start;
            field_len[count] = i - start;
            ++count;
            start = i + 1;
        }
    }
    if (count < 4) return false;
    if (field_len[0] == 0) return false;
    for (size_t i = 0; i < field_len[0]; ++i)
        if (field[0][i] < '0' || field[0][i] > '9') return false;
    if (field_len[1] != 1 || field_len[2] != 1 || field_len[3] != 1) return false;

    Fields f;
    switch (field[1][0]) {
        case '0': f.busy = false; break;
        case '1': f.busy = true; break;
        default: return false;
    }
    switch (field[2][0]) {
        case 'I': f.state = activity::State::Idle; break;
        case 'S': f.state = activity::State::Sequential; break;
        case 'R': f.state = activity::State::Random; break;
        default: return false;
    }
    switch (field[3][0]) {
        case '-': f.op = Op::None; break;
        case 'R': f.op = Op::Read; break;
        case 'W': f.op = Op::Write; break;
        default: return false;
    }
    out = f;
    return true;
}

bool Receiver::feed(char c, Fields& out) {
    if (c != '\n') {
        if (len_ < sizeof line_) {
            line_[len_++] = c;
        } else {
            overlong_ = true;
        }
        return false;
    }
    size_t len = len_;
    bool overlong = overlong_;
    len_ = 0;
    overlong_ = false;
    while (len > 0 && line_[len - 1] == '\r') --len;
    if (len == 0 && !overlong) return false;  // an empty line: nothing said
    if (!overlong && parseLine(line_, len, out)) {
        ++valid_;
        return true;
    }
    ++rejected_;
    return false;
}

}  // namespace activity_link
