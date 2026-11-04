// Tests for the CD-ROM build's ATAPI device (atapi_device): the reset
// signature, IDENTIFY PACKET DEVICE and whole PACKET exchanges, register by
// register, through a virtual host that talks to it as a host driver does.
#include <cstdint>
#include <cstring>
#include <vector>

#include "atapi_device.h"
#include "block_source.h"
#include "check.h"
#include "gen_disk.h"

using atapi::Device;
using ata::Reg;

namespace {

constexpr uint32_t TEST_SECTORS = 4096;  // 1,024 frames
const IdentifyInfo INFO{"HDDSYNTH PRO CD-ROM", "HSP-000001", "0.29.0", TEST_SECTORS, 3, NO_DMA};

// A source whose reads fail, for read errors.
class FailingDisk : public BlockSource {
public:
    uint32_t sectors() const override { return TEST_SECTORS; }
    bool read(uint32_t, uint16_t[256]) override { return false; }
};

struct VirtualHost {
    VirtualHost(Device& d, BlockSource* s) : dev(d), source(s) {}

    Device& dev;
    BlockSource* source;
    unsigned interrupts = 0;  // data blocks and status phases that came with INTRQ
    std::vector<uint16_t> block_bytes;  // each DRQ block's byte count, for the last packet

    // Core 1 and core 0's share: run a queued command or packet, serve a fetch.
    void pump() {
        if (dev.commandQueued()) dev.runQueuedCommand();
        ata::Fetch f;
        if (source && dev.pendingFetch(f)) dev.fetchDone(f.id, source->read(f.lba, f.buffer));
    }

    uint8_t altStatus() { return uint8_t(dev.read(Reg::AltStatusDevCtrl)); }
    uint8_t reg(Reg r) { return uint8_t(dev.read(r)); }

    bool waitNotBusy() {
        for (int i = 0; i < 1000; ++i) {
            pump();
            if (!(altStatus() & ata::ST_BSY)) return true;
        }
        return false;
    }

    bool atapiSignature() {
        return reg(Reg::SectorCount) == 1 && reg(Reg::LbaLow) == 1 && reg(Reg::LbaMid) == 0x14 &&
               reg(Reg::LbaHigh) == 0xEB;
    }

    void command(uint8_t c) { dev.write(Reg::StatusCommand, c); }

    // A PACKET exchange the PIO way: the packet, then each data block (its
    // length from the byte count registers), then the status phase. Returns
    // the final Status; `out` gets the data.
    uint8_t packet(std::initializer_list<uint8_t> bytes, std::vector<uint8_t>& out, uint16_t limit = 0xFFFE) {
        uint8_t p[12] = {};
        unsigned n = 0;
        for (uint8_t b : bytes) p[n++] = b;
        out.clear();
        block_bytes.clear();
        dev.write(Reg::Device, 0xA0);
        dev.write(Reg::ErrorFeatures, 0);  // PIO
        dev.write(Reg::LbaMid, limit & 0xFF);
        dev.write(Reg::LbaHigh, limit >> 8);
        command(atapi::CMD_PACKET);
        if (!waitNotBusy()) return 0xFF;
        uint8_t st = altStatus();
        if (!(st & ata::ST_DRQ)) return st;  // refused at once
        CHECK_EQ(reg(Reg::SectorCount), atapi::IR_COD);
        for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
        for (int guard = 0; guard < 100000; ++guard) {
            if (!waitNotBusy()) return 0xFF;
            if (dev.intrq()) ++interrupts;
            st = reg(Reg::StatusCommand);  // and acknowledge the interrupt
            if (!(st & ata::ST_DRQ)) {
                CHECK_EQ(reg(Reg::SectorCount), atapi::IR_COD | atapi::IR_IO);
                return st;
            }
            CHECK_EQ(reg(Reg::SectorCount), atapi::IR_IO);
            uint16_t count = uint16_t(reg(Reg::LbaMid) | reg(Reg::LbaHigh) << 8);
            block_bytes.push_back(count);
            for (unsigned w = 0; w < (count + 1u) / 2; ++w) {
                uint16_t v = dev.read(Reg::Data);
                out.push_back(uint8_t(v));
                if (2 * w + 1 < count) out.push_back(uint8_t(v >> 8));
            }
        }
        return 0xFF;
    }

