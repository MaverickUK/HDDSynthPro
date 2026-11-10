// Tests for the drive build's device (hdcd_device): a hard disk as device 0
// and a CD-ROM drive as device 1 on one cable, or the CD-ROM alone as device
// 0 (`device0 = cdrom`), through a virtual
// host that talks to both as a BIOS and a CD-ROM driver do, with core 0's
// share serving each unit's fetches from its own disk.
#include <cstdint>
#include <cstring>
#include <vector>

#include "block_source.h"
#include "check.h"
#include "gen_disk.h"
#include "hdcd_device.h"

using ata::Reg;
using hdcd::Device;

namespace {

const IdentifyInfo DISK{"HDDSYNTH PROTO", "HSP-000001", "0.30.0", 131072, 3, NO_DMA};
const IdentifyInfo DISC{"HDDSYNTH PRO CD-ROM", "HSP-000001-CD", "0.30.0", 4096, 3, NO_DMA};  // 1,024 frames
const IdentifyInfo DISK1{"HDDSYNTH PRO D1", "HSP-000001-D1", "0.30.0", 2048, 3, NO_DMA};

// The disc: generated sectors from 100,000 up, so its data can't pass for the disk's.
constexpr uint32_t DISC_BASE = 100000;
class DiscSource : public BlockSource {
public:
    uint32_t sectors() const override { return DISC.total_sectors; }
    bool read(uint32_t lba, uint16_t words[256]) override {
        if (lba >= DISC.total_sectors) return false;
        uint8_t bytes[512];
        gen_disk::sector(DISC_BASE + lba, bytes);
        std::memcpy(words, bytes, sizeof bytes);
        return true;
    }
};

struct Host {
    Device& dev;
    BlockSource* disks[2];
    int fetches[2] = {0, 0};
    bool serve = true;  // false: core 0 is slow, fetches wait

    void pump() {
        if (dev.commandQueued()) dev.runQueuedCommand();
        if (dev.dataOutHeld()) dev.releaseDataOut();
        ata::Fetch f;
        if (serve && dev.pendingFetch(f)) {
            ++fetches[f.unit];
            BlockSource* disk = disks[f.unit];
            bool ok = f.store ? disk->write(f.lba, f.buffer) : disk->read(f.lba, f.buffer);
            dev.fetchDone(f.id, ok);
        }
    }
    uint8_t reg(Reg r) { return uint8_t(dev.read(r)); }
    uint8_t altStatus() { return reg(Reg::AltStatusDevCtrl); }
    void select(int unit) { dev.write(Reg::Device, unit ? 0xB0 : 0xA0); }
    void command(uint8_t c) { dev.write(Reg::StatusCommand, c); }
    bool waitNotBusy() {
        for (int i = 0; i < 1000; ++i) {
            pump();
            if (!(altStatus() & ata::ST_BSY)) return true;
        }
        return false;
    }
    uint16_t signature() { return uint16_t(reg(Reg::LbaHigh) << 8 | reg(Reg::LbaMid)); }

    // READ SECTORS on device 0, the PIO way; `out` gets the words.
    uint8_t readSectors(uint32_t lba, uint8_t count, std::vector<uint16_t>& out) {
        out.clear();
        select(0);
        dev.write(Reg::SectorCount, count);
        dev.write(Reg::LbaLow, lba & 0xFF);
        dev.write(Reg::LbaMid, (lba >> 8) & 0xFF);
        dev.write(Reg::LbaHigh, (lba >> 16) & 0xFF);
        dev.write(Reg::Device, 0xE0 | ((lba >> 24) & 0x0F));
        command(ata::CMD_READ_SECTORS);
        for (unsigned s = 0; s < (count ? count : 256u); ++s) {
            if (!waitNotBusy()) return 0xFF;
            uint8_t st = reg(Reg::StatusCommand);
            if (!(st & ata::ST_DRQ)) return st;
            for (int w = 0; w < 256; ++w) out.push_back(dev.read(Reg::Data));
        }
        waitNotBusy();
        return reg(Reg::StatusCommand);
    }

