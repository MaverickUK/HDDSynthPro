// The CD-ROM drive's packet commands (SCSI multimedia commands, MMC), for
// atapi::Device: a 12-byte command packet in, what to do about it out. Pure
// C++, host-tested; no bus or register knowledge, so each command is tested
// on its own (docs/atapi.md).
//
// A data disc only: one Mode 1 track of 2,048-byte frames, starting at frame
// 0. No audio, no raw sectors (READ CD), no writing.
//
// Runs on core 1 (atapi::Device runs a packet as its next job, like an ATA
// command), so it builds replies with plain loops into the caller's buffer:
// no allocation, no library calls.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mmc {

constexpr unsigned PACKET_BYTES = 12;
constexpr uint32_t FRAME_BYTES = 2048;

// Opcodes handled. Anything else is ILLEGAL REQUEST, "invalid command operation code".
constexpr uint8_t OP_TEST_UNIT_READY = 0x00;
constexpr uint8_t OP_REQUEST_SENSE = 0x03;
constexpr uint8_t OP_INQUIRY = 0x12;
constexpr uint8_t OP_MODE_SENSE_6 = 0x1A;
constexpr uint8_t OP_START_STOP_UNIT = 0x1B;
constexpr uint8_t OP_PREVENT_ALLOW = 0x1E;
constexpr uint8_t OP_READ_CAPACITY = 0x25;
constexpr uint8_t OP_READ_10 = 0x28;
constexpr uint8_t OP_SEEK_10 = 0x2B;
constexpr uint8_t OP_VERIFY_10 = 0x2F;
constexpr uint8_t OP_READ_TOC = 0x43;
constexpr uint8_t OP_GET_CONFIGURATION = 0x46;
constexpr uint8_t OP_GET_EVENT_STATUS = 0x4A;
constexpr uint8_t OP_MODE_SENSE_10 = 0x5A;
constexpr uint8_t OP_READ_12 = 0xA8;
constexpr uint8_t OP_SET_CD_SPEED = 0xBB;

// Sense data: what went wrong, for CHECK CONDITION and REQUEST SENSE.
struct Sense {
    uint8_t key;   // the top four bits of the Error register on CHECK CONDITION
    uint8_t asc;   // additional sense code
    uint8_t ascq;  // ...and its qualifier
};

constexpr uint8_t KEY_NO_SENSE = 0x0;
constexpr uint8_t KEY_NOT_READY = 0x2;
constexpr uint8_t KEY_MEDIUM_ERROR = 0x3;
constexpr uint8_t KEY_ILLEGAL_REQUEST = 0x5;
constexpr uint8_t KEY_UNIT_ATTENTION = 0x6;

constexpr Sense SENSE_NONE{KEY_NO_SENSE, 0x00, 0x00};
constexpr Sense SENSE_NO_MEDIUM{KEY_NOT_READY, 0x3A, 0x00};       // medium not present
constexpr Sense SENSE_READ_ERROR{KEY_MEDIUM_ERROR, 0x11, 0x00};   // unrecovered read error
constexpr Sense SENSE_BAD_OPCODE{KEY_ILLEGAL_REQUEST, 0x20, 0x00};  // invalid command operation code
constexpr Sense SENSE_BAD_LBA{KEY_ILLEGAL_REQUEST, 0x21, 0x00};     // logical block address out of range
constexpr Sense SENSE_BAD_FIELD{KEY_ILLEGAL_REQUEST, 0x24, 0x00};   // invalid field in the packet
constexpr Sense SENSE_NO_SAVING{KEY_ILLEGAL_REQUEST, 0x39, 0x00};   // saving parameters not supported
constexpr Sense SENSE_POWER_ON{KEY_UNIT_ATTENTION, 0x29, 0x00};     // power on, reset or bus device reset
constexpr Sense SENSE_MEDIUM_CHANGED{KEY_UNIT_ATTENTION, 0x28, 0x00};  // not ready to ready: medium may have changed

// GET EVENT STATUS NOTIFICATION's media events.
constexpr uint8_t MEDIA_NO_CHANGE = 0x00;
constexpr uint8_t MEDIA_NEW = 0x02;
constexpr uint8_t MEDIA_REMOVED = 0x03;

// The disc in the drive: `frames` 2,048-byte frames, or 0 for no disc.
struct Disc {
    uint32_t frames;
};

// What the drive remembers between packets.
struct State {
    Sense sense = SENSE_NONE;        // the last command's, for REQUEST SENSE
    bool unit_attention = false;     // the next command (not INQUIRY or REQUEST SENSE) reports `attention`
    Sense attention = SENSE_POWER_ON;
    uint8_t media_event = MEDIA_NO_CHANGE;  // GET EVENT STATUS NOTIFICATION reports it once
};

// The drive after power-on or a hardware reset: a unit attention to report,
// and a new-media event if there's a disc.
void powerOn(State& state, const Disc& disc);
// A disc put in or taken out (`disc` as it is now): a disc put in raises a
// unit attention (medium may have changed) and a new-media event; one taken
// out a removal event, and commands then find no medium, as on a real drive.
void discChanged(State& state, const Disc& disc);

// What the device does with a packet.
struct Reply {
    enum class Kind : uint8_t {
        Good,   // done, no data
        Data,   // `bytes` bytes of reply in the caller's buffer, then done
        Read,   // `frames` frames from frame `lba`, then done
        Error,  // CHECK CONDITION with `state.sense`
    };
    Kind kind;
    uint16_t bytes;  // Data
    uint32_t lba;    // Read
    uint32_t frames;
};

// The largest reply execute() builds; `buffer` must hold at least this.
constexpr size_t MAX_REPLY = 96;

// Runs one packet against `disc`. Data goes into `buffer` (`size` bytes,
// at least MAX_REPLY), cut to the packet's allocation length. Updates
// `state`: the sense data, and any unit attention or media event reported.
Reply execute(const uint8_t packet[PACKET_BYTES], const Disc& disc, State& state, uint8_t* buffer, size_t size);

// The frames a read covers, from its packet (READ(10), READ(12)): for the trace.
uint32_t readLength(const uint8_t packet[PACKET_BYTES]);

// A short name for the trace ("?" if not handled).
const char* name(uint8_t opcode);

// Frame `lba` as minutes, seconds and frames (MSF), counting the 2 s pregap
// as READ TOC does: frame 0 is 00:02:00.
void toMsf(uint32_t lba, uint8_t& m, uint8_t& s, uint8_t& f);

}  // namespace mmc
