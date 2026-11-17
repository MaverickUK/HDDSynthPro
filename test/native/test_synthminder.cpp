// Tests for SynthMinder's command (8Dh in ata_device) and protocol
// (synthminder.h): the sectors through the device as SynthMinder's IDE.PAS
// moves them, and the drive's answers, relays and errors.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ata_device.h"
#include "check.h"
#include "identify.h"
#include "sentence.h"
#include "synthminder.h"

using namespace ata;

namespace {

const IdentifyInfo INFO{"HDDSYNTH PROTO", "HSP-000001", "0.35.0", 131072};

// A sector as SynthMinder builds one: header, then the sentence and a line feed.
std::vector<uint8_t> requestSector(const std::string& sentence, uint16_t sequence = 1) {
    std::vector<uint8_t> s(synthminder::SECTOR_BYTES, 0);
    memcpy(s.data(), "SMND", 4);
    s[4] = synthminder::PROTOCOL_VERSION;
    s[6] = uint8_t(sequence);
    s[7] = uint8_t(sequence >> 8);
    std::string text = sentence + "\n";
    s[8] = uint8_t(text.size());
    s[9] = uint8_t(text.size() >> 8);
    memcpy(s.data() + synthminder::HEADER_BYTES, text.data(), text.size());
    return s;
}

std::string sentence(const char* body) {
    char line[128];
    size_t n = formatSentence(line, sizeof line, body);
    return std::string(line, n - 2);
}

// A reply sector's text, its flags and its sequence.
struct ReplySector {
    std::string text;
    bool more = false;
    uint16_t sequence = 0;
    bool valid = false;
};
ReplySector readReply(const uint8_t* s) {
    ReplySector r;
    r.valid = memcmp(s, "SMND", 4) == 0 && s[4] == synthminder::PROTOCOL_VERSION;
    r.more = s[5] & synthminder::FLAG_MORE;
    r.sequence = uint16_t(s[6] | (s[7] << 8));
    size_t n = size_t(s[8] | (s[9] << 8));
    r.text.assign(reinterpret_cast<const char*>(s + synthminder::HEADER_BYTES), n);
    return r;
}

// ---- 8Dh in the device -------------------------------------------------------

struct Host {
    Device& dev;
    std::vector<Fetch> fetches = {};  // every fetch core 0 was asked for
    // Core 0: a SEND's request is copied to `request`; a RECEIVE is answered
    // with `reply` once `reply_ready`.
    std::vector<uint16_t> request = {}, reply = std::vector<uint16_t>(256, 0);
    bool reply_ready = true;

