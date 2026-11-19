// SynthMinder's protocol: a DOS program on the PC talks to the drive with ATA
// command 8Dh (docs/synthminder-protocol.md). Each request is one sentence in
// the HDD Synth's format (sentence.h): $SB... sentences the drive answers
// itself, $HD... sentences it relays to the HDD Synth over the Synth Link and
// whose answers it collects.
//
// Core 1 only moves the sectors (ata_device: 8Dh as a one-sector write or
// read, a Fetch with `message` set); everything here runs on core 0. Pure
// C++, host-tested: what needs the card, the settings or the UART is behind
// Platform.
#pragma once

#include <cstddef>
#include <cstdint>

namespace synthminder {

constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr size_t SECTOR_BYTES = 512;
constexpr size_t HEADER_BYTES = 16;
constexpr size_t TEXT_MAX = SECTOR_BYTES - HEADER_BYTES;  // 496
constexpr size_t MAX_SECTORS = 16;                        // a reply's, at most
constexpr uint8_t FLAG_MORE = 0x01;                       // the reply continues in the next RECEIVE
// The synth has this long to answer a relayed request (the drive answers
// SynthMinder within a second whatever happens).
// 1.5 s: the synth takes about 900 ms to list its packs (it reads each
// pack's pack.json and looks for its drive.json on its card), and only then
// sends the lines; SynthMinder waits 2 s for a reply (IDE.PAS).
constexpr uint32_t RELAY_TIMEOUT_MS = 1500;
// A reply's text, all sectors together: RAM is short (the drive builds use
// 97%). A pack list's line is up to 88 bytes, so about 34 packs fit; past
// that the list is cut short (SynthMinder shows what came).
constexpr size_t REPLY_TEXT_MAX = 3072;
// A request's sentence: SynthMinder's longest is a file name's $SBC.
constexpr size_t REQUEST_LINE_MAX = 128;
// The longest sentence the drive relays to the synth: one that fits the link
// UART's 32-byte TX FIFO, so sending it never waits.
constexpr size_t RELAY_LINE_MAX = 30;
// A value or a file name in a $SB... sentence.
constexpr size_t VALUE_MAX = 64;

// A request sector's first sentence, without its line ending.
struct Request {
    uint16_t sequence = 0;
    char line[REQUEST_LINE_MAX + 1] = "";
    size_t length = 0;
};

enum class SectorError : uint8_t { None, Signature, Version, Length, NoLine };
SectorError parseRequest(const uint8_t* sector, Request& out);

// A reply, as sentences: each is kept whole within one sector.
class Reply {
public:
    void clear();
    // One sentence, without its line ending. False if it doesn't fit (then
    // nothing of it is added).
    bool add(const char* line, size_t len);
    // "$" + body + "*hh". False if it doesn't fit.
    bool addSentence(const char* body);
    size_t lines() const { return lines_; }
    size_t sectors() const { return sectors_ ? sectors_ : 1; }  // an empty reply is one empty sector
    // Sector `i` of the reply, header and all, into `out` (SECTOR_BYTES).
    void sector(size_t i, uint16_t sequence, uint8_t* out) const;

private:
    char text_[REPLY_TEXT_MAX];
    size_t len_ = 0;
    uint16_t start_[MAX_SECTORS + 1] = {};  // where each sector's text starts; start_[sectors_] is the end
    size_t sectors_ = 0;
    size_t lines_ = 0;
};

// The characters a value may not hold (',', '*', '$', control characters)
// become '_', as the synth does in pack names. Always NUL-terminates `out`.
void sanitise(const char* in, char* out, size_t out_len);

// $SBV,1,<key>,<value>; $SBF,1,<index>,<count>,<name>,<sizeKB>,<kind>;
// $SBK,1,<what>,OK, (reason "") or $SBK,1,<what>,ERR,<reason>.
bool addValue(Reply& reply, const char* key, const char* value);
bool addFile(Reply& reply, unsigned index, unsigned count, const char* name, uint32_t size_kb, char kind);
bool addAck(Reply& reply, const char* what, const char* reason);
// $SBS,1,<key>,<next>,<now>: one of the settings SynthMinder can change, as
// HDDSYNTH.INI has it (from the next start) and as the drive runs now.
bool addSetting(Reply& reply, const char* key, const char* next, const char* now);

// The settings SET may change (protocol 1: device0 first, the rest since
// IDE Synth Bay 0.36.0), and what their values must be. checkSetting says
// whether `value` can be right for `key` without looking at the card: Bad
// with the reason ("key": not one of these; "value": not a value it takes),
// or what the drive must still find on the card.
enum class SettingNeeds : uint8_t {
    Bad,           // why() says which
    Nothing,       // on/off, a seek timing: nothing to find
    Image,         // device0: an .IMG on the card
    Disc,          // cdrom: an .ISO or .CUE on the card
    ImageOrDisc,   // device1: an .IMG, an .ISO or .CUE (a CD-ROM with that disc), or none/cdrom (Nothing)
};
struct SettingCheck {
    SettingNeeds needs = SettingNeeds::Bad;
    const char* why = "key";
};
SettingCheck checkSetting(const char* key, const char* value);
// The keys of SETTINGS' answer, in the order it gives them.
extern const char* const SETTING_KEYS[];
extern const size_t SETTING_COUNT;

// A request for the drive itself.
enum class BayQuery : uint8_t { Status, Files, Disc, Set, Settings };
struct BayRequest {
    BayQuery query = BayQuery::Status;
    char key[VALUE_MAX + 1] = "";    // Disc: the disc's name ("-" ejects); Set: the setting
    char value[VALUE_MAX + 1] = "";  // Set: its value
};

// What the drive needs from the firmware around it.
class Platform {
public:
    virtual ~Platform() = default;
    // Answer a $SB... request into `reply` (at once: within the second).
    virtual void answer(const BayRequest& request, Reply& reply) = 0;
    // Send `line` (a whole sentence, without its line ending) to the synth.
    // False if the link is off.
    virtual bool relay(const char* line, size_t len) = 0;
};

// The requests in flight, one at a time, as SynthMinder sends them.
class Service {
public:
    explicit Service(Platform& platform) : platform_(platform) {}

