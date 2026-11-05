// Tests for the CD-ROM drive's packet commands (mmc), each on its own.
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"
#include "mmc.h"

using namespace mmc;

namespace {

const Disc DISC{1000};  // 1,000 frames (about 2 MB)
const Disc NO_DISC{0};

struct Run {
    Reply reply;
    std::vector<uint8_t> data;
};

// Runs one packet; the packet's bytes after the given ones are zero.
Run run(std::initializer_list<uint8_t> bytes, const Disc& disc, State& st) {
    uint8_t packet[PACKET_BYTES] = {};
    unsigned i = 0;
    for (uint8_t b : bytes) packet[i++] = b;
    uint8_t buf[512];
    std::memset(buf, 0xEE, sizeof buf);
    Run r;
    r.reply = execute(packet, disc, st, buf, sizeof buf);
    if (r.reply.kind == Reply::Kind::Data) r.data.assign(buf, buf + r.reply.bytes);
    return r;
}

State ready() {  // power-on, with the unit attention already reported
    State st;
    powerOn(st, DISC);
    st.unit_attention = false;
    st.media_event = false;
    return st;
}

uint32_t be32(const std::vector<uint8_t>& d, size_t at) {
    return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
}

bool sameSense(const Sense& a, const Sense& b) { return a.key == b.key && a.asc == b.asc && a.ascq == b.ascq; }

}  // namespace

TEST(mmc_unit_attention_once_after_power_on) {
    State st;
    powerOn(st, DISC);
    // INQUIRY and REQUEST SENSE don't report it...
    CHECK(run({OP_INQUIRY, 0, 0, 0, 36}, DISC, st).reply.kind == Reply::Kind::Data);
    CHECK(st.unit_attention);
    // ...the next other command does, once.
    Run r = run({OP_TEST_UNIT_READY}, DISC, st);
    CHECK(r.reply.kind == Reply::Kind::Error);
    CHECK(sameSense(st.sense, SENSE_POWER_ON));
    CHECK(run({OP_TEST_UNIT_READY}, DISC, st).reply.kind == Reply::Kind::Good);
}

TEST(mmc_request_sense_reports_and_clears) {
    State st = ready();
    run({0xFF}, DISC, st);  // an unknown opcode
    Run r = run({OP_REQUEST_SENSE, 0, 0, 0, 18}, DISC, st);
    CHECK(r.reply.kind == Reply::Kind::Data);
    CHECK_EQ(r.data.size(), size_t(18));
    CHECK_EQ(r.data[0], 0x70);
    CHECK_EQ(r.data[2], KEY_ILLEGAL_REQUEST);
    CHECK_EQ(r.data[7], 10);
    CHECK_EQ(r.data[12], 0x20);  // invalid command operation code
    r = run({OP_REQUEST_SENSE, 0, 0, 0, 18}, DISC, st);
    CHECK_EQ(r.data[2], KEY_NO_SENSE);
    // Cut to the allocation length
    CHECK_EQ(run({OP_REQUEST_SENSE, 0, 0, 0, 8}, DISC, st).data.size(), size_t(8));
}

TEST(mmc_request_sense_reports_a_pending_unit_attention) {
    State st;
    powerOn(st, DISC);
    Run r = run({OP_REQUEST_SENSE, 0, 0, 0, 18}, DISC, st);
    CHECK_EQ(r.data[2], KEY_UNIT_ATTENTION);
    CHECK_EQ(r.data[12], 0x29);
    CHECK(!st.unit_attention);
    CHECK(run({OP_TEST_UNIT_READY}, DISC, st).reply.kind == Reply::Kind::Good);
}