    // A PACKET to device `unit` (the CD-ROM's: 1 unless it's alone), the PIO
    // way; `out` gets the data.
    uint8_t packet(std::initializer_list<uint8_t> bytes, std::vector<uint8_t>& out, int unit = 1) {
        uint8_t p[12] = {};
        unsigned n = 0;
        for (uint8_t b : bytes) p[n++] = b;
        out.clear();
        select(unit);
        dev.write(Reg::ErrorFeatures, 0);
        dev.write(Reg::LbaMid, 0xFE);
        dev.write(Reg::LbaHigh, 0xFF);
        command(atapi::CMD_PACKET);
        if (!waitNotBusy()) return 0xFF;
        if (!(altStatus() & ata::ST_DRQ)) return altStatus();
        for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
        for (int guard = 0; guard < 100000; ++guard) {
            if (!waitNotBusy()) return 0xFF;
            uint8_t st = reg(Reg::StatusCommand);
            if (!(st & ata::ST_DRQ)) return st;
            uint16_t count = uint16_t(reg(Reg::LbaMid) | reg(Reg::LbaHigh) << 8);
            for (unsigned w = 0; w < (count + 1u) / 2; ++w) {
                uint16_t v = dev.read(Reg::Data);
                out.push_back(uint8_t(v));
                if (2 * w + 1 < count) out.push_back(uint8_t(v >> 8));
            }
        }
        return 0xFF;
    }
};

std::vector<uint16_t> diskWords(uint32_t lba, unsigned count) {
    std::vector<uint16_t> v;
    uint8_t bytes[512];
    for (unsigned s = 0; s < count; ++s) {
        gen_disk::sector(lba + s, bytes);
        for (int w = 0; w < 256; ++w) v.push_back(uint16_t(bytes[2 * w] | bytes[2 * w + 1] << 8));
    }
    return v;
}

std::vector<uint8_t> discFrames(uint32_t frame, uint32_t frames) {
    std::vector<uint8_t> v;
    uint8_t bytes[512];
    for (uint32_t s = frame * 4; s < (frame + frames) * 4; ++s) {
        gen_disk::sector(DISC_BASE + s, bytes);
        v.insert(v.end(), bytes, bytes + 512);
    }
    return v;
}

}  // namespace

TEST(hdcd_each_device_shows_its_signature) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    CHECK(dev.hasCdrom());
    // After power-on, device 0 selected: the ATA signature, DRDY
    CHECK_EQ(host.signature(), 0x0000);
    CHECK(host.altStatus() & ata::ST_DRDY);
    // Device 1: the packet signature, Status 00h until a command has run
    host.select(1);
    CHECK_EQ(host.signature(), 0xEB14);
    CHECK_EQ(host.altStatus(), 0x00);
    // A hard reset: both again, device 0 selected
    dev.hardReset(true);
    dev.hardReset(false);
    CHECK_EQ(host.signature(), 0x0000);
    host.select(1);
    CHECK_EQ(host.signature(), 0xEB14);
    // And SRST
    host.select(0);
    dev.write(Reg::AltStatusDevCtrl, ata::DC_SRST);
    dev.write(Reg::AltStatusDevCtrl, 0);
    CHECK_EQ(host.signature(), 0x0000);
    host.select(1);
    CHECK_EQ(host.signature(), 0xEB14);
}