    uint8_t status() { return uint8_t(dev.read(Reg::StatusCommand)); }
    uint8_t altStatus() { return uint8_t(dev.read(Reg::AltStatusDevCtrl)); }
    void pump() {
        Fetch f;
        if (!dev.pendingFetch(f)) return;
        if (fetches.empty() || fetches.back().id != f.id) fetches.push_back(f);
        if (f.store) {
            request.assign(f.buffer, f.buffer + 256);
        } else {
            if (!reply_ready) return;  // held, as core 0 holds a relay's
            memcpy(f.buffer, reply.data(), 512);
        }
        dev.fetchDone(f.id, true);
    }
    bool waitNotBusy(int polls = 100) {
        for (int i = 0; i < polls; ++i) {
            if (!(altStatus() & ST_BSY)) return true;
            pump();
        }
        return false;
    }
    void command(uint8_t features, uint8_t count = 1, uint8_t low = 'S', uint8_t mid = 'M', uint8_t version = 1) {
        dev.write(Reg::Device, 0xA0);
        dev.write(Reg::ErrorFeatures, features);
        dev.write(Reg::SectorCount, count);
        dev.write(Reg::LbaLow, low);
        dev.write(Reg::LbaMid, mid);
        dev.write(Reg::LbaHigh, version);
        dev.write(Reg::StatusCommand, CMD_SYNTHMINDER);
    }
};

TEST(sm_send_is_a_one_sector_data_out) {
    Device dev(INFO);
    Host h{dev};
    h.command(SM_SEND);
    CHECK(h.waitNotBusy());
    CHECK(!dev.intrq());  // no interrupt before the data, as for WRITE SECTORS
    uint8_t st = h.status();
    CHECK((st & ST_DRQ) && !(st & ST_ERR));
    for (int i = 0; i < 256; ++i) dev.write(Reg::Data, uint16_t(0x4D53 + i));
    CHECK(h.waitNotBusy());
    st = h.status();
    CHECK(!(st & (ST_ERR | ST_DRQ)));
    CHECK_EQ(h.fetches.size(), 1u);
    CHECK(h.fetches[0].message && h.fetches[0].store && !h.fetches[0].flush);
    CHECK_EQ(int(h.fetches[0].sectors), 1);
    CHECK_EQ(h.request.size(), 256u);
    CHECK_EQ(h.request[0], 0x4D53);
    CHECK_EQ(h.request[255], 0x4D53 + 255);
}

TEST(sm_receive_is_busy_until_the_reply_is_ready) {
    Device dev(INFO);
    Host h{dev};
    for (int i = 0; i < 256; ++i) h.reply[size_t(i)] = uint16_t(i * 3);
    h.reply_ready = false;  // the synth hasn't answered yet
    h.command(SM_RECEIVE);
    CHECK(!h.waitNotBusy(50));  // BSY all the while: never IORDY, never DRQ early
    CHECK(h.altStatus() & ST_BSY);
    h.reply_ready = true;
    CHECK(h.waitNotBusy());
    uint8_t st = h.status();
    CHECK((st & ST_DRQ) && !(st & ST_ERR));
    std::vector<uint16_t> got;
    for (int i = 0; i < 256; ++i) got.push_back(dev.read(Reg::Data));
    CHECK(got == h.reply);
    CHECK(h.waitNotBusy());
    CHECK(!(h.altStatus() & (ST_DRQ | ST_ERR)));
    CHECK_EQ(h.fetches.size(), 1u);
    CHECK(h.fetches[0].message && !h.fetches[0].store);
}

TEST(sm_aborts_anything_not_synthminders) {
    Device dev(INFO);
    Host h{dev};
    struct Case {
        uint8_t features, count, low, mid, version;
    } cases[] = {
        {0x03, 1, 'S', 'M', 1},     // another subcommand
        {SM_SEND, 2, 'S', 'M', 1},  // two sectors
        {SM_SEND, 1, 'X', 'M', 1},  // no signature
        {SM_SEND, 1, 'S', 'X', 1},
        {SM_RECEIVE, 1, 'S', 'M', 2},  // another version
    };
    for (const Case& c : cases) {
        h.command(c.features, c.count, c.low, c.mid, c.version);
        CHECK(h.waitNotBusy());
        uint8_t st = h.status();
        CHECK((st & ST_ERR) && !(st & ST_DRQ));
        CHECK_EQ(int(dev.error()), int(ER_ABRT));
    }
    CHECK(h.fetches.empty());
}

TEST(sm_never_flushes_with_the_write_cache_off) {
    Device dev(INFO);
    Host h{dev};
    dev.write(Reg::ErrorFeatures, SF_DISABLE_WRITE_CACHE);
    dev.write(Reg::StatusCommand, CMD_SET_FEATURES);
    CHECK(h.waitNotBusy());
    h.command(SM_SEND);
    CHECK(h.waitNotBusy());
    for (int i = 0; i < 256; ++i) dev.write(Reg::Data, 0);
    CHECK(h.waitNotBusy());
    CHECK_EQ(h.fetches.size(), 1u);
    CHECK(!h.fetches[0].flush);
}

TEST(sm_a_reset_ends_a_waiting_receive) {
    Device dev(INFO);
    Host h{dev};
    h.reply_ready = false;
    h.command(SM_RECEIVE);
    CHECK(!h.waitNotBusy(10));
    Fetch f;
    CHECK(dev.pendingFetch(f));
    dev.hardReset(true);
    dev.hardReset(false);
    CHECK(!dev.pendingFetch(f));
    dev.fetchDone(f.id, true);  // core 0's late reply: stale, ignored
    CHECK(!(h.altStatus() & (ST_BSY | ST_DRQ)));
    // The next command is a plain one again.
    dev.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
    CHECK(h.waitNotBusy());
    CHECK(h.status() & ST_DRQ);
}

// ---- The protocol --------------------------------------------------------------

struct FakePlatform : synthminder::Platform {
    std::vector<std::string> relayed;
    std::vector<synthminder::BayQuery> asked;
    synthminder::BayRequest last;
    bool link = true;
    int files = 3;
    void answer(const synthminder::BayRequest& r, synthminder::Reply& reply) override {
        asked.push_back(r.query);
        last = r;
        switch (r.query) {
            case synthminder::BayQuery::Status:
                synthminder::addValue(reply, "firmware", "0.35.0");
                synthminder::addValue(reply, "device0", "ICL386MB.IMG");
                break;
            case synthminder::BayQuery::Files:
                for (int i = 0; i < files; ++i) {
                    char name[16];
                    std::snprintf(name, sizeof name, "DISK%02d.IMG", i);
                    synthminder::addFile(reply, unsigned(i), unsigned(files), name, 65536, 'H');
                }
                break;
            case synthminder::BayQuery::Disc: synthminder::addAck(reply, "DISC", ""); break;
            case synthminder::BayQuery::Set: synthminder::addAck(reply, "SET", "key"); break;
        }
    }
    bool relay(const char* line, size_t len) override {
        if (!link) return false;
        relayed.emplace_back(line, len);
        return true;
    }
};

// Sends `request` and collects the whole reply, RECEIVE by RECEIVE.
std::vector<ReplySector> exchange(synthminder::Service& s, const std::string& request, uint32_t now = 0,
                                  uint16_t sequence = 7) {
    auto sector = requestSector(request, sequence);
    s.onSend(sector.data(), now);
    std::vector<ReplySector> out;
    uint8_t buf[512];
    for (int i = 0; i < 20; ++i) {
        if (!s.onReceive(buf, now)) break;  // waiting for the synth
        out.push_back(readReply(buf));
        if (!out.back().more) break;
    }
    return out;
}

TEST(sm_bay_status) {
    FakePlatform p;
    synthminder::Service s(p);
    auto r = exchange(s, sentence("SBQ,1,STATUS"));
    CHECK_EQ(r.size(), 1u);
    CHECK(r[0].valid && !r[0].more);
    CHECK_EQ(r[0].sequence, 7);
    CHECK_STR(r[0].text, sentence("SBV,1,firmware,0.35.0") + "\n" + sentence("SBV,1,device0,ICL386MB.IMG") + "\n");
    CHECK(p.asked.size() == 1 && p.asked[0] == synthminder::BayQuery::Status);
    // Read once: the next RECEIVE has nothing waiting.
    uint8_t buf[512];
    CHECK(s.onReceive(buf, 0));
    CHECK_STR(readReply(buf).text, sentence("SBK,1,REQUEST,ERR,none") + "\n");
}

TEST(sm_bay_commands_and_errors) {
    FakePlatform p;
    synthminder::Service s(p);
    auto r = exchange(s, sentence("SBC,1,DISC,QUAKE.ISO"));
    CHECK_STR(r[0].text, sentence("SBK,1,DISC,OK,") + "\n");
    CHECK_STR(p.last.key, "QUAKE.ISO");
    r = exchange(s, sentence("SBC,1,SET,device0,DOSBENCH.IMG"));
    CHECK_STR(p.last.key, "device0");
    CHECK_STR(p.last.value, "DOSBENCH.IMG");
    CHECK_STR(r[0].text, sentence("SBK,1,SET,ERR,key") + "\n");
    r = exchange(s, sentence("SBQ,1,WHATEVER"));
    CHECK_STR(r[0].text, sentence("SBK,1,SBQ,ERR,unknown") + "\n");
    r = exchange(s, sentence("XYZ,1"));
    CHECK_STR(r[0].text, sentence("SBK,1,XYZ,ERR,unknown") + "\n");
    r = exchange(s, "$SBQ,1,STATUS*00");  // a bad checksum
    CHECK_STR(r[0].text, sentence("SBK,1,REQUEST,ERR,invalid") + "\n");
    auto bad = requestSector(sentence("SBQ,1,STATUS"));
    bad[0] = 'X';  // not SynthMinder's sector
    s.onSend(bad.data(), 0);
    uint8_t buf[512];
    CHECK(s.onReceive(buf, 0));
    CHECK_STR(readReply(buf).text, sentence("SBK,1,REQUEST,ERR,invalid") + "\n");
    CHECK_EQ(p.asked.size(), 2u);  // only DISC and SET reached the platform
}

TEST(sm_a_long_reply_spans_sectors_whole_sentences_each) {
    FakePlatform p;
    p.files = 20;  // about 640 bytes: two sectors
    synthminder::Service s(p);
    auto r = exchange(s, sentence("SBQ,1,FILES"));
    CHECK_EQ(r.size(), 2u);
    CHECK(r[0].more && !r[1].more);
    std::string all;
    for (auto& sec : r) {
        CHECK(sec.text.size() <= synthminder::TEXT_MAX);
        CHECK(!sec.text.empty() && sec.text.back() == '\n');  // no sentence split
        all += sec.text;
    }
    int lines = 0;
    for (char c : all) lines += c == '\n';
    CHECK_EQ(lines, 20);
}

TEST(sm_relays_and_collects_the_synths_answer) {
    FakePlatform p;
    synthminder::Service s(p);
    std::string q = sentence("HDQ,1,STATUS");
    auto r = exchange(s, q, 1000);
    CHECK(r.empty());  // waiting for the synth
    CHECK(s.relaying());
    CHECK(p.relayed.size() == 1 && p.relayed[0] == q);
    std::string model = sentence("HDM,2,-"), status = sentence("HDS,1,0.25.0,RUN,2,8,65,50,4100");
    s.onLinkLine(model.data(), model.size());  // the synth's $HDM every 2 s: not the answer
    CHECK(s.relaying());
    s.onLinkLine(status.data(), status.size());
    CHECK(!s.relaying());
    uint8_t buf[512];
    CHECK(s.onReceive(buf, 1100));
    CHECK_STR(readReply(buf).text, status + "\n");
}

TEST(sm_relays_a_pack_list_by_its_count) {
    FakePlatform p;
    synthminder::Service s(p);
    exchange(s, sentence("HDQ,1,PACKS"));
    for (int i = 0; i < 3; ++i) {
        CHECK(s.relaying());
        char body[80];
        std::snprintf(body, sizeof body, "HDP,1,%d,3,PACK%d,Pack %d,R", i, i, i);
        std::string l = sentence(body);
        s.onLinkLine(l.data(), l.size());
    }
    CHECK(!s.relaying());
    uint8_t buf[512];
    CHECK(s.onReceive(buf, 0));
    ReplySector r = readReply(buf);
    int lines = 0;
    for (char c : r.text) lines += c == '\n';
    CHECK_EQ(lines, 3);
}

TEST(sm_relay_errors) {
    FakePlatform p;
    synthminder::Service s(p);
    // The synth doesn't answer in 500 ms.
    exchange(s, sentence("HDC,1,VOL,55"), 1000);
    uint8_t buf[512];
    CHECK(!s.onReceive(buf, 1499));
    CHECK(s.onReceive(buf, 1500));
    CHECK_STR(readReply(buf).text, sentence("SBK,1,RELAY,ERR,nosynth") + "\n");
    CHECK_EQ(s.timeouts(), 1u);
    std::string late = sentence("HDK,1,VOL,OK,");
    s.onLinkLine(late.data(), late.size());  // too late: ignored
    CHECK(s.onReceive(buf, 1600));
    CHECK_STR(readReply(buf).text, sentence("SBK,1,REQUEST,ERR,none") + "\n");
    // The link is off.
    p.link = false;
    auto r = exchange(s, sentence("HDQ,1,STATUS"));
    CHECK_STR(r[0].text, sentence("SBK,1,RELAY,ERR,nosynth") + "\n");
    // A sentence the synth doesn't know.
    p.link = true;
    r = exchange(s, sentence("HDX,1"));
    CHECK_STR(r[0].text, sentence("SBK,1,HDX,ERR,unknown") + "\n");
    CHECK_EQ(p.relayed.size(), 1u);  // only the first went to the synth
}

TEST(sm_values_are_sanitised) {
    synthminder::Reply reply;
    reply.clear();
    synthminder::addValue(reply, "disc", "A,B*C$D");
    uint8_t buf[512];
    reply.sector(0, 0, buf);
    CHECK_STR(readReply(buf).text, sentence("SBV,1,disc,A_B_C_D") + "\n");
}

TEST(sm_reply_never_splits_or_overflows) {
    synthminder::Reply reply;
    reply.clear();
    std::string line(95, 'x');  // five fit a sector (480 of 496 bytes)
    int added = 0;
    while (reply.add(line.data(), line.size())) ++added;
    CHECK(reply.sectors() <= synthminder::MAX_SECTORS);
    CHECK(size_t(added) * 96 <= synthminder::REPLY_TEXT_MAX);
    CHECK_EQ(added, int(synthminder::REPLY_TEXT_MAX / 96));
    uint8_t buf[512];
    for (size_t i = 0; i < reply.sectors(); ++i) {
        reply.sector(i, 0, buf);
        ReplySector r = readReply(buf);
        CHECK_EQ(r.text.size() % 96, 0u);
        CHECK_EQ(r.more, i + 1 < reply.sectors());
    }
}

}  // namespace

void runSynthMinderTests() {
    RUN(sm_send_is_a_one_sector_data_out);
    RUN(sm_receive_is_busy_until_the_reply_is_ready);
    RUN(sm_aborts_anything_not_synthminders);
    RUN(sm_never_flushes_with_the_write_cache_off);
    RUN(sm_a_reset_ends_a_waiting_receive);
    RUN(sm_bay_status);
    RUN(sm_bay_commands_and_errors);
    RUN(sm_a_long_reply_spans_sectors_whole_sentences_each);
    RUN(sm_relays_and_collects_the_synths_answer);
    RUN(sm_relays_a_pack_list_by_its_count);
    RUN(sm_relay_errors);
    RUN(sm_values_are_sanitised);
    RUN(sm_reply_never_splits_or_overflows);
}
