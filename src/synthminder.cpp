#include "synthminder.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "seek_timing.h"

#include "sentence.h"

namespace synthminder {

static uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

SectorError parseRequest(const uint8_t* sector, Request& out) {
    if (memcmp(sector, "SMND", 4) != 0) return SectorError::Signature;
    if (sector[4] != PROTOCOL_VERSION) return SectorError::Version;
    out.sequence = le16(sector + 6);
    size_t length = le16(sector + 8);
    if (length > TEXT_MAX) return SectorError::Length;
    // The first sentence: up to its line feed (or the end of the text), less a CR.
    const char* text = reinterpret_cast<const char*>(sector + HEADER_BYTES);
    const char* lf = static_cast<const char*>(memchr(text, '\n', length));
    size_t n = lf ? size_t(lf - text) : length;
    if (n && text[n - 1] == '\r') --n;
    if (n == 0) return SectorError::NoLine;
    if (n > REQUEST_LINE_MAX) return SectorError::Length;
    memcpy(out.line, text, n);
    out.line[n] = '\0';
    out.length = n;
    return SectorError::None;
}

// ---- Reply ------------------------------------------------------------------

void Reply::clear() {
    len_ = 0;
    sectors_ = 0;
    lines_ = 0;
    start_[0] = 0;
}

bool Reply::add(const char* line, size_t len) {
    const size_t need = len + 1;  // and its line feed
    if (need > TEXT_MAX || len_ + need > REPLY_TEXT_MAX) return false;
    if (sectors_ == 0) {
        start_[0] = 0;
        sectors_ = 1;
    } else if (len_ - start_[sectors_ - 1] + need > TEXT_MAX) {  // a sentence is never split: the next sector
        if (sectors_ == MAX_SECTORS) return false;
        ++sectors_;  // start_[sectors_ - 1] is already len_: the old end is the new start
    }
    memcpy(text_ + len_, line, len);
    len_ += len;
    text_[len_++] = '\n';
    start_[sectors_] = uint16_t(len_);
    ++lines_;
    return true;
}

bool Reply::addSentence(const char* body) {
    char line[SENTENCE_MAX_LINE + 8];
    size_t n = formatSentence(line, sizeof line, body);
    if (n < 2) return false;
    return add(line, n - 2);  // without formatSentence's "\r\n"
}

void Reply::sector(size_t i, uint16_t sequence, uint8_t* out) const {
    memset(out, 0, SECTOR_BYTES);
    memcpy(out, "SMND", 4);
    out[4] = PROTOCOL_VERSION;
    out[5] = i + 1 < sectors() ? FLAG_MORE : 0;
    out[6] = uint8_t(sequence);
    out[7] = uint8_t(sequence >> 8);
    if (i >= sectors_) return;  // empty
    size_t n = size_t(start_[i + 1] - start_[i]);
    out[8] = uint8_t(n);
    out[9] = uint8_t(n >> 8);
    memcpy(out + HEADER_BYTES, text_ + start_[i], n);
}

// ---- $SB... sentences ---------------------------------------------------------

void sanitise(const char* in, char* out, size_t out_len) {
    if (!out_len) return;
    size_t i = 0;
    for (; in[i] && i + 1 < out_len; ++i) {
        char c = in[i];
        out[i] = (c == ',' || c == '*' || c == '$' || (unsigned char)c < 0x20 || (unsigned char)c > 0x7E) ? '_' : c;
    }
    out[i] = '\0';
}

bool addValue(Reply& reply, const char* key, const char* value) {
    char k[VALUE_MAX + 1], v[VALUE_MAX + 1], body[SENTENCE_MAX_LINE];
    sanitise(key, k, sizeof k);
    sanitise(value, v, sizeof v);
    if (std::snprintf(body, sizeof body, "SBV,1,%s,%s", k, v) >= int(sizeof body)) return false;
    return reply.addSentence(body);
}

bool addSetting(Reply& reply, const char* key, const char* next, const char* now) {
    char k[VALUE_MAX + 1], n[VALUE_MAX + 1], v[VALUE_MAX + 1], body[SENTENCE_MAX_LINE];
    sanitise(key, k, sizeof k);
    sanitise(next, n, sizeof n);
    sanitise(now, v, sizeof v);
    if (std::snprintf(body, sizeof body, "SBS,1,%s,%s,%s", k, n, v) >= int(sizeof body)) return false;
    return reply.addSentence(body);
}

const char* const SETTING_KEYS[] = {"device1", "cdrom", "seek_timing", "activity_link",
                                    "write_behind", "read_ahead", "sd_high_speed"};
const size_t SETTING_COUNT = sizeof(SETTING_KEYS) / sizeof(SETTING_KEYS[0]);

namespace {
bool sameNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}
bool endsWithNoCase(const char* s, const char* end) {
    size_t n = std::strlen(s), m = std::strlen(end);
    return n > m && sameNoCase(s + n - m, end);
}
SettingCheck ok(SettingNeeds needs) {
    SettingCheck c;
    c.needs = needs;
    c.why = "";
    return c;
}
SettingCheck bad(const char* why) {
    SettingCheck c;
    c.why = why;
    return c;
}
}  // namespace