TEST(hdcd_a_bios_probe_finds_a_disk_and_a_cdrom) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    uint16_t w[256];
    // Device 0: IDENTIFY DEVICE
    host.select(0);
    host.command(ata::CMD_IDENTIFY_DEVICE);
    CHECK(host.waitNotBusy());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_DRQ);
    for (auto& word : w) word = dev.read(Reg::Data);
    CHECK_EQ(w[27], uint16_t('H' << 8 | 'D'));
    CHECK_EQ(w[0] & 0x8000, 0);  // ATA
    CHECK_EQ(uint32_t(w[60] | w[61] << 16), DISK.total_sectors);
    // Device 1: IDENTIFY DEVICE aborted with the packet signature, then IDENTIFY PACKET DEVICE
    host.select(1);
    host.command(ata::CMD_IDENTIFY_DEVICE);
    CHECK(host.waitNotBusy());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_ERR);
    CHECK_EQ(host.signature(), 0xEB14);
    host.command(atapi::CMD_IDENTIFY_PACKET_DEVICE);
    CHECK(host.waitNotBusy());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_DRQ);
    for (auto& word : w) word = dev.read(Reg::Data);
    CHECK_EQ(w[0], 0x85C0);  // ATAPI, CD-ROM
    CHECK_EQ(w[10], uint16_t('H' << 8 | 'S'));  // its own serial
    // Device 0 is still a disk, and unchanged
    host.select(0);
    CHECK_EQ(host.signature(), 0x0000);
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(hdcd_reads_from_both_interleaved) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    std::vector<uint8_t> d;
    host.packet({mmc::OP_TEST_UNIT_READY}, d);  // the unit attention
    std::vector<uint16_t> words;
    for (int round = 0; round < 3; ++round) {
        CHECK_EQ(host.readSectors(1000 + round * 7, 3, words), ata::ST_DRDY | ata::ST_DSC);
        CHECK(words == diskWords(1000 + round * 7, 3));
        CHECK_EQ(host.packet({mmc::OP_READ_10, 0, 0, 0, 0, uint8_t(10 + round), 0, 0, 2}, d), ata::ST_DRDY);
        CHECK(d == discFrames(10 + round, 2));
    }
    CHECK(host.fetches[0] >= 9);
    CHECK_EQ(host.fetches[1], 3 * 8);  // a fetch per 512-byte sector of the disc, all from unit 1
    // The trace sees which device ran each command
    CHECK_EQ(dev.lastCommand().unit, 1);
    CHECK_EQ(dev.lastCommand().packet, mmc::OP_READ_10);
    host.readSectors(5, 1, words);
    CHECK_EQ(dev.lastCommand().unit, 0);
    CHECK_EQ(dev.lastCommand().command, ata::CMD_READ_SECTORS);
    CHECK_EQ(dev.lastCommand().packet, 0);
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(hdcd_intrq_follows_the_selected_device) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    // The CD-ROM finishes a command with an interrupt...
    host.select(1);
    host.command(ata::CMD_CHECK_POWER_MODE);
    CHECK(host.waitNotBusy());
    CHECK(dev.intrq());
    CHECK(dev.intrqDriven());
    // ...which device 0 doesn't show while it's selected
    host.select(0);
    CHECK(!dev.intrq());
    // and comes back with device 1
    host.select(1);
    CHECK(dev.intrq());
    host.reg(Reg::StatusCommand);  // acknowledged
    CHECK(!dev.intrq());
    // The disk's interrupt likewise
    host.select(0);
    host.command(ata::CMD_CHECK_POWER_MODE);
    CHECK(host.waitNotBusy());
    CHECK(dev.intrq());
    host.select(1);
    CHECK(!dev.intrq());
}

