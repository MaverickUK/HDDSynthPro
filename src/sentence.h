// NMEA-style sentences, shared by the HDDSynthPro link (link_parser.h) and
// the USB companion app protocol (host_protocol.h):
//
//   $<NAME>,<field>,<field>...*<checksum>\r\n
//
// The checksum is two hex digits: the XOR of every byte between '$' and '*'.
// Platform independent (unit tested natively).
#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t SENTENCE_MAX_LINE = 80;  // longest line accepted, without its line ending

uint8_t sentenceChecksum(const char* body, size_t len);

// Check "$BODY*hh" (without the line ending): a '$', exactly two hex digits
// (either case) after the '*', and a matching checksum. On success `body`
// and `bodyLen` give BODY.
bool sentenceBody(const char* line, size_t len, const char*& body, size_t& bodyLen);

// One field of a body: not NUL-terminated.
struct Field {
    const char* p = nullptr;
    size_t n = 0;
    bool is(const char* s) const;      // exactly equal to `s`
    bool toUint(uint32_t& out) const;  // one or more decimal digits, nothing else
};

// Split a body at ','. Returns how many fields there are (fields past `max`
// are counted but not stored).
size_t splitFields(const char* body, size_t len, Field* out, size_t max);

// Write "$" + body + "*hh\r\n" into `buf` (NUL-terminated). Returns the
// length without the NUL, or 0 if it doesn't fit.
size_t formatSentence(char* buf, size_t bufLen, const char* body);

// Assembles '$' lines from a byte stream. A '$' always starts a new line (as
// in NMEA), so noise before it is dropped; a line ends at '\n' (an optional
// '\r' before it is stripped).
class LineAssembler {
public:
    enum class Result : uint8_t {
        None,     // nothing finished yet
        Line,     // line()/length() hold a complete line, starting with '$'
        Dropped,  // a line was cut off by a '$', or was longer than SENTENCE_MAX_LINE
    };

    Result feed(char c);
    bool inLine() const { return inLine_; }  // between a '$' and its '\n'
    const char* line() const { return buf_; }
    size_t length() const { return len_; }

private:
    char buf_[SENTENCE_MAX_LINE + 2];  // room for a '\r' and a NUL
    size_t len_ = 0;
    bool inLine_ = false;
    bool tooLong_ = false;
};