    // Clears the power-on unit attention, as a host's first command does.
    void clearAttention() {
        std::vector<uint8_t> d;
        packet({mmc::OP_TEST_UNIT_READY}, d);
    }
};

// What the test disc holds at frame `lba`: generated sectors 4n to 4n+3.
std::vector<uint8_t> expectedFrames(uint32_t lba, uint32_t frames) {
    std::vector<uint8_t> v;
    uint8_t sector[gen_disk::SECTOR_BYTES];
    for (uint32_t s = lba * 4; s < (lba + frames) * 4; ++s) {
        gen_disk::sector(s, sector);
        v.insert(v.end(), sector, sector + sizeof sector);
    }
    return v;
}

}  // namespace

TEST(atapi_signature_after_power_on_and_reset) {
    Device dev(INFO);
    VirtualHost host{dev, nullptr};
    CHECK(host.atapiSignature());
    CHECK_EQ(host.altStatus(), 0x00);  // no DRDY until a command has run
    dev.hardReset(true);
    CHECK(host.altStatus() & ata::ST_BSY);
    dev.hardReset(false);
    CHECK(host.atapiSignature());
    CHECK_EQ(host.reg(Reg::ErrorFeatures), 0x01);
    // A soft reset too
    dev.write(Reg::LbaMid, 0x55);
    dev.write(Reg::AltStatusDevCtrl, ata::DC_SRST);
    dev.write(Reg::AltStatusDevCtrl, 0);
    CHECK(host.atapiSignature());
}

TEST(atapi_identify_device_is_aborted_with_the_signature) {
    Device dev(INFO);
    VirtualHost host{dev, nullptr};
    dev.write(Reg::LbaMid, 0);
    dev.write(Reg::LbaHigh, 0);
    host.command(ata::CMD_IDENTIFY_DEVICE);
    CHECK(host.waitNotBusy());
    CHECK(dev.intrq());
    uint8_t st = host.reg(Reg::StatusCommand);
    CHECK(st & ata::ST_ERR);
    CHECK(!(st & ata::ST_DRQ));
    CHECK_EQ(host.reg(Reg::ErrorFeatures), ata::ER_ABRT);
    CHECK(host.atapiSignature());
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(atapi_identify_packet_device) {
    Device dev(INFO);
    VirtualHost host{dev, nullptr};
    host.command(atapi::CMD_IDENTIFY_PACKET_DEVICE);
    CHECK(host.waitNotBusy());
    CHECK(dev.intrq());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_DRQ);
    uint16_t w[256];
    for (auto& word : w) word = dev.read(Reg::Data);
    CHECK_EQ(w[0], 0x85C0);  // ATAPI, CD-ROM, removable, accelerated DRQ, 12-byte packets
    CHECK_EQ(w[27], uint16_t('H' << 8 | 'D'));
    CHECK_EQ(w[49] & (1u << 8), 0u);  // no DMA
    CHECK_EQ(w[64], 0x0001);          // PIO mode 3
    CHECK(w[82] & (1u << 4));         // PACKET
    uint8_t sum = 0;
    for (uint16_t word : w) sum = uint8_t(sum + (word & 0xFF) + (word >> 8));
    CHECK_EQ(sum, 0);
    CHECK_EQ(w[255] & 0xFF, 0xA5);
    CHECK_EQ(host.altStatus(), ata::ST_DRDY);  // done, no interrupt at the end
    CHECK(!dev.intrq());
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(atapi_first_packet_reports_the_unit_attention) {
    Device dev(INFO);
    VirtualHost host{dev, nullptr};
    std::vector<uint8_t> d;
    uint8_t st = host.packet({mmc::OP_TEST_UNIT_READY}, d);
    CHECK(st & ata::ST_ERR);
    CHECK_EQ(host.reg(Reg::ErrorFeatures) >> 4, mmc::KEY_UNIT_ATTENTION);
    st = host.packet({mmc::OP_REQUEST_SENSE, 0, 0, 0, 18}, d);
    CHECK(!(st & ata::ST_ERR));
    CHECK_EQ(d.size(), size_t(18));
    CHECK_EQ(d[2], mmc::KEY_UNIT_ATTENTION);
    CHECK_EQ(d[12], 0x29);
    CHECK_EQ(host.packet({mmc::OP_TEST_UNIT_READY}, d), ata::ST_DRDY);
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
    CHECK_EQ(dev.lastCommand().command, atapi::CMD_PACKET);
    CHECK_EQ(dev.lastCommand().packet, mmc::OP_TEST_UNIT_READY);
}

TEST(atapi_inquiry_in_blocks_of_the_hosts_limit) {
    Device dev(INFO);
    VirtualHost host{dev, nullptr};
    std::vector<uint8_t> d;
    CHECK_EQ(host.packet({mmc::OP_INQUIRY, 0, 0, 0, 36}, d), ata::ST_DRDY);
    CHECK_EQ(d.size(), size_t(36));
    CHECK_EQ(host.block_bytes.size(), size_t(1));
    CHECK_STR(std::string(d.begin() + 8, d.begin() + 16), "HDDSYNTH");
    // A host that takes 15 bytes at a time: blocks of whole words, the last short
    unsigned before = host.interrupts;
    CHECK_EQ(host.packet({mmc::OP_INQUIRY, 0, 0, 0, 36}, d, 15), ata::ST_DRDY);
    CHECK_EQ(d.size(), size_t(36));
    CHECK(host.block_bytes == std::vector<uint16_t>({14, 14, 8}));
    CHECK_EQ(host.interrupts - before, 4u);  // each block, and the status
    CHECK_STR(std::string(d.begin() + 16, d.begin() + 26), "PRO CD-ROM");
    // An odd length: the last word's high byte is padding
    CHECK_EQ(host.packet({mmc::OP_INQUIRY, 0, 0, 0, 7}, d), ata::ST_DRDY);
    CHECK_EQ(d.size(), size_t(7));
    CHECK_EQ(host.block_bytes[0], 7);
}

TEST(atapi_reads_frames_from_the_disc) {
    Device dev(INFO);
    GeneratedDisk disc(TEST_SECTORS);
    VirtualHost host{dev, &disc};
    host.clearAttention();
    std::vector<uint8_t> d;
    unsigned before = host.interrupts;
    CHECK_EQ(host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 5, 0, 0, 2}, d), ata::ST_DRDY);  // 2 frames from 5
    CHECK(d == expectedFrames(5, 2));
    CHECK_EQ(host.block_bytes.size(), size_t(8));  // a 512-byte block per sector fetched
    for (uint16_t b : host.block_bytes) CHECK_EQ(b, 512);
    CHECK_EQ(host.interrupts - before, 9u);
    CHECK_EQ(dev.lastCommand().packet, mmc::OP_READ_10);
    CHECK_EQ(dev.lastCommand().lba, 5u);
    CHECK_EQ(dev.lastCommand().count, 2);
    // The last frame, by READ(12)
    CHECK_EQ(host.packet({mmc::OP_READ_12, 0, 0, 0, 0x03, 0xFF, 0, 0, 0, 1}, d), ata::ST_DRDY);
    CHECK(d == expectedFrames(1023, 1));
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(atapi_reads_in_blocks_smaller_than_a_sector) {
    Device dev(INFO);
    GeneratedDisk disc(TEST_SECTORS);
    VirtualHost host{dev, &disc};
    host.clearAttention();
    std::vector<uint8_t> d;
    CHECK_EQ(host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 9, 0, 0, 1}, d, 200), ata::ST_DRDY);
    CHECK(d == expectedFrames(9, 1));
    CHECK_EQ(host.block_bytes.size(), size_t(12));  // 200, 200, 112 for each of the 4 sectors
    CHECK_EQ(host.block_bytes[2], 112);
}