TEST(hdcd_device_reset_resets_the_cdrom_alone) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    std::vector<uint8_t> d;
    host.packet({mmc::OP_TEST_UNIT_READY}, d);  // the unit attention
    host.serve = false;  // the disc read stays BSY
    host.select(1);
    dev.write(Reg::LbaMid, 0xFE);
    dev.write(Reg::LbaHigh, 0xFF);
    host.command(atapi::CMD_PACKET);
    host.pump();
    const uint8_t p[12] = {mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1};
    for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    host.pump();
    CHECK(host.altStatus() & ata::ST_BSY);
    host.command(atapi::CMD_DEVICE_RESET);
    CHECK(host.waitNotBusy());
    // Device 1 is still the one selected, with its signature
    CHECK(dev.cdSelected());
    CHECK_EQ(host.signature(), 0xEB14);
    // Device 0 never noticed
    host.select(0);
    CHECK(host.altStatus() & ata::ST_DRDY);
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(hdcd_a_stale_cdrom_fetch_never_reaches_the_disk) {
    // The host gives up on a disc read (core 0 still fetching) and reads the
    // disk: the disc's late result must not complete the disk's fetch.
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    std::vector<uint8_t> d;
    host.packet({mmc::OP_TEST_UNIT_READY}, d);
    host.serve = false;
    host.select(1);
    dev.write(Reg::LbaMid, 0xFE);
    dev.write(Reg::LbaHigh, 0xFF);
    host.command(atapi::CMD_PACKET);
    host.pump();
    const uint8_t p[12] = {mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1};
    for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    host.pump();
    ata::Fetch stale;
    CHECK(dev.pendingFetch(stale));
    CHECK_EQ(stale.unit, 1);
    CHECK(stale.id & 0x80000000u);
    host.command(atapi::CMD_DEVICE_RESET);
    CHECK(host.waitNotBusy());
    // The disk's read, its fetch pending
    host.select(0);
    dev.write(Reg::SectorCount, 1);
    dev.write(Reg::LbaLow, 50);
    dev.write(Reg::LbaMid, 0);
    dev.write(Reg::LbaHigh, 0);
    dev.write(Reg::Device, 0xE0);
    host.command(ata::CMD_READ_SECTORS);
    host.pump();
    ata::Fetch f;
    CHECK(dev.pendingFetch(f));
    CHECK_EQ(f.unit, 0);
    dev.fetchDone(stale.id, true);  // the disc's, late: ignored by both
    CHECK(host.altStatus() & ata::ST_BSY);
    CHECK(dev.pendingFetch(f));
    host.serve = true;
    CHECK(host.waitNotBusy());
    std::vector<uint16_t> words;
    for (int w = 0; w < 256; ++w) words.push_back(dev.read(Reg::Data));
    CHECK(words == diskWords(50, 1));
}

TEST(hdcd_dma_on_either_device) {
    IdentifyInfo disk = DISK, disc = DISC;
    disk.mdma_mode = disc.mdma_mode = 2;
    Device dev(disk, nullptr, &disc);
    GeneratedDisk gen(disk.total_sectors);
    DiscSource source;
    Host host{dev, {&gen, &source}};
    std::vector<uint8_t> d;
    host.packet({mmc::OP_TEST_UNIT_READY}, d);
    auto runDma = [&](std::vector<uint16_t>& out) {
        out.clear();
        for (int guard = 0; guard < 100000; ++guard) {
            host.pump();
            ata::DmaBurst burst;
            if (dev.dmaBurstReady(burst)) {
                for (uint16_t i = 0; i < burst.count; ++i) out.push_back(burst.from[i]);
                dev.dmaBurstDone();
                continue;
            }
            if (!dev.dmaCommand() && !(host.altStatus() & ata::ST_BSY)) return host.reg(Reg::StatusCommand);
        }
        return uint8_t(0xFF);
    };
    std::vector<uint16_t> words;
    // READ DMA on device 0
    host.select(0);
    dev.write(Reg::SectorCount, 2);
    dev.write(Reg::LbaLow, 77);
    dev.write(Reg::LbaMid, 0);
    dev.write(Reg::LbaHigh, 0);
    dev.write(Reg::Device, 0xE0);
    host.command(ata::CMD_READ_DMA);
    CHECK(!(runDma(words) & ata::ST_ERR));
    CHECK(words == diskWords(77, 2));
    // A DMA packet on device 1
    host.select(1);
    dev.write(Reg::ErrorFeatures, atapi::FEATURES_DMA);
    host.command(atapi::CMD_PACKET);
    host.pump();
    const uint8_t p[12] = {mmc::OP_READ_10, 0, 0, 0, 0, 3, 0, 0, 1};
    for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    CHECK(!(runDma(words) & ata::ST_ERR));
    std::vector<uint8_t> bytes;
    for (uint16_t v : words) {
        bytes.push_back(uint8_t(v));
        bytes.push_back(uint8_t(v >> 8));
    }
    CHECK(bytes == discFrames(3, 1));
    CHECK(!dev.dmaCommand());
}

