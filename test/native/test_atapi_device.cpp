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
}
