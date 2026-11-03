// Tests for two drives on one cable (docs/two-drives.md): ata::Device with a
// device 1, each unit's own state swapped in when the host selects it, and a
// virtual host that probes both as a BIOS does, with a disk per unit.
#include <cstdint>
#include <cstring>
#include <vector>

#include "ata_device.h"
#include "block_source.h"
#include "check.h"
#include "identify.h"

using namespace ata;

namespace {

const IdentifyInfo DEVICE0{"HDDSYNTH PROTO", "HSP-000001", "0.29.0", 131072};
const IdentifyInfo DEVICE1{"HDDSYNTH PRO D1", "HSP-000001-D1", "0.29.0", 2048};

// A small disk whose every word names its unit and sector: word i of
// sector n on unit u is (u << 15) | (n << 4 & 0x7FF0) | (i & 0xF).
class TaggedDisk : public BlockSource {
public:
    TaggedDisk(uint16_t unit, uint32_t sectors, bool writable)
        : unit_(unit), sectors_(sectors), writable_(writable) {}
    uint32_t sectors() const override { return sectors_; }
    bool read(uint32_t lba, uint16_t words[256]) override {
        if (lba >= sectors_) return false;
        for (unsigned i = 0; i < 256; ++i) words[i] = word(lba, i);
        for (const auto& w : written_)
            if (w.lba == lba) std::memcpy(words, w.words, sizeof w.words);
        return true;
    }
    bool write(uint32_t lba, const uint16_t words[256]) override {
        if (!writable_ || lba >= sectors_) return false;
        Written w{lba, {}};
        std::memcpy(w.words, words, sizeof w.words);
        written_.push_back(w);
        return true;
    }
    uint16_t word(uint32_t lba, unsigned i) const {
        return uint16_t((unit_ << 15) | ((lba << 4) & 0x7FF0) | (i & 0xF));
    }
    size_t writes() const { return written_.size(); }

private:
    struct Written {
        uint32_t lba;
        uint16_t words[256];
    };
    uint16_t unit_;
    uint32_t sectors_;
    bool writable_;
    std::vector<Written> written_;
};

// The host, and core 0 serving fetches from each unit's disk.
struct TwoHost {
    Device& dev;
    BlockSource* disks[2];
    int fetches[2] = {0, 0};