TEST(hdcd_without_a_cdrom_is_the_hard_disk_build) {
    // No CD-ROM: device 1 is a second disk, or none, as ata::Device has it.
    Device none(DISK, nullptr, nullptr);
    GeneratedDisk disk(DISK.total_sectors);
    Host host{none, {&disk, &disk}};
    CHECK(!none.hasCdrom());
    host.select(1);
    CHECK_EQ(host.altStatus(), 0x00);  // no device 1: Status 00h
    CHECK(!none.cdSelected());
    host.command(ata::CMD_IDENTIFY_DEVICE);
    host.pump();
    CHECK_EQ(none.commandsStarted(), 0u);  // not ours to run
    Device two(DISK, &DISK1, nullptr);
    Host host2{two, {&disk, &disk}};
    host2.select(1);
    host2.command(ata::CMD_IDENTIFY_DEVICE);
    CHECK(host2.waitNotBusy());
    CHECK(host2.reg(Reg::StatusCommand) & ata::ST_DRQ);
    uint16_t w[256];
    for (auto& word : w) word = two.read(Reg::Data);
    CHECK_EQ(uint32_t(w[60] | w[61] << 16), DISK1.total_sectors);
    CHECK_EQ(two.lastCommand().unit, 1);
}

TEST(hdcd_execute_device_diagnostic_runs_on_both) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    host.select(1);
    host.command(ata::CMD_EXECUTE_DEVICE_DIAGNOSTIC);
    CHECK(host.waitNotBusy());
    // Device 0 selected after it, reporting a pass; device 1 has its signature
    CHECK(!dev.cdSelected());
    CHECK_EQ(host.reg(Reg::ErrorFeatures), 0x01);
    CHECK_EQ(host.signature(), 0x0000);
    host.select(1);
    CHECK_EQ(host.signature(), 0xEB14);
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

TEST(hdcd_selecting_the_disk_while_the_cdrom_is_busy) {
    // The CD-ROM ignores the Device register while it fetches (BSY), so its own
    // copy still says device 1. The disk must answer from then on, and only
    // the disk may run the next command.
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    std::vector<uint8_t> d;
    host.packet({mmc::OP_TEST_UNIT_READY}, d);  // the unit attention
    host.serve = false;
    host.select(1);
    dev.write(Reg::LbaMid, 0xFE);
    dev.write(Reg::LbaHigh, 0xFF);
    host.command(atapi::CMD_PACKET);
    host.pump();
    const uint8_t p[12] = {mmc::OP_READ_10, 0, 0, 0, 0, 0, 0, 0, 1};
    for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    host.pump();
    CHECK(host.altStatus() & ata::ST_BSY);  // the CD-ROM, fetching
    uint32_t cd_started = dev.cdrom().commandsStarted();
    // The host gives up on it and selects the disk
    host.select(0);
    CHECK(!dev.cdSelected());
    CHECK(dev.cdrom().selected());  // its own copy lags, as on a real drive
    CHECK_EQ(host.altStatus(), ata::ST_DRDY | ata::ST_DSC);  // the disk's Status, not the CD-ROM's BSY
    host.command(ata::CMD_IDENTIFY_DEVICE);
    host.serve = true;
    CHECK(host.waitNotBusy());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_DRQ);
    uint16_t w[256];
    for (auto& word : w) word = dev.read(Reg::Data);
    CHECK_EQ(w[27], uint16_t('H' << 8 | 'D'));
    CHECK_EQ(w[0] & 0x8000, 0);  // the disk's IDENTIFY
    CHECK_EQ(dev.cdrom().commandsStarted(), cd_started);  // the CD-ROM ran nothing of it
    // It was busy when the IDENTIFY came, so it catches up at the next command
    host.command(ata::CMD_CHECK_POWER_MODE);
    CHECK(host.waitNotBusy());
    CHECK(!dev.cdrom().selected());
    CHECK_EQ(dev.cdrom().commandsStarted(), cd_started);
    // Back to the CD-ROM: a new packet works
    CHECK_EQ(host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 2, 0, 0, 1}, d), ata::ST_DRDY);
    CHECK(d == discFrames(2, 1));
}