TEST(atapi_read_errors) {
    Device dev(INFO);
    GeneratedDisk disc(TEST_SECTORS);
    VirtualHost host{dev, &disc};
    host.clearAttention();
    std::vector<uint8_t> d;
    // Past the end: CHECK CONDITION, ILLEGAL REQUEST, with ABRT
    uint8_t st = host.packet({mmc::OP_READ_10, 0, 0, 0, 0x03, 0xFF, 0, 0, 2}, d);
    CHECK(st & ata::ST_ERR);
    CHECK_EQ(host.reg(Reg::ErrorFeatures), uint8_t(mmc::KEY_ILLEGAL_REQUEST << 4 | ata::ER_ABRT));
    CHECK(d.empty());
    // The card fails: MEDIUM ERROR
    FailingDisk bad;
    host.source = &bad;
    st = host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1}, d);
    CHECK(st & ata::ST_ERR);
    CHECK_EQ(host.reg(Reg::ErrorFeatures) >> 4, mmc::KEY_MEDIUM_ERROR);
    host.source = &disc;
    CHECK_EQ(host.packet({mmc::OP_REQUEST_SENSE, 0, 0, 0, 18}, d), ata::ST_DRDY);
    CHECK_EQ(d[12], 0x11);
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(atapi_no_disc) {
    IdentifyInfo empty = INFO;
    empty.total_sectors = 0;
    Device dev(empty);
    VirtualHost host{dev, nullptr};
    host.clearAttention();
    std::vector<uint8_t> d;
    CHECK(host.packet({mmc::OP_TEST_UNIT_READY}, d) & ata::ST_ERR);
    CHECK_EQ(host.reg(Reg::ErrorFeatures) >> 4, mmc::KEY_NOT_READY);
    CHECK_EQ(host.packet({mmc::OP_INQUIRY, 0, 0, 0, 36}, d), ata::ST_DRDY);
}