TEST(mmc_inquiry) {
    State st = ready();
    Run r = run({OP_INQUIRY, 0, 0, 0, 96}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(36));
    CHECK_EQ(r.data[0], 0x05);  // CD-ROM
    CHECK_EQ(r.data[1], 0x80);  // removable
    CHECK_EQ(r.data[4], 31);
    CHECK_STR(std::string(r.data.begin() + 8, r.data.begin() + 16), "HDDSYNTH");
    CHECK_STR(std::string(r.data.begin() + 16, r.data.begin() + 32), "PRO CD-ROM      ");
    CHECK_EQ(run({OP_INQUIRY, 0, 0, 0, 5}, DISC, st).data.size(), size_t(5));
    // No vital product data
    CHECK(run({OP_INQUIRY, 1, 0x80, 0, 96}, DISC, st).reply.kind == Reply::Kind::Error);
    CHECK(sameSense(st.sense, SENSE_BAD_FIELD));
}

TEST(mmc_test_unit_ready_without_a_disc) {
    State st;
    powerOn(st, NO_DISC);
    st.unit_attention = false;
    CHECK(run({OP_TEST_UNIT_READY}, NO_DISC, st).reply.kind == Reply::Kind::Error);
    CHECK(sameSense(st.sense, SENSE_NO_MEDIUM));
    // INQUIRY still works
    CHECK(run({OP_INQUIRY, 0, 0, 0, 36}, NO_DISC, st).reply.kind == Reply::Kind::Data);
    CHECK(run({OP_READ_CAPACITY}, NO_DISC, st).reply.kind == Reply::Kind::Error);
    CHECK(run({OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1}, NO_DISC, st).reply.kind == Reply::Kind::Error);
    CHECK(sameSense(st.sense, SENSE_NO_MEDIUM));
}

TEST(mmc_read_capacity) {
    State st = ready();
    Run r = run({OP_READ_CAPACITY}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(8));
    CHECK_EQ(be32(r.data, 0), 999u);
    CHECK_EQ(be32(r.data, 4), 2048u);
}

TEST(mmc_reads) {
    State st = ready();
    Run r = run({OP_READ_10, 0, 0, 0, 0x01, 0x02, 0, 0x00, 0x10}, DISC, st);  // 16 frames from 258
    CHECK(r.reply.kind == Reply::Kind::Read);
    CHECK_EQ(r.reply.lba, 258u);
    CHECK_EQ(r.reply.frames, 16u);
    r = run({OP_READ_12, 0, 0, 0, 0, 10, 0, 0, 0, 3}, DISC, st);
    CHECK(r.reply.kind == Reply::Kind::Read);
    CHECK_EQ(r.reply.lba, 10u);
    CHECK_EQ(r.reply.frames, 3u);
    // The last frame, then one past it
    CHECK(run({OP_READ_10, 0, 0, 0, 0x03, 0xE7, 0, 0, 1}, DISC, st).reply.kind == Reply::Kind::Read);
    CHECK(run({OP_READ_10, 0, 0, 0, 0x03, 0xE7, 0, 0, 2}, DISC, st).reply.kind == Reply::Kind::Error);
    CHECK(sameSense(st.sense, SENSE_BAD_LBA));
    CHECK(run({OP_READ_10, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 1}, DISC, st).reply.kind == Reply::Kind::Error);
    // Nothing to read: done
    CHECK(run({OP_READ_10, 0, 0, 0, 0, 5, 0, 0, 0}, DISC, st).reply.kind == Reply::Kind::Good);
}

TEST(mmc_seek_and_verify) {
    State st = ready();
    CHECK(run({OP_SEEK_10, 0, 0, 0, 0, 5}, DISC, st).reply.kind == Reply::Kind::Good);
    CHECK(run({OP_SEEK_10, 0, 0, 0, 0x03, 0xE8}, DISC, st).reply.kind == Reply::Kind::Error);
    CHECK(run({OP_VERIFY_10, 0, 0, 0, 0, 5, 0, 0, 10}, DISC, st).reply.kind == Reply::Kind::Good);
    CHECK(run({OP_VERIFY_10, 0, 0, 0, 0x03, 0xE0, 0, 0, 10}, DISC, st).reply.kind == Reply::Kind::Error);
}

TEST(mmc_msf) {
    uint8_t m, s, f;
    toMsf(0, m, s, f);
    CHECK(m == 0 && s == 2 && f == 0);
    toMsf(1000, m, s, f);  // 1150 frames: 15 s and 25 frames
    CHECK(m == 0 && s == 15 && f == 25);
    toMsf(330000, m, s, f);  // a full 74-minute disc's lead-out area
    CHECK_EQ(m, 73);
    CHECK_EQ(s, 22);
    CHECK_EQ(f, 0);
}