TEST(hdcd_resets_select_device_0) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    // SRST written with device 1 selected
    host.select(1);
    dev.write(Reg::AltStatusDevCtrl, ata::DC_SRST);
    dev.write(Reg::AltStatusDevCtrl, 0);
    CHECK(!dev.cdSelected());
    CHECK_EQ(host.signature(), 0x0000);
    // RESET#
    host.select(1);
    dev.hardReset(true);
    dev.hardReset(false);
    CHECK(!dev.cdSelected());
    CHECK_EQ(host.signature(), 0x0000);
    // EXECUTE DEVICE DIAGNOSTIC, queued as core 1 does it
    host.select(1);
    dev.queueCommand(ata::CMD_EXECUTE_DEVICE_DIAGNOSTIC);
    dev.runQueuedCommand();
    CHECK(!dev.cdSelected());
    CHECK_EQ(host.signature(), 0x0000);
    host.select(1);
    CHECK_EQ(host.signature(), 0xEB14);
}

TEST(hdcd_changing_the_disc_leaves_the_disk_alone) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    std::vector<uint8_t> d;
    host.packet({mmc::OP_TEST_UNIT_READY}, d);
    CHECK(dev.discIdle());
    dev.changeDisc(0);
    CHECK_EQ(dev.cdrom().discFrames(), 0u);
    CHECK(host.packet({mmc::OP_TEST_UNIT_READY}, d) & ata::ST_ERR);  // no disc
    dev.changeDisc(DISC.total_sectors);
    CHECK(host.packet({mmc::OP_TEST_UNIT_READY}, d) & ata::ST_ERR);  // medium may have changed
    CHECK_EQ(host.packet({mmc::OP_TEST_UNIT_READY}, d), ata::ST_DRDY);
    std::vector<uint16_t> words;
    CHECK_EQ(host.readSectors(9, 1, words), ata::ST_DRDY | ata::ST_DSC);  // the disk never noticed
    CHECK(words == diskWords(9, 1));
    // No CD-ROM: nothing to change
    Device none(DISK, nullptr, nullptr);
    CHECK(!none.discIdle());
}

TEST(hdcd_a_cdrom_alone_as_device_0) {
    // device0 = cdrom: the CD-ROM is the only drive, its disc device 0's disk.
    Device dev(DISK, &DISK1, &DISC, 0);
    DiscSource disc;
    GeneratedDisk unused(DISK.total_sectors);
    Host host{dev, {&disc, &unused}};
    CHECK(dev.hasCdrom());
    CHECK_EQ(dev.cdUnit(), 0);
    // Device 0: the packet signature
    CHECK_EQ(host.signature(), 0xEB14);
    CHECK(dev.cdSelected());
    // IDENTIFY DEVICE aborted, IDENTIFY PACKET DEVICE answered
    host.command(ata::CMD_IDENTIFY_DEVICE);
    CHECK(host.waitNotBusy());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_ERR);
    host.command(atapi::CMD_IDENTIFY_PACKET_DEVICE);
    CHECK(host.waitNotBusy());
    CHECK(host.reg(Reg::StatusCommand) & ata::ST_DRQ);
    uint16_t w[256];
    for (auto& word : w) word = dev.read(Reg::Data);
    CHECK_EQ(w[0], 0x85C0);
    CHECK_EQ(dev.lastCommand().unit, 0);
    // Reads: from unit 0, the disc
    std::vector<uint8_t> d;
    host.packet({mmc::OP_TEST_UNIT_READY}, d, 0);  // the unit attention
    CHECK_EQ(host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 7, 0, 0, 2}, d, 0), ata::ST_DRDY);
    CHECK(d == discFrames(7, 2));
    CHECK_EQ(host.fetches[0], 8);
    CHECK_EQ(host.fetches[1], 0);
    CHECK_EQ(dev.lastCommand().unit, 0);
    CHECK_EQ(dev.lastCommand().packet, mmc::OP_READ_10);
    // No device 1, and no hard disk behind it: Status 00h, nothing runs
    host.select(1);
    CHECK_EQ(host.altStatus(), 0x00);
    uint32_t started = dev.commandsStarted();
    host.command(ata::CMD_IDENTIFY_DEVICE);
    host.pump();
    CHECK_EQ(dev.commandsStarted(), started);
    CHECK_EQ(dev.disk().commandsStarted(), 0u);  // the hard disk never saw a thing
    // A reset: the packet signature again, device 0 selected
    dev.hardReset(true);
    dev.hardReset(false);
    CHECK_EQ(host.signature(), 0xEB14);
    CHECK(dev.cdSelected());
    // A disc change, as console M does it
    CHECK(dev.discIdle());
    dev.changeDisc(0);
    CHECK(host.packet({mmc::OP_TEST_UNIT_READY}, d, 0) & ata::ST_ERR);  // no disc
    CHECK_EQ(dev.disk().commandsStarted(), 0u);
}