TEST(atapi_device_reset_recovers_a_busy_drive) {
    Device dev(INFO);
    VirtualHost host{dev, nullptr};  // nothing serves fetches: the read stays BSY
    host.clearAttention();
    dev.write(Reg::LbaMid, 0xFE);
    dev.write(Reg::LbaHigh, 0xFF);
    host.command(atapi::CMD_PACKET);
    host.pump();
    const uint8_t p[12] = {mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1};
    for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    host.pump();
    CHECK(host.altStatus() & ata::ST_BSY);
    ata::Fetch stale;
    CHECK(dev.pendingFetch(stale));
    host.command(atapi::CMD_DEVICE_RESET);
    CHECK(host.waitNotBusy());
    CHECK(host.atapiSignature());
    CHECK(!dev.intrq());
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
    dev.fetchDone(stale.id, true);  // the old fetch's result: ignored
    CHECK(!(host.altStatus() & ata::ST_DRQ));
}

TEST(atapi_refuses_dma_and_other_ata_commands) {
    Device dev(INFO);
    VirtualHost host{dev, nullptr};
    dev.write(Reg::ErrorFeatures, atapi::FEATURES_DMA);
    host.command(atapi::CMD_PACKET);
    CHECK(host.waitNotBusy());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_ERR);
    CHECK_EQ(host.reg(Reg::ErrorFeatures), ata::ER_ABRT);
    host.command(ata::CMD_WRITE_SECTORS);
    CHECK(host.waitNotBusy());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_ERR);
    // The housekeeping a bridge sends
    dev.write(Reg::ErrorFeatures, ata::SF_SET_TRANSFER_MODE);
    dev.write(Reg::SectorCount, ata::XFER_PIO_FLOW_0 + 3);
    host.command(ata::CMD_SET_FEATURES);
    CHECK(host.waitNotBusy());
    CHECK_EQ(host.reg(Reg::StatusCommand), ata::ST_DRDY);
    dev.write(Reg::SectorCount, ata::XFER_MDMA_0 + 2);
    host.command(ata::CMD_SET_FEATURES);
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_ERR);
    host.command(ata::CMD_CHECK_POWER_MODE);
    CHECK_EQ(host.reg(Reg::SectorCount), ata::POWER_ACTIVE_OR_IDLE);
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(atapi_device_1_is_absent) {
    Device dev(INFO);
    VirtualHost host{dev, nullptr};
    dev.write(Reg::Device, 0xB0);
    CHECK_EQ(host.altStatus(), 0);
    host.command(atapi::CMD_IDENTIFY_PACKET_DEVICE);
    CHECK_EQ(dev.commandsStarted(), 0u);
    dev.write(Reg::Device, 0xA0);
    host.command(atapi::CMD_IDENTIFY_PACKET_DEVICE);
    CHECK(host.waitNotBusy());
    CHECK(host.altStatus() & ata::ST_DRQ);
}

TEST(atapi_trace_names_the_packet) {
    // Through the trace's own builder, as core 1 records it
    Device dev(INFO);
    GeneratedDisk disc(TEST_SECTORS);
    VirtualHost host{dev, &disc};
    host.clearAttention();
    std::vector<uint8_t> d;
    host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 7, 0, 0, 3}, d);
    const atapi::CommandInfo& c = dev.lastCommand();
    CHECK_EQ(c.command, 0xA0);
    CHECK_EQ(c.packet, mmc::OP_READ_10);
    CHECK_EQ(c.lba, 7u);
    CHECK_EQ(c.count, 3);
}

// ---- Awkward hosts: what real drivers do that a tidy host doesn't ------------