TEST(mmc_read_toc_format_0) {
    State st = ready();
    Run r = run({OP_READ_TOC, 0, 0, 0, 0, 0, 0, 0x03, 0x24}, DISC, st);  // LBA, from track 0, 804 bytes
    CHECK_EQ(r.data.size(), size_t(20));
    CHECK_EQ(r.data[1], 18);  // length after the field
    CHECK_EQ(r.data[2], 1);   // first track
    CHECK_EQ(r.data[3], 1);   // last track
    CHECK_EQ(r.data[5], 0x14);
    CHECK_EQ(r.data[6], 1);
    CHECK_EQ(be32(r.data, 8), 0u);
    CHECK_EQ(r.data[14], 0xAA);
    CHECK_EQ(be32(r.data, 16), 1000u);  // the lead-out starts after the last frame
    // MSF
    r = run({OP_READ_TOC, 0x02, 0, 0, 0, 0, 0, 0x03, 0x24}, DISC, st);
    CHECK_EQ(be32(r.data, 8), 0x00000200u);    // 00:02:00
    CHECK_EQ(be32(r.data, 16), 0x00000F19u);  // 00:15:25
    // From the lead-out only; a track that isn't there
    r = run({OP_READ_TOC, 0, 0, 0, 0, 0, 0xAA, 0x03, 0x24}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(12));
    CHECK(run({OP_READ_TOC, 0, 0, 0, 0, 0, 2, 0x03, 0x24}, DISC, st).reply.kind == Reply::Kind::Error);
    // Cut to the allocation length: the header alone says how long it is
    r = run({OP_READ_TOC, 0, 0, 0, 0, 0, 0, 0, 4}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(4));
    CHECK_EQ(r.data[1], 18);
}

TEST(mmc_read_toc_sessions_and_full) {
    State st = ready();
    Run r = run({OP_READ_TOC, 0, 1, 0, 0, 0, 0, 0, 12}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(12));
    CHECK_EQ(r.data[1], 10);
    CHECK_EQ(r.data[6], 1);
    // The format in the control byte (SFF-8020)
    r = run({OP_READ_TOC, 0, 0, 0, 0, 0, 0, 0, 12, 0x40}, DISC, st);
    CHECK_EQ(r.data[1], 10);
    // The full TOC: A0h, A1h, A2h, track 1
    r = run({OP_READ_TOC, 0x02, 2, 0, 0, 0, 0, 0x01, 0x00}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(48));
    CHECK_EQ(r.data[1], 46);
    CHECK_EQ(r.data[4 + 3], 0xA0);
    CHECK_EQ(r.data[4 + 8], 1);
    CHECK_EQ(r.data[15 + 3], 0xA1);
    CHECK_EQ(r.data[26 + 3], 0xA2);
    CHECK(r.data[26 + 8] == 0 && r.data[26 + 9] == 15 && r.data[26 + 10] == 25);
    CHECK_EQ(r.data[37 + 3], 0x01);
    CHECK(run({OP_READ_TOC, 0, 5, 0, 0, 0, 0, 0x01, 0x00}, DISC, st).reply.kind == Reply::Kind::Error);
    CHECK(run({OP_READ_TOC, 0, 0, 0, 0, 0, 0, 0x01, 0x00}, NO_DISC, st).reply.kind == Reply::Kind::Error);
}