TEST(hdcd_a_cdrom_as_device_0_by_dma) {
    IdentifyInfo dma_disc = DISC;
    dma_disc.mdma_mode = 2;
    Device dev(DISK, nullptr, &dma_disc, 0);
    DiscSource disc;
    Host host{dev, {&disc, &disc}};
    std::vector<uint8_t> d;
    host.packet({mmc::OP_TEST_UNIT_READY}, d, 0);
    // READ (10) of one frame with PACKET's DMA bit: four bursts of a sector each
    uint8_t p[12] = {mmc::OP_READ_10, 0, 0, 0, 0, 3, 0, 0, 1};
    host.select(0);
    dev.write(Reg::ErrorFeatures, atapi::FEATURES_DMA);
    dev.write(Reg::LbaMid, 0xFE);
    dev.write(Reg::LbaHigh, 0xFF);
    host.command(atapi::CMD_PACKET);
    CHECK(host.waitNotBusy());
    for (unsigned i = 0; i < 6; ++i) dev.write(Reg::Data, uint16_t(p[2 * i] | p[2 * i + 1] << 8));
    CHECK(dev.dmaCommand());
    std::vector<uint8_t> got;
    for (int guard = 0; guard < 10000 && dev.dmaCommand(); ++guard) {
        host.pump();
        ata::DmaBurst burst;
        if (dev.dmaBurstReady(burst)) {
            for (unsigned i = 0; i < burst.count; ++i) {
                got.push_back(uint8_t(burst.from[i]));
                got.push_back(uint8_t(burst.from[i] >> 8));
            }
            dev.dmaBurstDone();
        }
    }
    CHECK(got == discFrames(3, 1));
    CHECK_EQ(host.fetches[0], 4);
    CHECK_EQ(host.fetches[1], 0);
}

// Writes to the disk with the CD-ROM beside it: the data goes to the disk
// alone, and the CD-ROM reads as before.
TEST(hdcd_disk_writes_with_a_cdrom_beside_it) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk base(DISK.total_sectors);
    RamOverlayDisk<8> disk(base);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    std::vector<uint8_t> d;
    host.packet({mmc::OP_TEST_UNIT_READY}, d);  // the unit attention
    const uint32_t lba = 2000;
    std::vector<uint16_t> written;
    for (int w = 0; w < 2 * 256; ++w) written.push_back(uint16_t(0xA500 + w));
    host.select(0);
    dev.write(Reg::SectorCount, 2);
    dev.write(Reg::LbaLow, lba & 0xFF);
    dev.write(Reg::LbaMid, (lba >> 8) & 0xFF);
    dev.write(Reg::LbaHigh, 0);
    dev.write(Reg::Device, 0xE0);
    host.command(ata::CMD_WRITE_SECTORS);
    for (int s = 0; s < 2; ++s) {
        CHECK(host.waitNotBusy());
        CHECK(host.altStatus() & ata::ST_DRQ);
        for (int w = 0; w < 256; ++w) dev.write(Reg::Data, written[size_t(s * 256 + w)]);
    }
    CHECK(host.waitNotBusy());
    CHECK_EQ(host.reg(Reg::StatusCommand), ata::ST_DRDY | ata::ST_DSC);
    std::vector<uint16_t> words;
    CHECK_EQ(host.readSectors(lba, 2, words), ata::ST_DRDY | ata::ST_DSC);
    CHECK(words == written);
    CHECK_EQ(host.fetches[1], 0);  // the CD-ROM saw none of it
    CHECK_EQ(host.packet({mmc::OP_READ_10, 0, 0, 0, 0, 7, 0, 0, 1}, d), ata::ST_DRDY);
    CHECK(d == discFrames(7, 1));
    CHECK_EQ(dev.commandsStarted(), dev.commandsCompleted());
}

