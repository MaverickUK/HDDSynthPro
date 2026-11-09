#include "mmc.h"

#include "core1_ram.h"
#include "version.h"

namespace mmc {

namespace {

// Big-endian fields, as SCSI packs them.
CORE1_RAM_CD uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
CORE1_RAM_CD uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// A reply being built: bytes past `size` are counted but not stored, so a
// reply cut by the allocation length still reports its full length.
struct Out {
    uint8_t* p;
    size_t size;
    size_t n = 0;
    CORE1_RAM_CD void u8(uint8_t v) {
        if (n < size) p[n] = v;
        ++n;
    }
    CORE1_RAM_CD void u16(uint16_t v) {
        u8(uint8_t(v >> 8));
        u8(uint8_t(v));
    }
    CORE1_RAM_CD void u32(uint32_t v) {
        u16(uint16_t(v >> 16));
        u16(uint16_t(v));
    }
    CORE1_RAM_CD void zeros(unsigned count) {
        for (unsigned i = 0; i < count; ++i) u8(0);
    }
    // A space-padded ASCII field, `width` bytes.
    CORE1_RAM_CD void text(const char* s, unsigned width) {
        for (unsigned i = 0; i < width; ++i) u8(*s ? uint8_t(*s++) : uint8_t(' '));
    }
    // Fills in a length field written earlier as a placeholder.
    CORE1_RAM_CD void set16(size_t at, uint16_t v) {
        if (at + 1 < size) {
            p[at] = uint8_t(v >> 8);
            p[at + 1] = uint8_t(v);
        }
    }
    CORE1_RAM_CD void set32(size_t at, uint32_t v) {
        set16(at, uint16_t(v >> 16));
        set16(at + 2, uint16_t(v));
    }
};

CORE1_RAM_CD Reply good(State& st) {
    st.sense = SENSE_NONE;
    return Reply{Reply::Kind::Good, 0, 0, 0};
}

CORE1_RAM_CD Reply error(State& st, const Sense& s) {
    st.sense = s;
    return Reply{Reply::Kind::Error, 0, 0, 0};
}

// `out`'s reply, cut to the host's allocation length.
CORE1_RAM_CD Reply data(State& st, const Out& out, uint32_t allocation) {
    st.sense = SENSE_NONE;
    uint32_t n = out.n < out.size ? uint32_t(out.n) : uint32_t(out.size);
    if (n > allocation) n = allocation;
    return Reply{Reply::Kind::Data, uint16_t(n), 0, 0};
}

// A TOC address: LBA, or MSF (00h, M, S, F) when the host asks for it.
CORE1_RAM_CD void address(Out& out, uint32_t lba, bool msf) {
    if (!msf) {
        out.u32(lba);
        return;
    }
    uint8_t m, s, f;
    toMsf(lba, m, s, f);
    out.u8(0);
    out.u8(m);
    out.u8(s);
    out.u8(f);
}

constexpr uint8_t DATA_TRACK = 0x14;  // ADR 1 (position), control 4 (a data track)
constexpr uint8_t LEAD_OUT = 0xAA;

// One entry of the full TOC (READ TOC format 2): `point` at P-MSF.
CORE1_RAM_CD void tocPoint(Out& out, uint8_t point, uint8_t pm, uint8_t ps, uint8_t pf) {
    out.u8(1);  // session
    out.u8(DATA_TRACK);
    out.u8(0);  // TNO: the lead-in
    out.u8(point);
    out.zeros(4);  // running time in the lead-in, and a zero byte
    out.u8(pm);
    out.u8(ps);
    out.u8(pf);
}

// A GET CONFIGURATION feature's header, if the host asked for it (rt 0: all
// from `start`, 1: the current ones from `start`, 2: only `start`): its code,
// persistent and current bits, and its payload's length. True if written, so
// the caller writes the payload.
CORE1_RAM_CD bool feature(Out& out, uint8_t rt, uint16_t start, uint16_t code, bool persistent, bool current,
                       uint8_t length) {
    bool wanted = rt == 2 ? code == start : code >= start && (rt == 0 || current);
    if (!wanted) return false;
    out.u16(code);
    out.u8(uint8_t((persistent ? 0x02 : 0) | (current ? 0x01 : 0)));  // version 0
    out.u8(length);
    return true;
}

CORE1_RAM_CD Reply readToc(const uint8_t* c, const Disc& disc, State& st, Out& out) {
    if (!disc.frames) return error(st, SENSE_NO_MEDIUM);
    bool msf = (c[1] & 0x02) != 0;
    uint8_t format = c[2] & 0x0F;
    if (format == 0) format = uint8_t(c[9] >> 6);  // older hosts (SFF-8020) put it in the control byte
    uint8_t track = c[6];
    out.u16(0);  // length, below
    out.u8(1);   // first track, or session
    out.u8(1);   // last
    switch (format) {
        case 0:  // the TOC: track 1, then the lead-out
            if (track > 1 && track != LEAD_OUT) return error(st, SENSE_BAD_FIELD);
            if (track <= 1) {
                out.u8(0);
                out.u8(DATA_TRACK);
                out.u8(1);
                out.u8(0);
                address(out, 0, msf);
            }
            out.u8(0);
            out.u8(DATA_TRACK);
            out.u8(LEAD_OUT);
            out.u8(0);
            address(out, disc.frames, msf);
            break;
        case 1:  // session information: one session, its first track at frame 0
            out.u8(0);
            out.u8(DATA_TRACK);
            out.u8(1);
            out.u8(0);
            address(out, 0, msf);
            break;
        case 2: {  // the full TOC, as the lead-in holds it: points A0h, A1h, A2h and track 1
            uint8_t m, s, f;
            toMsf(disc.frames, m, s, f);
            tocPoint(out, 0xA0, 1, 0x00, 0);  // first track 1, disc type 00h (CD-DA or CD-ROM)
            tocPoint(out, 0xA1, 1, 0, 0);     // last track 1
            tocPoint(out, 0xA2, m, s, f);     // the lead-out's start
            tocPoint(out, 0x01, 0, 2, 0);     // track 1 at 00:02:00
            break;
        }
        default:
            return error(st, SENSE_BAD_FIELD);
    }
    out.set16(0, uint16_t(out.n - 2));
    return data(st, out, be16(c + 7));
}

// GET CONFIGURATION's features: the CD-ROM profile, and what a read-only
// drive with a tray implies.
CORE1_RAM_CD Reply getConfiguration(const uint8_t* c, const Disc& disc, State& st, Out& out) {
    uint8_t rt = c[1] & 0x03;  // 0: all features, 1: the current ones, 2: just one
    uint16_t start = be16(c + 2);
    if (rt == 3) return error(st, SENSE_BAD_FIELD);
    bool present = disc.frames != 0;
    out.u32(0);  // length, below
    out.u16(0);
    out.u16(present ? 0x0008 : 0x0000);  // current profile: CD-ROM, or none without a disc
    if (feature(out, rt, start, 0x0000, true, true, 4)) {  // profile list: just CD-ROM
        out.u16(0x0008);
        out.u8(present ? 0x01 : 0x00);
        out.u8(0);
    }
    if (feature(out, rt, start, 0x0001, true, true, 4)) out.u32(0x00000002);  // core: the ATAPI interface
    if (feature(out, rt, start, 0x0003, true, true, 4)) {                     // removable medium
        out.u8(0x29);  // a tray, eject, lock
        out.zeros(3);
    }
    if (feature(out, rt, start, 0x0010, false, present, 8)) {  // random readable
        out.u32(FRAME_BYTES);
        out.u16(1);  // blocking
        out.zeros(2);
    }
    if (feature(out, rt, start, 0x001E, false, present, 4)) out.zeros(4);  // CD read
    out.set32(0, uint32_t(out.n - 4));
    return data(st, out, be16(c + 7));
}

CORE1_RAM_CD Reply getEventStatus(const uint8_t* c, const Disc& disc, State& st, Out& out) {
    if (!(c[1] & 0x01)) return error(st, SENSE_BAD_FIELD);  // polled only: no asynchronous events
    constexpr uint8_t MEDIA_CLASS = 0x10;
    if (c[4] & MEDIA_CLASS) {
        out.u16(6);
        out.u8(0x04);  // a media event
        out.u8(MEDIA_CLASS);
        out.u8(st.media_event);  // new media, removal, or no change
        st.media_event = MEDIA_NO_CHANGE;
        out.u8(disc.frames ? 0x02 : 0x00);   // media present; the tray closed
        out.zeros(2);
    } else {
        out.u16(2);
        out.u8(0x80);  // no event available
        out.u8(MEDIA_CLASS);
    }
    return data(st, out, be16(c + 7));
}

CORE1_RAM_CD Reply modeSense(const uint8_t* c, const Disc& disc, State& st, Out& out, bool six) {
    uint8_t pc = uint8_t(c[2] >> 6);  // 0 current, 1 changeable, 2 default, 3 saved
    uint8_t page = c[2] & 0x3F;
    if (pc == 3) return error(st, SENSE_NO_SAVING);
    if (page != 0x01 && page != 0x2A && page != 0x3F) return error(st, SENSE_BAD_FIELD);
    uint8_t medium = disc.frames ? 0x01 : 0x70;  // a 120 mm data CD, or no disc with the tray closed
    if (six) {
        out.u8(0);  // length, below
        out.u8(medium);
        out.zeros(2);  // device-specific; no block descriptors
    } else {
        out.u16(0);
        out.u8(medium);
        out.zeros(5);
    }
    bool changeable = pc == 1;  // nothing is
    if (page == 0x01 || page == 0x3F) {  // read error recovery
        out.u8(0x01);
        out.u8(6);
        out.u8(0);
        out.u8(changeable ? 0 : 5);  // read retry count
        out.zeros(4);
    }
    if (page == 0x2A || page == 0x3F) {  // CD capabilities and mechanical status
        constexpr uint16_t SPEED_KBS = 1411;  // about 8x, what the SD card gives
        out.u8(0x2A);
        out.u8(20);
        out.zeros(4);  // reads CD-ROM only, writes nothing, no audio
        out.u8(changeable ? 0 : 0x29);  // a tray, eject, lock
        out.u8(0);
        out.u16(changeable ? 0 : SPEED_KBS);  // maximum read speed
        out.zeros(4);                         // volume levels, buffer size
        out.u16(changeable ? 0 : SPEED_KBS);  // current read speed
        out.zeros(6);
    }
    if (six)
        out.p[0] = uint8_t(out.n - 1);
    else
        out.set16(0, uint16_t(out.n - 2));
    return data(st, out, six ? c[4] : be16(c + 7));
}

CORE1_RAM_CD Reply inquiry(const uint8_t* c, State& st, Out& out) {
    if ((c[1] & 0x01) || c[2]) return error(st, SENSE_BAD_FIELD);  // no vital product data pages
    out.u8(0x05);  // a CD-ROM device
    out.u8(0x80);  // removable
    out.u8(0x00);
    out.u8(0x32);  // ATAPI version 3, response format 2
    out.u8(31);    // the rest's length
    out.zeros(3);
    out.text("HDDSYNTH", 8);
    out.text("PRO CD-ROM", 16);
    out.text(FIRMWARE_VERSION, 4);
    return data(st, out, be16(c + 3));
}

// The frames from `lba` exist, or the packet fails: no disc, or out of range.
CORE1_RAM_CD bool inRange(const Disc& disc, State& st, uint32_t lba, uint32_t frames, Reply& failed) {
    if (!disc.frames) {
        failed = error(st, SENSE_NO_MEDIUM);
        return false;
    }
    if (lba >= disc.frames || frames > disc.frames - lba) {
        failed = error(st, SENSE_BAD_LBA);
        return false;
    }
    return true;
}

}  // namespace

CORE1_RAM_CD void powerOn(State& state, const Disc& disc) {
    state.sense = SENSE_NONE;
    state.unit_attention = true;
    state.attention = SENSE_POWER_ON;
    state.media_event = disc.frames ? MEDIA_NEW : MEDIA_NO_CHANGE;
}

CORE1_RAM_CD void discChanged(State& state, const Disc& disc) {
    if (disc.frames) {
        state.unit_attention = true;
        state.attention = SENSE_MEDIUM_CHANGED;
        state.media_event = MEDIA_NEW;
    } else {
        state.unit_attention = false;  // nothing to read: NOT READY says so
        state.media_event = MEDIA_REMOVED;
    }
}

CORE1_RAM_CD void toMsf(uint32_t lba, uint8_t& m, uint8_t& s, uint8_t& f) {
    uint32_t x = lba + 150;  // the 2 s pregap, at 75 frames a second
    f = uint8_t(x % 75);
    s = uint8_t(x / 75 % 60);
    m = uint8_t(x / (75 * 60));
}

CORE1_RAM_CD uint32_t readLength(const uint8_t* c) {
    if (c[0] == OP_READ_10) return be16(c + 7);
    if (c[0] == OP_READ_12) return be32(c + 6);
    return 0;
}

CORE1_RAM_CD Reply execute(const uint8_t* c, const Disc& disc, State& st, uint8_t* buffer, size_t size) {
    Out out{buffer, size};
    uint8_t op = c[0];
    // MMC: these four report no unit attention, so a host can always ask
    // what the drive is and what happened.
    bool attention_exempt = op == OP_INQUIRY || op == OP_REQUEST_SENSE || op == OP_GET_CONFIGURATION ||
                            op == OP_GET_EVENT_STATUS;
    if (st.unit_attention && !attention_exempt) {
        st.unit_attention = false;
        return error(st, st.attention);
    }
    Reply failed{};
    switch (op) {
        case OP_TEST_UNIT_READY:
            return disc.frames ? good(st) : error(st, SENSE_NO_MEDIUM);
        case OP_REQUEST_SENSE: {
            Sense s = st.unit_attention ? st.attention : st.sense;
            st.unit_attention = false;
            out.u8(0x70);  // current error, fixed format
            out.u8(0);
            out.u8(s.key);
            out.zeros(4);  // information
            out.u8(10);    // the rest's length
            out.zeros(4);  // command-specific information
            out.u8(s.asc);
            out.u8(s.ascq);
            out.zeros(4);
            return data(st, out, c[4]);  // and the sense is cleared
        }
        case OP_INQUIRY:
            return inquiry(c, st, out);
        case OP_START_STOP_UNIT:   // eject and load do nothing yet
        case OP_PREVENT_ALLOW:     // nor does locking the tray
        case OP_SET_CD_SPEED:      // the card sets the speed
            return good(st);
        case OP_READ_CAPACITY:
            if (!disc.frames) return error(st, SENSE_NO_MEDIUM);
            out.u32(disc.frames - 1);  // the last frame's address
            out.u32(FRAME_BYTES);
            return data(st, out, 8);
        case OP_READ_10:
        case OP_READ_12: {
            uint32_t lba = be32(c + 2);
            uint32_t frames = readLength(c);
            if (frames == 0) return disc.frames ? good(st) : error(st, SENSE_NO_MEDIUM);
            if (!inRange(disc, st, lba, frames, failed)) return failed;
            st.sense = SENSE_NONE;
            return Reply{Reply::Kind::Read, 0, lba, frames};
        }
        case OP_SEEK_10:
            return inRange(disc, st, be32(c + 2), 1, failed) ? good(st) : failed;
        case OP_VERIFY_10: {  // the image can't hold a bad frame, so a range check is the job
            uint32_t frames = be16(c + 7);
            if (frames == 0) return disc.frames ? good(st) : error(st, SENSE_NO_MEDIUM);
            return inRange(disc, st, be32(c + 2), frames, failed) ? good(st) : failed;
        }
        case OP_READ_TOC:
            return readToc(c, disc, st, out);
        case OP_GET_CONFIGURATION:
            return getConfiguration(c, disc, st, out);
        case OP_GET_EVENT_STATUS:
            return getEventStatus(c, disc, st, out);
        case OP_MODE_SENSE_6:
            return modeSense(c, disc, st, out, true);
        case OP_MODE_SENSE_10:
            return modeSense(c, disc, st, out, false);
        default:
            return error(st, SENSE_BAD_OPCODE);
    }
}

const char* name(uint8_t opcode) {
    switch (opcode) {
        case OP_TEST_UNIT_READY: return "TEST_UNIT_READY";
        case OP_REQUEST_SENSE: return "REQUEST_SENSE";
        case OP_INQUIRY: return "INQUIRY";
        case OP_MODE_SENSE_6: return "MODE_SENSE_6";
        case OP_START_STOP_UNIT: return "START_STOP";
        case OP_PREVENT_ALLOW: return "PREVENT_ALLOW";
        case OP_READ_CAPACITY: return "READ_CAPACITY";
        case OP_READ_10: return "READ_10";
        case OP_SEEK_10: return "SEEK_10";
        case OP_VERIFY_10: return "VERIFY_10";
        case OP_READ_TOC: return "READ_TOC";
        case OP_GET_CONFIGURATION: return "GET_CONFIG";
        case OP_GET_EVENT_STATUS: return "GET_EVENT";
        case OP_MODE_SENSE_10: return "MODE_SENSE_10";
        case OP_READ_12: return "READ_12";
        case OP_SET_CD_SPEED: return "SET_CD_SPEED";
        default: return "?";
    }
}

}  // namespace mmc