TEST(mmc_get_configuration) {
    State st = ready();
    Run r = run({OP_GET_CONFIGURATION, 0, 0, 0, 0, 0, 0, 0x01, 0x00}, DISC, st);
    CHECK(r.reply.kind == Reply::Kind::Data);
    CHECK_EQ(be32(r.data, 0), uint32_t(r.data.size() - 4));
    CHECK_EQ(r.data[6] << 8 | r.data[7], 0x0008);  // current profile: CD-ROM
    CHECK_EQ(r.data[8] << 8 | r.data[9], 0x0000);  // the profile list first
    CHECK_EQ(r.data[12] << 8 | r.data[13], 0x0008);
    CHECK_EQ(r.data[14], 1);  // current
    size_t all = r.data.size();
    // Just one feature
    r = run({OP_GET_CONFIGURATION, 2, 0x00, 0x1E, 0, 0, 0, 0x01, 0x00}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(8 + 8));
    CHECK_EQ(r.data[8] << 8 | r.data[9], 0x001E);
    // No disc: no current profile, and the disc's features aren't current
    r = run({OP_GET_CONFIGURATION, 1, 0, 0, 0, 0, 0, 0x01, 0x00}, NO_DISC, st);
    CHECK_EQ(r.data[6] << 8 | r.data[7], 0x0000);
    CHECK(r.data.size() < all);
    // The header only
    CHECK_EQ(run({OP_GET_CONFIGURATION, 0, 0, 0, 0, 0, 0, 0, 8}, DISC, st).data.size(), size_t(8));
}

TEST(mmc_get_event_status) {
    State st;
    powerOn(st, DISC);
    st.unit_attention = false;
    // A new disc once, then no change
    Run r = run({OP_GET_EVENT_STATUS, 1, 0, 0, 0x10, 0, 0, 0, 8}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(8));
    CHECK_EQ(r.data[2], 0x04);
    CHECK_EQ(r.data[4], 0x02);  // new media
    CHECK_EQ(r.data[5], 0x02);  // present
    r = run({OP_GET_EVENT_STATUS, 1, 0, 0, 0x10, 0, 0, 0, 8}, DISC, st);
    CHECK_EQ(r.data[4], 0x00);
    // Classes not supported: no event
    r = run({OP_GET_EVENT_STATUS, 1, 0, 0, 0x02, 0, 0, 0, 8}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(4));
    CHECK_EQ(r.data[2], 0x80);
    // Asynchronous: refused
    CHECK(run({OP_GET_EVENT_STATUS, 0, 0, 0, 0x10, 0, 0, 0, 8}, DISC, st).reply.kind == Reply::Kind::Error);
    // No unit attention for it
    State fresh;
    powerOn(fresh, DISC);
    CHECK(run({OP_GET_EVENT_STATUS, 1, 0, 0, 0x10, 0, 0, 0, 8}, DISC, fresh).reply.kind == Reply::Kind::Data);
    CHECK(fresh.unit_attention);
}

TEST(mmc_disc_changes) {
    // Taken out: a removal event, then NOT READY; no unit attention
    State st = ready();
    Disc none{0};
    discChanged(st, none);
    Run r = run({OP_GET_EVENT_STATUS, 1, 0, 0, 0x10, 0, 0, 0, 8}, none, st);
    CHECK_EQ(r.data[4], MEDIA_REMOVED);
    CHECK_EQ(r.data[5], 0x00);  // no medium
    r = run({OP_TEST_UNIT_READY}, none, st);
    CHECK(r.reply.kind == Reply::Kind::Error);
    CHECK_EQ(st.sense.key, KEY_NOT_READY);
    CHECK_EQ(st.sense.asc, 0x3A);
    // Another put in: a new-media event, and the next command a unit
    // attention, medium may have changed (28h), once
    Disc other{2000};
    discChanged(st, other);
    r = run({OP_GET_EVENT_STATUS, 1, 0, 0, 0x10, 0, 0, 0, 8}, other, st);  // exempt: no attention
    CHECK_EQ(r.data[4], MEDIA_NEW);
    CHECK_EQ(r.data[5], 0x02);
    r = run({OP_READ_CAPACITY}, other, st);
    CHECK(r.reply.kind == Reply::Kind::Error);
    CHECK_EQ(st.sense.key, KEY_UNIT_ATTENTION);
    CHECK_EQ(st.sense.asc, 0x28);
    r = run({OP_READ_CAPACITY}, other, st);
    CHECK(r.reply.kind == Reply::Kind::Data);
    CHECK_EQ(be32(r.data, 0), 1999u);  // the new disc's last frame
    // REQUEST SENSE reports a pending one as 28h too
    discChanged(st, other);
    r = run({OP_REQUEST_SENSE, 0, 0, 0, 18}, other, st);
    CHECK_EQ(r.data[2], KEY_UNIT_ATTENTION);
    CHECK_EQ(r.data[12], 0x28);
    CHECK(!st.unit_attention);
    // A reset after it reports power-on again (29h)
    powerOn(st, other);
    r = run({OP_TEST_UNIT_READY}, other, st);
    CHECK_EQ(st.sense.asc, 0x29);
}