namespace {

// A packet written by hand, up to the end of its command phase (the device
// has it, and has run it unless `run` is false).
void writePacket(VirtualHost& host, std::initializer_list<uint8_t> bytes, uint16_t limit, bool run = true) {
    uint8_t p[12] = {};
    unsigned n = 0;
    for (uint8_t b : bytes) p[n++] = b;
    host.dev.write(Reg::Device, 0xA0);
    host.dev.write(Reg::ErrorFeatures, 0);
    host.dev.write(Reg::LbaMid, limit & 0xFF);
    host.dev.write(Reg::LbaHigh, limit >> 8);
    host.command(atapi::CMD_PACKET);
    host.pump();  // core 1 runs A0h: DRQ for the packet
    for (unsigned i = 0; i < 6; ++i) host.dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    if (run) host.dev.runQueuedCommand();
}

}  // namespace

TEST(atapi_byte_count_limit_of_zero_means_any) {
    // ATAPI forbids 0; some drivers send it anyway. Taken as the largest.
    Device dev(INFO);
    GeneratedDisk disc(TEST_SECTORS);
    VirtualHost host{dev, &disc};
    host.clearAttention();
    std::vector<uint8_t> d;
    CHECK_EQ(host.packet({mmc::OP_INQUIRY, 0, 0, 0, 36}, d, 0), ata::ST_DRDY);
    CHECK_EQ(d.size(), size_t(36));
    CHECK_EQ(host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 3, 0, 0, 1}, d, 0), ata::ST_DRDY);
    CHECK(d == expectedFrames(3, 1));
    // An odd limit, and one of 1: blocks stay whole words
    CHECK_EQ(host.packet({mmc::OP_INQUIRY, 0, 0, 0, 36}, d, 1), ata::ST_DRDY);
    CHECK_EQ(d.size(), size_t(36));
    CHECK_EQ(host.block_bytes[0], 2);
    CHECK_EQ(host.packet({mmc::OP_INQUIRY, 0, 0, 0, 36}, d, 11), ata::ST_DRDY);
    CHECK_EQ(host.block_bytes[0], 10);
}

TEST(atapi_polling_host_with_interrupts_off) {
    // nIEN set: INTRQ never driven, and a host that only polls Alternate
    // Status (no acknowledging read of Status) still gets every block.
    Device dev(INFO);
    GeneratedDisk disc(TEST_SECTORS);
    VirtualHost host{dev, &disc};
    host.clearAttention();
    dev.write(Reg::AltStatusDevCtrl, ata::DC_NIEN);
    writePacket(host, {mmc::OP_READ_10, 0, 0, 0, 0, 2, 0, 0, 1}, 0xFFFE);
    std::vector<uint8_t> d;
    for (int guard = 0; guard < 10000; ++guard) {
        CHECK(!dev.intrq());
        CHECK(!dev.intrqDriven());
        if (!host.waitNotBusy()) break;
        if (!(host.altStatus() & ata::ST_DRQ)) break;
        uint16_t count = uint16_t(host.reg(Reg::LbaMid) | host.reg(Reg::LbaHigh) << 8);
        for (unsigned w = 0; w < count / 2u; ++w) {
            uint16_t v = dev.read(Reg::Data);
            d.push_back(uint8_t(v));
            d.push_back(uint8_t(v >> 8));
        }
    }
    CHECK(d == expectedFrames(2, 1));
    CHECK_EQ(host.altStatus(), ata::ST_DRDY);
    CHECK_EQ(host.reg(Reg::SectorCount), atapi::IR_COD | atapi::IR_IO);
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(atapi_host_gives_up_mid_block) {
    // A driver that times out reads half a block, then sends the next packet.
    Device dev(INFO);
    GeneratedDisk disc(TEST_SECTORS);
    VirtualHost host{dev, &disc};
    host.clearAttention();
    writePacket(host, {mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 4}, 0xFFFE);
    CHECK(host.waitNotBusy());
    CHECK(host.altStatus() & ata::ST_DRQ);
    for (int i = 0; i < 100; ++i) dev.read(Reg::Data);
    std::vector<uint8_t> d;
    CHECK_EQ(host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 9, 0, 0, 1}, d), ata::ST_DRDY);
    CHECK(d == expectedFrames(9, 1));
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
    // Or gives up at the packet itself, three words in
    host.dev.write(Reg::LbaMid, 0xFE);
    host.dev.write(Reg::LbaHigh, 0xFF);
    host.command(atapi::CMD_PACKET);
    host.pump();
    for (int i = 0; i < 3; ++i) dev.write(Reg::Data, 0);
    CHECK_EQ(host.packet({mmc::OP_INQUIRY, 0, 0, 0, 36}, d), ata::ST_DRDY);
    CHECK_EQ(d.size(), size_t(36));
}