    void pump() {
        Fetch f;
        if (dev.dataOutHeld()) dev.releaseDataOut();
        if (dev.pendingFetch(f)) {
            BlockSource* disk = disks[f.unit];
            ++fetches[f.unit];
            bool ok = f.store ? disk->write(f.lba, f.buffer) : f.flush ? disk->flush() : disk->read(f.lba, f.buffer);
            dev.fetchDone(f.id, ok);
        }
    }
    uint8_t status() { return uint8_t(dev.read(Reg::StatusCommand)); }
    uint8_t altStatus() { return uint8_t(dev.read(Reg::AltStatusDevCtrl)); }
    void select(int unit) { dev.write(Reg::Device, unit ? 0xB0 : 0xA0); }
    bool waitNotBusy() {
        for (int i = 0; i < 1000; ++i) {
            if (!(altStatus() & ST_BSY)) return true;
            pump();
        }
        return false;
    }
    bool signature() {
        return dev.read(Reg::SectorCount) == 1 && dev.read(Reg::LbaLow) == 1 && dev.read(Reg::LbaMid) == 0 &&
               dev.read(Reg::LbaHigh) == 0;
    }
    void setLba(int unit, uint32_t lba, uint8_t count) {
        dev.write(Reg::SectorCount, count);
        dev.write(Reg::LbaLow, lba & 0xFF);
        dev.write(Reg::LbaMid, (lba >> 8) & 0xFF);
        dev.write(Reg::LbaHigh, (lba >> 16) & 0xFF);
        dev.write(Reg::Device, (unit ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    }
    bool identify(int unit, uint16_t out[256]) {
        select(unit);
        if (!waitNotBusy()) return false;
        dev.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
        if (!waitNotBusy()) return false;
        uint8_t st = status();
        if ((st & ST_ERR) || !(st & ST_DRQ)) return false;
        for (int i = 0; i < 256; ++i) out[i] = dev.read(Reg::Data);
        return true;
    }
    bool read(int unit, uint32_t lba, uint8_t count, std::vector<uint16_t>& out) {
        setLba(unit, lba, count);
        dev.write(Reg::StatusCommand, CMD_READ_SECTORS);
        out.clear();
        for (int s = 0; s < count; ++s) {
            if (!waitNotBusy()) return false;
            uint8_t st = status();
            if ((st & ST_ERR) || !(st & ST_DRQ)) return false;
            for (int i = 0; i < 256; ++i) out.push_back(dev.read(Reg::Data));
        }
        if (!waitNotBusy()) return false;
        return !(altStatus() & (ST_ERR | ST_DRQ));
    }
    bool write(int unit, uint32_t lba, uint16_t fill) {
        setLba(unit, lba, 1);
        dev.write(Reg::StatusCommand, CMD_WRITE_SECTORS);
        if (!waitNotBusy()) return false;
        uint8_t st = status();
        if ((st & ST_ERR) || !(st & ST_DRQ)) return false;
        for (int i = 0; i < 256; ++i) dev.write(Reg::Data, fill);
        if (!waitNotBusy()) return false;
        return !(status() & (ST_ERR | ST_DRQ));
    }
    void command(int unit, uint8_t cmd) {
        select(unit);
        dev.write(Reg::StatusCommand, cmd);
        waitNotBusy();
    }
};

std::string identifyString(const uint16_t* id, unsigned first, unsigned words) {
    std::string s;
    for (unsigned i = 0; i < words; ++i) {
        s += char(id[first + i] >> 8);
        s += char(id[first + i] & 0xFF);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

}  // namespace

TEST(test_two_drives_both_present_after_power_on) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    CHECK(d.hasDevice1());
    for (int unit = 0; unit < 2; ++unit) {
        h.select(unit);
        CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
        CHECK_EQ(d.read(Reg::ErrorFeatures), 0x01);  // diagnostics passed
        CHECK(h.signature());
        CHECK_EQ(d.unit(), unit);
    }
}

TEST(test_two_drives_identify_per_unit) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    uint16_t id0[256], id1[256];
    CHECK(h.identify(0, id0));
    CHECK(h.identify(1, id1));
    CHECK_STR(identifyString(id0, 27, 20), "HDDSYNTH PROTO");
    CHECK_STR(identifyString(id1, 27, 20), "HDDSYNTH PRO D1");
    CHECK_STR(identifyString(id1, 10, 10), "HSP-000001-D1");
    CHECK_EQ(uint32_t(id0[60]) | (uint32_t(id0[61]) << 16), 131072u);
    CHECK_EQ(uint32_t(id1[60]) | (uint32_t(id1[61]) << 16), 2048u);
}

TEST(test_two_drives_reads_go_to_the_selected_unit) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    std::vector<uint16_t> out;
    CHECK(h.read(1, 100, 3, out));
    CHECK_EQ(out.size(), 768u);
    CHECK_EQ(out[0], s.word(100, 0));
    CHECK_EQ(out[767], s.word(102, 255));
    CHECK(h.read(0, 100, 2, out));
    CHECK_EQ(out[0], m.word(100, 0));
    CHECK_EQ(out[511], m.word(101, 255));
    CHECK(h.fetches[0] >= 2);
    CHECK(h.fetches[1] >= 3);
    // Each unit's own size: LBA 3000 exists only on device 0.
    CHECK(h.read(0, 3000, 1, out));
    CHECK(!h.read(1, 3000, 1, out));
}

TEST(test_two_drives_status_and_error_are_per_unit) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    std::vector<uint16_t> out;
    CHECK(!h.read(1, 5000, 1, out));  // past device 1's end: IDNF
    CHECK(h.altStatus() & ST_ERR);
    CHECK_EQ(d.read(Reg::ErrorFeatures), ER_IDNF);
    h.select(0);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);  // device 0 didn't fail
    CHECK_EQ(d.read(Reg::ErrorFeatures), 0x01);
    h.select(1);
    CHECK(h.altStatus() & ST_ERR);  // device 1's error is still there
    CHECK_EQ(d.read(Reg::ErrorFeatures), ER_IDNF);
}

TEST(test_two_drives_interrupt_belongs_to_its_unit) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    h.select(1);
    d.write(Reg::AltStatusDevCtrl, 0);  // nIEN clear
    d.write(Reg::StatusCommand, CMD_IDLE_IMMEDIATE);
    h.waitNotBusy();
    CHECK(d.intrq());
    CHECK(d.intrqDriven());
    h.select(0);
    CHECK(!d.intrq());  // device 0 has none pending
    CHECK(d.intrqDriven());  // but it drives the line, being selected and present
    h.select(1);
    CHECK(d.intrq());
    h.status();  // reading Status acknowledges it
    CHECK(!d.intrq());
}

TEST(test_two_drives_soft_reset_selects_device_0) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    std::vector<uint16_t> out;
    CHECK(!h.read(1, 5000, 1, out));
    d.write(Reg::AltStatusDevCtrl, DC_SRST);
    d.write(Reg::AltStatusDevCtrl, 0);
    CHECK_EQ(d.unit(), 0);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK(h.signature());
    h.select(1);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);  // device 1's error is cleared too
    CHECK_EQ(d.read(Reg::ErrorFeatures), 0x01);
}

TEST(test_two_drives_diagnostic_from_device_1) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    h.command(1, CMD_EXECUTE_DEVICE_DIAGNOSTIC);
    CHECK_EQ(d.unit(), 0);  // the signature selects device 0
    CHECK_EQ(d.read(Reg::ErrorFeatures), 0x01);
    CHECK(h.signature());
}