TEST(mmc_mode_sense) {
    State st = ready();
    Run r = run({OP_MODE_SENSE_10, 0, 0x2A, 0, 0, 0, 0, 0x00, 0xFF}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(8 + 22));
    CHECK_EQ(r.data[0] << 8 | r.data[1], int(r.data.size() - 2));
    CHECK_EQ(r.data[2], 0x01);  // a data CD
    CHECK_EQ(r.data[8], 0x2A);
    CHECK_EQ(r.data[9], 20);
    r = run({OP_MODE_SENSE_10, 0, 0x3F, 0, 0, 0, 0, 0x00, 0xFF}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(8 + 8 + 22));
    CHECK_EQ(r.data[8], 0x01);
    CHECK_EQ(r.data[16], 0x2A);
    // MODE SENSE(6)
    r = run({OP_MODE_SENSE_6, 0, 0x01, 0, 0xFF}, DISC, st);
    CHECK_EQ(r.data.size(), size_t(4 + 8));
    CHECK_EQ(r.data[0], 11);
    // Unknown pages, saved values
    CHECK(run({OP_MODE_SENSE_10, 0, 0x0E, 0, 0, 0, 0, 0, 0xFF}, DISC, st).reply.kind == Reply::Kind::Error);
    CHECK(sameSense(st.sense, SENSE_BAD_FIELD));
    CHECK(run({OP_MODE_SENSE_10, 0, 0xEA, 0, 0, 0, 0, 0, 0xFF}, DISC, st).reply.kind == Reply::Kind::Error);
    CHECK(sameSense(st.sense, SENSE_NO_SAVING));
    // No disc: still answered, with the medium type saying so
    r = run({OP_MODE_SENSE_10, 0, 0x2A, 0, 0, 0, 0, 0x00, 0xFF}, NO_DISC, st);
    CHECK_EQ(r.data[2], 0x70);
}

TEST(mmc_accepted_and_refused) {
    State st = ready();
    CHECK(run({OP_START_STOP_UNIT, 0, 0, 0, 0x02}, DISC, st).reply.kind == Reply::Kind::Good);
    CHECK(run({OP_PREVENT_ALLOW, 0, 0, 0, 1}, DISC, st).reply.kind == Reply::Kind::Good);
    CHECK(run({OP_SET_CD_SPEED, 0, 0xFF, 0xFF}, DISC, st).reply.kind == Reply::Kind::Good);
    CHECK(run({0xBE}, DISC, st).reply.kind == Reply::Kind::Error);  // READ CD: not yet
    CHECK(sameSense(st.sense, SENSE_BAD_OPCODE));
    CHECK_STR(name(OP_READ_10), "READ_10");
    CHECK_STR(name(0xBE), "?");
}

void runMmcTests() {
    RUN(mmc_unit_attention_once_after_power_on);
    RUN(mmc_request_sense_reports_and_clears);
    RUN(mmc_request_sense_reports_a_pending_unit_attention);
    RUN(mmc_inquiry);
    RUN(mmc_test_unit_ready_without_a_disc);
    RUN(mmc_read_capacity);
    RUN(mmc_reads);
    RUN(mmc_seek_and_verify);
    RUN(mmc_msf);
    RUN(mmc_read_toc_format_0);
    RUN(mmc_read_toc_sessions_and_full);
    RUN(mmc_get_configuration);
    RUN(mmc_get_event_status);
    RUN(mmc_disc_changes);
    RUN(mmc_mode_sense);
    RUN(mmc_accepted_and_refused);
}