TEST(atapi_reply_survives_a_stale_fetch) {
    // After a soft reset cuts a read short, core 0 may still be filling the
    // sector buffer for the old fetch; the next packet's reply is apart from it.
    Device dev(INFO);
    VirtualHost host{dev, nullptr};  // fetches are served by hand here
    host.clearAttention();
    writePacket(host, {mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1}, 0xFFFE);
    ata::Fetch stale;
    CHECK(dev.pendingFetch(stale));
    dev.write(Reg::AltStatusDevCtrl, ata::DC_SRST);
    dev.write(Reg::AltStatusDevCtrl, 0);
    writePacket(host, {mmc::OP_INQUIRY, 0, 0, 0, 36}, 0xFFFE);
    std::memset(stale.buffer, 0x5A, 512);  // core 0, late
    dev.fetchDone(stale.id, true);
    CHECK(host.altStatus() & ata::ST_DRQ);
    CHECK_EQ(host.reg(Reg::LbaMid), 36);
    std::vector<uint8_t> d;
    for (int w = 0; w < 18; ++w) {
        uint16_t v = dev.read(Reg::Data);
        d.push_back(uint8_t(v));
        d.push_back(uint8_t(v >> 8));
    }
    CHECK_EQ(d[0], 0x05);
    CHECK_STR(std::string(d.begin() + 8, d.begin() + 16), "HDDSYNTH");
}

TEST(atapi_execute_device_diagnostic) {
    Device dev(INFO);
    VirtualHost host{dev, nullptr};
    dev.write(Reg::LbaMid, 0);
    host.command(ata::CMD_EXECUTE_DEVICE_DIAGNOSTIC);
    CHECK(host.waitNotBusy());
    CHECK(dev.intrq());
    CHECK(host.atapiSignature());
    CHECK_EQ(host.reg(Reg::ErrorFeatures), 0x01);  // passed, no device 1
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(atapi_reads_past_the_drq_block_get_nothing) {
    // A driver that ignores the byte count and reads a whole 2,048-byte frame
    // in one go: after the first 512-byte block the drive is BSY fetching the
    // next, so the extra reads return 0 and move nothing. The data isn't lost
    // to a host that then waits for DRQ. (Drivers that don't wait are docs/
    // atapi.md's open question: they'd need 2,048-byte blocks.)
    Device dev(INFO);
    VirtualHost host{dev, nullptr};
    host.clearAttention();
    writePacket(host, {mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1}, 0xFFFE);
    GeneratedDisk disc(TEST_SECTORS);
    host.source = &disc;
    CHECK(host.waitNotBusy());
    host.source = nullptr;  // core 0 slow: the next sector isn't in yet
    for (int i = 0; i < 256; ++i) dev.read(Reg::Data);
    CHECK(host.altStatus() & ata::ST_BSY);
    CHECK_EQ(dev.read(Reg::Data), 0);
    host.source = &disc;
    CHECK(host.waitNotBusy());
    CHECK(host.altStatus() & ata::ST_DRQ);
    CHECK_EQ(host.reg(Reg::LbaMid) | host.reg(Reg::LbaHigh) << 8, 512);
}

// ---- PACKET with its DMA bit (multiword DMA offered) ---------------------------

namespace {

IdentifyInfo dmaInfo() {
    IdentifyInfo info = INFO;
    info.mdma_mode = 2;
    return info;
}

// A PACKET exchange the DMA way, as the USB adapter does it: the DMA bit and
// no byte count limit, the packet by PIO, then each block as ide_dma moves
// it (DMARQ, a burst, dmaBurstDone), until the status phase. Returns the
// final Status; `out` gets the data, `bursts` each burst's words.
uint8_t dmaPacket(VirtualHost& host, std::initializer_list<uint8_t> bytes, std::vector<uint8_t>& out,
                  std::vector<uint16_t>& bursts) {
    Device& dev = host.dev;
    uint8_t p[12] = {};
    unsigned n = 0;
    for (uint8_t b : bytes) p[n++] = b;
    out.clear();
    bursts.clear();
    dev.write(Reg::Device, 0xA0);
    dev.write(Reg::ErrorFeatures, atapi::FEATURES_DMA);
    dev.write(Reg::LbaMid, 0);
    dev.write(Reg::LbaHigh, 0);
    host.command(atapi::CMD_PACKET);
    if (!host.waitNotBusy()) return 0xFF;
    uint8_t st = host.altStatus();
    if (!(st & ata::ST_DRQ)) return st;  // refused at once
    for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    for (int guard = 0; guard < 100000; ++guard) {
        if (!host.waitNotBusy()) return 0xFF;
        ata::DmaBurst burst;
        if (dev.dmaBurstReady(burst)) {
            CHECK(dev.dmaCommand());
            CHECK(!dev.intrq());           // DMARQ asks for a block, not INTRQ
            CHECK(!dev.transferring());    // core 1 isn't pinned to the bus meanwhile
            const uint16_t* next;
            uint16_t left;
            CHECK(!dev.dataInRemaining(next, left));  // nor the fast lane
            ata::DmaBurst again;
            CHECK(!dev.dmaBurstReady(again));  // one burst at a time
            bursts.push_back(burst.count);
            for (uint16_t w = 0; w < burst.count; ++w) {
                out.push_back(uint8_t(burst.from[w]));
                out.push_back(uint8_t(burst.from[w] >> 8));
            }
            dev.dmaBurstDone();
            continue;
        }
        if (dev.intrq()) ++host.interrupts;
        st = host.reg(Reg::StatusCommand);
        if (!(st & ata::ST_DRQ)) {
            CHECK(!dev.dmaCommand());  // the status phase: core 1 ends the DMA command
            CHECK_EQ(host.reg(Reg::SectorCount), atapi::IR_COD | atapi::IR_IO);
            return st;
        }
    }
    return 0xFF;
}

}  // namespace

TEST(atapi_offers_multiword_dma) {
    Device dev(dmaInfo());
    VirtualHost host{dev, nullptr};
    auto identify = [&](uint16_t w[256]) {
        host.command(atapi::CMD_IDENTIFY_PACKET_DEVICE);
        CHECK(host.waitNotBusy());
        for (int i = 0; i < 256; ++i) w[i] = dev.read(Reg::Data);
        uint8_t sum = 0;
        for (int i = 0; i < 256; ++i) sum = uint8_t(sum + (w[i] & 0xFF) + (w[i] >> 8));
        CHECK_EQ(sum, 0);
    };
    uint16_t w[256];
    identify(w);
    CHECK(w[49] & (1u << 8));  // DMA supported
    CHECK_EQ(w[63], 0x0407);   // modes 0-2, mode 2 selected
    // SET FEATURES picks mode 1; word 63 follows, checksum and all
    dev.write(Reg::ErrorFeatures, ata::SF_SET_TRANSFER_MODE);
    dev.write(Reg::SectorCount, ata::XFER_MDMA_0 + 1);
    host.command(ata::CMD_SET_FEATURES);
    CHECK(host.waitNotBusy());
    CHECK_EQ(host.reg(Reg::StatusCommand), ata::ST_DRDY);
    identify(w);
    CHECK_EQ(w[63], 0x0207);
    // Past the mode offered: refused
    dev.write(Reg::ErrorFeatures, ata::SF_SET_TRANSFER_MODE);
    dev.write(Reg::SectorCount, ata::XFER_MDMA_0 + 3);
    host.command(ata::CMD_SET_FEATURES);
    CHECK(host.waitNotBusy());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_ERR);
}