// Both devices on a cable take every task-file register write, whichever is
// selected, and keep it while the other is selected.
TEST(hdcd_task_file_writes_reach_both_devices) {
    Device dev(DISK, nullptr, &DISC);
    GeneratedDisk disk(DISK.total_sectors);
    DiscSource disc;
    Host host{dev, {&disk, &disc}};
    host.select(1);
    dev.write(Reg::SectorCount, 0x12);
    dev.write(Reg::LbaLow, 0x34);
    dev.write(Reg::LbaMid, 0x56);
    dev.write(Reg::LbaHigh, 0x78);
    CHECK_EQ(host.reg(Reg::SectorCount), 0x12);
    CHECK_EQ(host.reg(Reg::LbaLow), 0x34);
    host.select(0);
    CHECK_EQ(host.reg(Reg::SectorCount), 0x12);
    CHECK_EQ(host.reg(Reg::LbaLow), 0x34);
    CHECK_EQ(host.reg(Reg::LbaMid), 0x56);
    CHECK_EQ(host.reg(Reg::LbaHigh), 0x78);
    dev.write(Reg::LbaLow, 0x9A);  // device 0 selected: device 1 takes it too
    host.select(1);
    CHECK_EQ(host.reg(Reg::LbaLow), 0x9A);
    CHECK_EQ(host.reg(Reg::LbaHigh), 0x78);
    // A busy device ignores them: the CD-ROM mid-packet, the disk still takes it
    dev.write(Reg::ErrorFeatures, 0);
    dev.write(Reg::LbaMid, 0xFE);
    dev.write(Reg::LbaHigh, 0xFF);
    host.command(atapi::CMD_PACKET);
    CHECK(host.waitNotBusy());
    CHECK(host.altStatus() & ata::ST_DRQ);  // waiting for the packet: not busy, so it takes writes
    host.select(0);
    dev.write(Reg::LbaLow, 0x21);
    CHECK_EQ(host.reg(Reg::LbaLow), 0x21);
}

void runHdcdTests() {
    RUN(hdcd_each_device_shows_its_signature);
    RUN(hdcd_a_bios_probe_finds_a_disk_and_a_cdrom);
    RUN(hdcd_reads_from_both_interleaved);
    RUN(hdcd_intrq_follows_the_selected_device);
    RUN(hdcd_device_reset_resets_the_cdrom_alone);
    RUN(hdcd_a_stale_cdrom_fetch_never_reaches_the_disk);
    RUN(hdcd_dma_on_either_device);
    RUN(hdcd_without_a_cdrom_is_the_hard_disk_build);
    RUN(hdcd_execute_device_diagnostic_runs_on_both);
    RUN(hdcd_selecting_the_disk_while_the_cdrom_is_busy);
    RUN(hdcd_resets_select_device_0);
    RUN(hdcd_changing_the_disc_leaves_the_disk_alone);
    RUN(hdcd_a_cdrom_alone_as_device_0);
    RUN(hdcd_a_cdrom_as_device_0_by_dma);
    RUN(hdcd_disk_writes_with_a_cdrom_beside_it);
    RUN(hdcd_task_file_writes_reach_both_devices);
}