    // SEND: a request sector arrived. Answers it at once, or starts a relay.
    void onSend(const uint8_t* sector, uint32_t now_ms);
    // RECEIVE: fills `sector` with the reply's next sector and returns true,
    // or returns false while a relayed request is still waiting for the synth
    // (call again: core 0 holds the RECEIVE, BSY, until then).
    bool onReceive(uint8_t* sector, uint32_t now_ms);
    // A sentence from the synth (without its line ending): kept if it's part
    // of the answer being collected.
    void onLinkLine(const char* line, size_t len);
    // A relay waiting for the synth.
    bool relaying() const { return relaying_; }

    // For `s`: requests, relays, and relays the synth didn't answer in time.
    uint32_t requests() const { return requests_; }
    uint32_t relays() const { return relays_; }
    uint32_t timeouts() const { return timeouts_; }

private:
    enum class Expect : uint8_t { One, Packs };
    void startRelay(const char* line, size_t len, uint32_t now_ms);
    void finish();  // the reply is ready: RECEIVE takes it from sector 0

    Platform& platform_;
    Reply reply_;
    uint16_t sequence_ = 0;
    bool ready_ = false;      // a reply is waiting for RECEIVE
    size_t next_sector_ = 0;  // the next RECEIVE's
    bool relaying_ = false;
    char expect_name_[4] = "";  // the answer's sentence name: HDS, HDP, HDK or HDM
    Expect expect_ = Expect::One;
    uint32_t expect_count_ = 0;  // Packs: from the first $HDP
    uint32_t relay_start_ms_ = 0;
    char relay_what_[8] = "";  // for the error reply
    uint32_t requests_ = 0, relays_ = 0, timeouts_ = 0;
};

}  // namespace synthminder