TEST(atapi_dma_packets) {
    Device dev(dmaInfo());
    GeneratedDisk disc(TEST_SECTORS);
    VirtualHost host{dev, &disc};
    std::vector<uint8_t> d;
    std::vector<uint16_t> bursts;
    // No data (the adapter's first packet): the unit attention, then good
    uint8_t st = dmaPacket(host, {mmc::OP_TEST_UNIT_READY}, d, bursts);
    CHECK(st & ata::ST_ERR);
    CHECK_EQ(host.reg(Reg::ErrorFeatures) >> 4, mmc::KEY_UNIT_ATTENTION);
    CHECK(bursts.empty());
    CHECK(!dev.dmaCommand());
    CHECK_EQ(dmaPacket(host, {mmc::OP_TEST_UNIT_READY}, d, bursts), ata::ST_DRDY);
    // A reply in one burst, whatever the byte count registers say
    unsigned before = host.interrupts;
    CHECK_EQ(dmaPacket(host, {mmc::OP_INQUIRY, 0, 0, 0, 36}, d, bursts), ata::ST_DRDY);
    CHECK(bursts == std::vector<uint16_t>({18}));
    CHECK_STR(std::string(d.begin() + 8, d.begin() + 16), "HDDSYNTH");
    CHECK_EQ(host.interrupts - before, 1u);  // the status phase only
    // An odd length: the last word's high byte is padding
    CHECK_EQ(dmaPacket(host, {mmc::OP_REQUEST_SENSE, 0, 0, 0, 17}, d, bursts), ata::ST_DRDY);
    CHECK(bursts == std::vector<uint16_t>({9}));
    // Two frames: a burst per sector fetched, the data intact
    before = host.interrupts;
    CHECK_EQ(dmaPacket(host, {mmc::OP_READ_10, 0, 0, 0, 0, 5, 0, 0, 2}, d, bursts), ata::ST_DRDY);
    CHECK(d == expectedFrames(5, 2));
    CHECK_EQ(bursts.size(), size_t(8));
    for (uint16_t b : bursts) CHECK_EQ(b, 256);
    CHECK_EQ(host.interrupts - before, 1u);
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
    // PIO still works on the same drive
    CHECK_EQ(host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 7, 0, 0, 1}, d), ata::ST_DRDY);
    CHECK(d == expectedFrames(7, 1));
}