SettingCheck checkSetting(const char* key, const char* value) {
    if (!value[0]) return bad("value");
    if (sameNoCase(key, "device0")) return endsWithNoCase(value, ".IMG") ? ok(SettingNeeds::Image) : bad("value");
    if (sameNoCase(key, "device1")) {
        if (sameNoCase(value, "none") || sameNoCase(value, "cdrom")) return ok(SettingNeeds::Nothing);
        return endsWithNoCase(value, ".IMG") || endsWithNoCase(value, ".ISO") || endsWithNoCase(value, ".CUE")
                   ? ok(SettingNeeds::ImageOrDisc)
                   : bad("value");
    }
    if (sameNoCase(key, "cdrom"))
        return endsWithNoCase(value, ".ISO") || endsWithNoCase(value, ".CUE") ? ok(SettingNeeds::Disc) : bad("value");
    if (sameNoCase(key, "seek_timing")) {
        seek_timing::Setting s;
        return seek_timing::parseSetting(value, std::strlen(value), s) ? ok(SettingNeeds::Nothing) : bad("value");
    }
    if (sameNoCase(key, "activity_link") || sameNoCase(key, "write_behind") || sameNoCase(key, "read_ahead") ||
        sameNoCase(key, "sd_high_speed"))
        return sameNoCase(value, "on") || sameNoCase(value, "off") ? ok(SettingNeeds::Nothing) : bad("value");
    return bad("key");
}

bool addFile(Reply& reply, unsigned index, unsigned count, const char* name, uint32_t size_kb, char kind) {
    char n[VALUE_MAX + 1], body[SENTENCE_MAX_LINE];
    sanitise(name, n, sizeof n);
    if (std::snprintf(body, sizeof body, "SBF,1,%u,%u,%s,%lu,%c", index, count, n, (unsigned long)size_kb, kind) >=
        int(sizeof body))
        return false;
    return reply.addSentence(body);
}

bool addAck(Reply& reply, const char* what, const char* reason) {
    char w[VALUE_MAX + 1], r[VALUE_MAX + 1], body[SENTENCE_MAX_LINE];
    sanitise(what, w, sizeof w);
    sanitise(reason, r, sizeof r);
    if (std::snprintf(body, sizeof body, "SBK,1,%s,%s,%s", w, r[0] ? "ERR" : "OK", r) >= int(sizeof body)) return false;
    return reply.addSentence(body);
}

// ---- The service --------------------------------------------------------------

static void copyField(const Field& f, char* out, size_t out_len) {
    size_t n = f.n < out_len - 1 ? f.n : out_len - 1;
    memcpy(out, f.p, n);
    out[n] = '\0';
}

void Service::finish() {
    relaying_ = false;
    ready_ = true;
    next_sector_ = 0;
}

