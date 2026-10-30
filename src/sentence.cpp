#include "sentence.h"

#include <stdio.h>
#include <string.h>

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

} // namespace

uint8_t sentenceChecksum(const char* body, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) sum ^= static_cast<uint8_t>(body[i]);
    return sum;
}

bool sentenceBody(const char* line, size_t len, const char*& body, size_t& bodyLen) {
    if (len < 4 || line[0] != '$') return false;
    const char* star = static_cast<const char*>(memchr(line, '*', len));
    if (!star || line + len - star != 3) return false;  // exactly two digits after '*'
    const int hi = hexDigit(star[1]), lo = hexDigit(star[2]);
    if (hi < 0 || lo < 0) return false;
    body = line + 1;
    bodyLen = static_cast<size_t>(star - body);
    return sentenceChecksum(body, bodyLen) == (hi << 4 | lo);
}

bool Field::is(const char* s) const { return strlen(s) == n && memcmp(p, s, n) == 0; }

bool Field::toUint(uint32_t& out) const {
    if (n == 0 || n > 9) return false;  // 9 digits always fit
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + static_cast<uint32_t>(p[i] - '0');
    }
    out = v;
    return true;
}

size_t splitFields(const char* body, size_t len, Field* out, size_t max) {
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i < len && body[i] != ',') continue;
        if (count < max) {
            out[count].p = body + start;
            out[count].n = i - start;
        }
        ++count;
        start = i + 1;
    }
    return count;
}

size_t formatSentence(char* buf, size_t bufLen, const char* body) {
    const size_t n = strlen(body);
    if (bufLen < n + 7) return 0;  // '$' + body + "*hh\r\n" + NUL
    buf[0] = '$';
    memcpy(buf + 1, body, n);
    snprintf(buf + 1 + n, bufLen - 1 - n, "*%02X\r\n", sentenceChecksum(body, n));
    return n + 6;
}

LineAssembler::Result LineAssembler::feed(char c) {
    if (c == '$') {  // a new sentence, whatever came before
        const bool cutOff = inLine_;
        inLine_ = true;
        tooLong_ = false;
        buf_[0] = c;
        len_ = 1;
        return cutOff ? Result::Dropped : Result::None;
    }
    if (!inLine_) return Result::None;  // noise between sentences
    if (c == '\n') {
        inLine_ = false;
        if (len_ && buf_[len_ - 1] == '\r') --len_;
        buf_[len_] = 0;
        return tooLong_ || len_ > SENTENCE_MAX_LINE ? Result::Dropped : Result::Line;
    }
    if (len_ < sizeof(buf_) - 1) buf_[len_++] = c;  // keep room for the NUL
    else tooLong_ = true;
    return Result::None;
}