TEST(test_two_drives_geometry_per_unit) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    // INITIALIZE DEVICE PARAMETERS on device 1 only: 4 heads, 32 sectors.
    d.write(Reg::SectorCount, 32);
    d.write(Reg::Device, 0xB0 | 3);
    d.write(Reg::StatusCommand, CMD_INITIALIZE_DEVICE_PARAMETERS);
    h.waitNotBusy();
    CHECK_EQ(d.geometry().heads, 4u);
    CHECK_EQ(d.geometry().sectors_per_track, 32u);
    h.select(0);
    CHECK_EQ(d.geometry().heads, 16u);  // device 0's default, untouched
    d.hardReset(true);
    d.hardReset(false);
    h.select(1);
    CHECK(d.geometry().heads != 4u);  // a hardware reset restores device 1's too
}

TEST(test_two_drives_standby_per_unit) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    h.command(1, CMD_STANDBY_IMMEDIATE);
    h.command(0, CMD_CHECK_POWER_MODE);
    CHECK_EQ(d.read(Reg::SectorCount), POWER_ACTIVE_OR_IDLE);
    h.command(1, CMD_CHECK_POWER_MODE);
    CHECK_EQ(d.read(Reg::SectorCount), POWER_STANDBY);
}

TEST(test_two_drives_select_ignored_mid_transfer) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    h.select(1);
    d.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
    CHECK(h.altStatus() & ST_DRQ);
    h.select(0);  // forbidden mid-transfer: device 1 keeps the bus
    CHECK_EQ(d.unit(), 1);
    uint16_t first = d.read(Reg::Data);
    CHECK_EQ(first, 0x0040);  // IDENTIFY word 0: fixed disk
}

TEST(test_two_drives_writes_refused_by_a_read_only_unit) {
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    CHECK(h.write(0, 10, 0x1234));
    CHECK_EQ(m.writes(), 1u);
    CHECK(!h.write(1, 10, 0x1234));  // the folder drive refuses writes
    CHECK_EQ(s.writes(), 0u);
    CHECK(h.altStatus() & ST_ERR);
    CHECK_EQ(d.read(Reg::ErrorFeatures) & ER_ABRT, ER_ABRT);
    h.select(0);
    CHECK(!(h.altStatus() & ST_ERR));
}

TEST(test_two_drives_bios_probe) {
    // A BIOS's detection after power-on: reset, then for each device select
    // it, wait for BSY to clear, check the registers echo, IDENTIFY, and read
    // the partition table. Both units answer as drives.
    Device d(DEVICE0, &DEVICE1);
    TaggedDisk m(0, 131072, true), s(1, 2048, false);
    TwoHost h{d, {&m, &s}};
    d.hardReset(true);
    d.hardReset(false);
    int found = 0;
    for (int unit = 0; unit < 2; ++unit) {
        h.select(unit);
        if (!h.waitNotBusy() || h.altStatus() == 0) continue;
        d.write(Reg::SectorCount, 0x55);
        d.write(Reg::LbaLow, 0xAA);
        if (d.read(Reg::SectorCount) != 0x55 || d.read(Reg::LbaLow) != 0xAA) continue;
        uint16_t id[256];
        if (!h.identify(unit, id)) continue;
        std::vector<uint16_t> mbr;
        if (!h.read(unit, 0, 1, mbr)) continue;
        CHECK_EQ(mbr[0], unit ? s.word(0, 0) : m.word(0, 0));
        ++found;
    }
    CHECK_EQ(found, 2);
}

TEST(test_device_0_alone_still_answers_for_device_1) {
    // No device 1: unchanged, device 0 answers 00h for it and refuses its commands.
    Device d(DEVICE0);
    TaggedDisk m(0, 131072, true);
    TwoHost h{d, {&m, &m}};
    CHECK(!d.hasDevice1());
    h.select(1);
    CHECK_EQ(h.altStatus(), 0);
    d.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
    CHECK_EQ(h.altStatus(), 0);
    CHECK(!d.intrqDriven());
    h.select(0);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    std::vector<uint16_t> out;
    CHECK(h.read(0, 7, 1, out));
    CHECK_EQ(h.fetches[1], 0);
}

void runTwoDrivesTests() {
    RUN(test_two_drives_both_present_after_power_on);
    RUN(test_two_drives_identify_per_unit);
    RUN(test_two_drives_reads_go_to_the_selected_unit);
    RUN(test_two_drives_status_and_error_are_per_unit);
    RUN(test_two_drives_interrupt_belongs_to_its_unit);
    RUN(test_two_drives_soft_reset_selects_device_0);
    RUN(test_two_drives_diagnostic_from_device_1);
    RUN(test_two_drives_geometry_per_unit);
    RUN(test_two_drives_standby_per_unit);
    RUN(test_two_drives_select_ignored_mid_transfer);
    RUN(test_two_drives_writes_refused_by_a_read_only_unit);
    RUN(test_two_drives_bios_probe);
    RUN(test_device_0_alone_still_answers_for_device_1);
}