void Service::onSend(const uint8_t* sector, uint32_t now_ms) {
    ++requests_;
    relaying_ = false;
    ready_ = false;
    reply_.clear();
    Request request;
    SectorError error = parseRequest(sector, request);
    sequence_ = error == SectorError::Signature ? 0 : le16(sector + 6);
    const char* body = nullptr;
    size_t body_len = 0;
    if (error != SectorError::None || !sentenceBody(request.line, request.length, body, body_len)) {
        addAck(reply_, "REQUEST", "invalid");
        finish();
        return;
    }
    Field f[5];
    size_t fields = splitFields(body, body_len, f, 5);
    char name[8];
    copyField(f[0], name, sizeof name);

    if (f[0].n == 3 && f[0].p[0] == 'H' && f[0].p[1] == 'D') {  // for the synth
        startRelay(request.line, request.length, now_ms);
        return;
    }

    BayRequest bay;
    bool known = fields >= 3 && f[1].is("1");
    if (known && f[0].is("SBQ") && f[2].is("STATUS")) {
        bay.query = BayQuery::Status;
    } else if (known && f[0].is("SBQ") && f[2].is("FILES")) {
        bay.query = BayQuery::Files;
    } else if (known && f[0].is("SBQ") && f[2].is("SETTINGS")) {
        bay.query = BayQuery::Settings;
    } else if (known && f[0].is("SBC") && f[2].is("DISC") && fields == 4 && f[3].n) {
        bay.query = BayQuery::Disc;
        copyField(f[3], bay.key, sizeof bay.key);
    } else if (known && f[0].is("SBC") && f[2].is("SET") && fields == 5 && f[3].n && f[4].n) {
        bay.query = BayQuery::Set;
        copyField(f[3], bay.key, sizeof bay.key);
        copyField(f[4], bay.value, sizeof bay.value);
    } else {
        addAck(reply_, name, "unknown");
        finish();
        return;
    }
    platform_.answer(bay, reply_);
    finish();
}

void Service::startRelay(const char* line, size_t len, uint32_t now_ms) {
    const char* body;
    size_t body_len;
    sentenceBody(line, len, body, body_len);  // checked by onSend
    Field f[3];
    size_t fields = splitFields(body, body_len, f, 3);
    copyField(f[0], relay_what_, sizeof relay_what_);
    // What the synth answers, and so when the answer is complete
    // (HDDSynth_Native's host_protocol.h).
    expect_ = Expect::One;
    expect_count_ = 0;
    if (f[0].is("HDQ") && fields >= 3 && f[2].is("STATUS")) {
        std::strcpy(expect_name_, "HDS");
    } else if (f[0].is("HDQ") && fields >= 3 && f[2].is("PACKS")) {
        std::strcpy(expect_name_, "HDP");
        expect_ = Expect::Packs;
    } else if (f[0].is("HDQ") && fields >= 3 && f[2].is("MODEL")) {
        std::strcpy(expect_name_, "HDM");
    } else if (f[0].is("HDC")) {
        std::strcpy(expect_name_, "HDK");
    } else {
        addAck(reply_, relay_what_, "unknown");
        finish();
        return;
    }
    // Longer than the link's TX FIFO: never one of SynthMinder's.
    if (len > RELAY_LINE_MAX) {
        addAck(reply_, "REQUEST", "invalid");
        finish();
        return;
    }
    if (!platform_.relay(line, len)) {
        addAck(reply_, "RELAY", "nosynth");
        finish();
        return;
    }
    ++relays_;
    relaying_ = true;
    relay_start_ms_ = now_ms;
}

void Service::onLinkLine(const char* line, size_t len) {
    if (!relaying_) return;
    const char* body;
    size_t body_len;
    if (!sentenceBody(line, len, body, body_len)) return;
    Field f[4];
    size_t fields = splitFields(body, body_len, f, 4);
    if (!f[0].is(expect_name_)) return;  // the synth's $HDM every 2 s, while it answers something else
    if (expect_ == Expect::Packs) {
        // $HDP,1,<index>,<count>,...: the first one says how many follow.
        uint32_t count = 0;
        if (fields < 4 || !f[3].toUint(count)) return;
        if (reply_.lines() == 0) expect_count_ = count;
    }
    if (!reply_.add(line, len)) {  // too long for the reply: what fits goes
        finish();
        return;
    }
    if (expect_ == Expect::One || reply_.lines() >= expect_count_) finish();
}

bool Service::onReceive(uint8_t* sector, uint32_t now_ms) {
    if (relaying_) {
        if (now_ms - relay_start_ms_ < RELAY_TIMEOUT_MS) return false;
        ++timeouts_;
        reply_.clear();
        addAck(reply_, "RELAY", "nosynth");
        finish();
    }
    if (!ready_) {  // nothing asked, or the reply was all read (so reply_ is free)
        reply_.clear();
        addAck(reply_, "REQUEST", "none");
        reply_.sector(0, sequence_, sector);
        reply_.clear();
        return true;
    }
    reply_.sector(next_sector_, sequence_, sector);
    if (++next_sector_ >= reply_.sectors()) ready_ = false;
    return true;
}

}  // namespace synthminder