TEST(atapi_dma_errors_and_resets_end_the_dma_command) {
    Device dev(dmaInfo());
    GeneratedDisk disc(TEST_SECTORS);
    VirtualHost host{dev, &disc};
    std::vector<uint8_t> d;
    std::vector<uint16_t> bursts;
    dmaPacket(host, {mmc::OP_TEST_UNIT_READY}, d, bursts);  // the unit attention
    // The card fails mid-read: CHECK CONDITION, and no DMA command left
    FailingDisk bad;
    host.source = &bad;
    uint8_t st = dmaPacket(host, {mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1}, d, bursts);
    CHECK(st & ata::ST_ERR);
    CHECK_EQ(host.reg(Reg::ErrorFeatures) >> 4, mmc::KEY_MEDIUM_ERROR);
    CHECK(!dev.dmaCommand());
    host.source = &disc;
    // A burst under way when RESET# comes: the DMA command ends with it
    dev.write(Reg::ErrorFeatures, atapi::FEATURES_DMA);
    host.command(atapi::CMD_PACKET);
    host.pump();
    const uint8_t p[12] = {mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1};
    for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    CHECK(host.waitNotBusy());
    ata::DmaBurst burst;
    CHECK(dev.dmaBurstReady(burst));
    dev.hardReset(true);
    CHECK(!dev.dmaCommand());
    dev.hardReset(false);
    dev.dmaBurstDone();  // core 1 finishing the cut-short burst: nothing to move on
    CHECK(host.atapiSignature());
    CHECK(!(host.altStatus() & ata::ST_DRQ));
    // The same with SRST
    dmaPacket(host, {mmc::OP_TEST_UNIT_READY}, d, bursts);  // the reset's unit attention
    dev.write(Reg::ErrorFeatures, atapi::FEATURES_DMA);
    host.command(atapi::CMD_PACKET);
    host.pump();
    for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    CHECK(host.waitNotBusy());
    CHECK(dev.dmaBurstReady(burst));
    dev.write(Reg::AltStatusDevCtrl, ata::DC_SRST);
    CHECK(!dev.dmaCommand());
    dev.write(Reg::AltStatusDevCtrl, 0);
    CHECK(host.atapiSignature());
}

void runAtapiDeviceTests() {
    RUN(atapi_signature_after_power_on_and_reset);
    RUN(atapi_identify_device_is_aborted_with_the_signature);
    RUN(atapi_identify_packet_device);
    RUN(atapi_first_packet_reports_the_unit_attention);
    RUN(atapi_inquiry_in_blocks_of_the_hosts_limit);
    RUN(atapi_reads_frames_from_the_disc);
    RUN(atapi_reads_in_blocks_smaller_than_a_sector);
    RUN(atapi_read_errors);
    RUN(atapi_no_disc);
    RUN(atapi_device_reset_recovers_a_busy_drive);
    RUN(atapi_refuses_dma_and_other_ata_commands);
    RUN(atapi_device_1_is_absent);
    RUN(atapi_trace_names_the_packet);
    RUN(atapi_byte_count_limit_of_zero_means_any);
    RUN(atapi_polling_host_with_interrupts_off);
    RUN(atapi_host_gives_up_mid_block);
    RUN(atapi_reply_survives_a_stale_fetch);
    RUN(atapi_execute_device_diagnostic);
    RUN(atapi_reads_past_the_drq_block_get_nothing);
    RUN(atapi_offers_multiword_dma);
    RUN(atapi_dma_packets);
    RUN(atapi_dma_errors_and_resets_end_the_dma_command);
}
