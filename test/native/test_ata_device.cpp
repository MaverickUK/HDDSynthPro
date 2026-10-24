// Tests for the ATA register file and command state machine (ata_device),
// including a "virtual host" that plays a typical start-up probe.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "ata_device.h"
#include "block_source.h"
#include "check.h"
#include "fetch_mailbox.h"
#include "identify.h"

using namespace ata;

static const IdentifyInfo INFO{"HDDSYNTH PROTO", "HSP-000001", "0.2.0", 131072};

// ---- A minimal virtual host, as a host driver would talk to the drive ----

struct VirtualHost {
    Device& dev;
    BlockSource* source = nullptr;  // stands in for core 0: serves fetches when polled
    uint32_t polls = 0;

    // Core 0's share: fetch (or store) whatever the device is waiting for.
    void pump() {
        Fetch f;
        if (source && dev.pendingFetch(f))
            dev.fetchDone(f.id, f.store ? source->write(f.lba, f.buffer) : source->read(f.lba, f.buffer));
    }

    uint8_t status() { return uint8_t(dev.read(Reg::StatusCommand)); }
    uint8_t altStatus() { return uint8_t(dev.read(Reg::AltStatusDevCtrl)); }
    void select(int device) { dev.write(Reg::Device, device ? 0xB0 : 0xA0); }

    // Polls Alternate Status until BSY clears (no side effects), like a host
    // waiting for a command. Returns false if it never clears.
    bool waitNotBusy(int max_polls = 1000) {
        for (int i = 0; i < max_polls; ++i, ++polls) {
            if (!(altStatus() & ST_BSY)) return true;
            pump();
        }
        return false;
    }

    void softReset() {
        dev.write(Reg::AltStatusDevCtrl, DC_SRST | DC_NIEN);
        dev.write(Reg::AltStatusDevCtrl, DC_NIEN);
    }

    // Register echo: write two registers, read them back. Hosts use this to
    // see whether anything is on the bus.
    bool echo(uint8_t a, uint8_t b) {
        dev.write(Reg::SectorCount, a);
        dev.write(Reg::LbaLow, b);
        return dev.read(Reg::SectorCount) == a && dev.read(Reg::LbaLow) == b;
    }

    bool ataSignature() {
        return dev.read(Reg::SectorCount) == 1 && dev.read(Reg::LbaLow) == 1 &&
               dev.read(Reg::LbaMid) == 0 && dev.read(Reg::LbaHigh) == 0;
    }

    void setLba(uint32_t lba, uint8_t count) {
        dev.write(Reg::SectorCount, count);
        dev.write(Reg::LbaLow, lba & 0xFF);
        dev.write(Reg::LbaMid, (lba >> 8) & 0xFF);
        dev.write(Reg::LbaHigh, (lba >> 16) & 0xFF);
        dev.write(Reg::Device, 0xE0 | ((lba >> 24) & 0x0F));  // LBA, device 0
    }

    // READ SECTORS the polled way, one DRQ block per sector. Returns false
    // on an error or a missing data phase; `out` gets the words read.
    bool readSectors(uint32_t lba, uint8_t count, std::vector<uint16_t>& out) {
        setLba(lba, count);
        return readAfterCommand(count ? count : 256, out);
    }

    bool readAfterCommand(int sectors, std::vector<uint16_t>& out) {
        dev.write(Reg::StatusCommand, CMD_READ_SECTORS);
        out.clear();
        for (int s = 0; s < sectors; ++s) {
            if (!waitNotBusy()) return false;
            uint8_t st = status();
            if ((st & ST_ERR) || !(st & ST_DRQ)) return false;
            for (int i = 0; i < 256; ++i) out.push_back(dev.read(Reg::Data));
        }
        if (!waitNotBusy()) return false;
        uint8_t st = altStatus();
        return !(st & (ST_ERR | ST_DRQ));
    }

    // WRITE SECTORS the polled way, one DRQ block per sector; `words` holds
    // 256 per sector. Returns false on an error or a missing data phase.
    bool writeSectors(uint32_t lba, uint8_t count, const std::vector<uint16_t>& words) {
        setLba(lba, count);
        dev.write(Reg::StatusCommand, CMD_WRITE_SECTORS);
        int sectors = count ? count : 256;
        for (int s = 0; s < sectors; ++s) {
            if (!waitNotBusy()) return false;
            uint8_t st = status();
            if ((st & ST_ERR) || !(st & ST_DRQ)) return false;
            for (int i = 0; i < 256; ++i) dev.write(Reg::Data, words[size_t(s) * 256 + size_t(i)]);
        }
        if (!waitNotBusy()) return false;
        uint8_t st = status();
        return !(st & (ST_ERR | ST_DRQ));
    }

    // IDENTIFY DEVICE the polled way: returns false on error or no data.
    bool identify(uint16_t out[256]) {
        dev.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
        if (!waitNotBusy()) return false;
        uint8_t st = status();  // also acknowledges the interrupt
        if ((st & ST_ERR) || !(st & ST_DRQ)) return false;
        for (int i = 0; i < 256; ++i) out[i] = dev.read(Reg::Data);
        return true;
    }
};

// ---- Register mapping ------------------------------------------------------

TEST(test_reg_mapping) {
    CHECK(regFor(true, false, 0) == Reg::Data);
    CHECK(regFor(true, false, 7) == Reg::StatusCommand);
    CHECK(regFor(false, true, 6) == Reg::AltStatusDevCtrl);
    CHECK(regFor(false, true, 7) == Reg::None);  // obsolete Device Address
    CHECK(regFor(false, false, 7) == Reg::None);
    CHECK(regFor(true, true, 7) == Reg::None);
}

// ---- Reset -----------------------------------------------------------------

TEST(test_power_on_state) {
    Device d(INFO);
    VirtualHost h{d};
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);
    CHECK(h.ataSignature());
    CHECK_EQ(d.read(Reg::ErrorFeatures), 0x01);
    CHECK(!d.intrq());
}

TEST(test_hard_reset) {
    Device d(INFO);
    VirtualHost h{d};
    h.echo(0x55, 0xAA);
    d.hardReset(true);
    CHECK(h.status() & ST_BSY);
    d.write(Reg::SectorCount, 0x12);  // ignored while busy
    d.hardReset(false);
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);
    CHECK(h.ataSignature());
}

TEST(test_soft_reset) {
    Device d(INFO);
    VirtualHost h{d};
    h.echo(0x55, 0xAA);
    d.write(Reg::AltStatusDevCtrl, DC_SRST);
    CHECK(h.altStatus() & ST_BSY);
    d.write(Reg::AltStatusDevCtrl, 0);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK(h.ataSignature());
}

TEST(test_soft_reset_interrupts_a_transfer) {
    Device d(INFO);
    VirtualHost h{d};
    d.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
    CHECK(h.altStatus() & ST_DRQ);
    h.softReset();
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK_EQ(d.read(Reg::Data), 0);  // nothing left to read
}

// ---- Registers -------------------------------------------------------------

TEST(test_register_echo) {
    Device d(INFO);
    VirtualHost h{d};
    CHECK(h.echo(0x55, 0xAA));
    CHECK(h.echo(0xAA, 0x55));
    d.write(Reg::LbaMid, 0x34);
    d.write(Reg::LbaHigh, 0x12);
    CHECK_EQ(d.read(Reg::LbaMid), 0x34);
    CHECK_EQ(d.read(Reg::LbaHigh), 0x12);
    d.write(Reg::LbaLow, 0x1FF);  // only the low byte is a register
    CHECK_EQ(d.read(Reg::LbaLow), 0xFF);
}

TEST(test_absent_device1) {
    Device d(INFO);
    VirtualHost h{d};
    h.select(1);
    // Device 0 answers for the absent device 1: status 00h means "nothing here",
    CHECK_EQ(h.status(), 0);
    CHECK_EQ(h.altStatus(), 0);
    // but the shared task-file registers still echo.
    CHECK(h.echo(0x55, 0xAA));
    // Commands for device 1 are not run.
    uint32_t before = d.commandsStarted();
    d.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
    CHECK_EQ(d.commandsStarted(), before);
    CHECK_EQ(d.read(Reg::Data), 0);
    h.select(0);
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);  // no DRQ: the IDENTIFY never ran
}

// ---- Commands --------------------------------------------------------------

TEST(test_identify_transfer) {
    Device d(INFO);
    VirtualHost h{d};
    uint16_t got[256], want[256];
    buildIdentify(INFO, want);
    CHECK(h.identify(got));
    bool same = true;
    for (int i = 0; i < 256; ++i) same = same && got[i] == want[i];
    CHECK(same);
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);  // DRQ clears after the last word
    CHECK_EQ(d.read(Reg::Data), 0);          // and extra reads get nothing
    CHECK_EQ(d.lastCommand().command, CMD_IDENTIFY_DEVICE);
}

TEST(test_identify_twice) {
    Device d(INFO);
    VirtualHost h{d};
    uint16_t a[256], b[256];
    CHECK(h.identify(a));
    CHECK(h.identify(b));
    CHECK_EQ(a[60], b[60]);
    CHECK_EQ(a[255], b[255]);
}

TEST(test_unknown_command_aborts) {
    Device d(INFO);
    VirtualHost h{d};
    d.write(Reg::StatusCommand, 0xA1);  // IDENTIFY PACKET DEVICE: we're not ATAPI
    uint8_t st = h.status();
    CHECK(st & ST_ERR);
    CHECK(st & ST_DRDY);
    CHECK(!(st & ST_DRQ));
    CHECK_EQ(d.read(Reg::ErrorFeatures), ER_ABRT);
    // The next command clears the error.
    uint16_t id[256];
    CHECK(h.identify(id));
    CHECK_EQ(d.read(Reg::ErrorFeatures), 0);
}

TEST(test_execute_device_diagnostic) {
    Device d(INFO);
    VirtualHost h{d};
    h.echo(0x55, 0xAA);
    d.write(Reg::StatusCommand, CMD_EXECUTE_DEVICE_DIAGNOSTIC);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK(h.ataSignature());
    CHECK_EQ(d.read(Reg::ErrorFeatures), 0x01);  // passed, no device 1
    CHECK(d.intrq());
}

TEST(test_diagnostic_runs_with_device1_selected) {
    Device d(INFO);
    VirtualHost h{d};
    h.select(1);
    d.write(Reg::StatusCommand, CMD_EXECUTE_DEVICE_DIAGNOSTIC);
    CHECK_EQ(d.commandsStarted(), 1u);
    CHECK_EQ(d.read(Reg::Device) & DEV_DEV, 0);  // device 0 selected again
}

TEST(test_command_ignored_while_busy) {
    Device d(INFO);
    d.hardReset(true);
    d.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
    d.hardReset(false);
    CHECK_EQ(d.commandsStarted(), 0u);
}

// ---- Interrupts ------------------------------------------------------------

TEST(test_intrq_cleared_by_status_not_alt_status) {
    Device d(INFO);
    VirtualHost h{d};
    d.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
    CHECK(d.intrq());
    h.altStatus();
    CHECK(d.intrq());  // Alternate Status has no side effects
    h.status();
    CHECK(!d.intrq());
}

TEST(test_intrq_masked_by_nien_and_device1) {
    Device d(INFO);
    VirtualHost h{d};
    d.write(Reg::AltStatusDevCtrl, DC_NIEN);
    d.write(Reg::StatusCommand, 0xFF);  // aborts, raises an interrupt
    CHECK(!d.intrq());
    d.write(Reg::AltStatusDevCtrl, 0);
    CHECK(d.intrq());  // still pending once unmasked
    h.select(1);
    CHECK(!d.intrq());  // INTRQ belongs to the selected device
    h.select(0);
    CHECK(d.intrq());
}

TEST(test_intrq_cleared_by_new_command) {
    Device d(INFO);
    d.write(Reg::StatusCommand, 0xFF);
    CHECK(d.intrq());
    d.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
    CHECK(d.intrq());  // the new command's own interrupt
    CHECK_EQ(d.error(), 0);
}

// ---- Virtual host: a typical start-up probe --------------------------------

// The shape of what Linux's libata and most USB-IDE bridges do: reset, check
// for each device with a register echo, read the signature, then IDENTIFY.
// Once the slice 0 trace shows what this adapter really does, its sequence
// gets its own test alongside this one.
TEST(test_virtual_host_probe) {
    Device d(INFO);
    VirtualHost h{d};

    d.hardReset(true);
    d.hardReset(false);

    // Device 0: present?
    h.select(0);
    CHECK(h.echo(0x55, 0xAA));
    CHECK(h.echo(0xAA, 0x55));

    // Device 1: the echo passes too (shared registers), but status reads 00h.
    h.select(1);
    CHECK(h.echo(0x55, 0xAA));
    CHECK_EQ(h.status(), 0);

    // Soft reset, then classify both devices by signature.
    h.select(0);
    h.softReset();
    CHECK(h.waitNotBusy());
    CHECK(h.ataSignature());
    CHECK_EQ(h.status() & (ST_BSY | ST_DRDY), ST_DRDY);
    h.select(1);
    CHECK_EQ(h.status(), 0);  // no device 1

    // IDENTIFY device 0.
    h.select(0);
    uint16_t id[256];
    CHECK(h.identify(id));
    uint32_t sectors = uint32_t(id[60]) | (uint32_t(id[61]) << 16);
    CHECK_EQ(sectors, 131072u);
    CHECK_EQ(id[0], 0x0040);
}


// ---- READ SECTORS ------------------------------------------------------------

static bool sameAsDisk(const std::vector<uint16_t>& got, uint32_t lba, uint32_t sectors) {
    GeneratedDisk disk;
    uint16_t want[256];
    if (got.size() != sectors * 256u) return false;
    for (uint32_t s = 0; s < sectors; ++s) {
        disk.read(lba + s, want);
        for (int i = 0; i < 256; ++i)
            if (got[s * 256 + i] != want[i]) return false;
    }
    return true;
}

TEST(test_generated_disk_words) {
    // The first word on the bus is the sector's first two bytes, low byte first.
    GeneratedDisk disk;
    uint16_t w[256];
    CHECK(disk.read(12345, w));
    CHECK_EQ(w[0], 0x3039);  // bytes 39 30: LBA 12345 little-endian
    CHECK(!disk.read(disk.sectors(), w));
}

TEST(test_read_one_sector) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    std::vector<uint16_t> got;
    CHECK(h.readSectors(0, 1, got));
    CHECK(sameAsDisk(got, 0, 1));
    CHECK_EQ(d.lastCommand().command, CMD_READ_SECTORS);
    CHECK_EQ(d.lastCommand().lba, 0u);
    CHECK_EQ(d.lastCommand().count, 1);
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
}

TEST(test_read_busy_until_fetched) {
    Device d(INFO);
    VirtualHost h{d};  // no source: nothing serves the fetch yet
    h.setLba(500, 1);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(h.altStatus() & ST_BSY);
    CHECK(!(h.altStatus() & ST_DRQ));
    CHECK_EQ(d.read(Reg::Data), 0);
    Fetch f;
    CHECK(d.pendingFetch(f));
    CHECK_EQ(f.lba, 500u);
    CHECK(!d.intrq());
    GeneratedDisk disk;
    d.fetchDone(f.id, disk.read(f.lba, f.buffer));
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);
    CHECK(d.intrq());  // one interrupt per DRQ block
    CHECK(!d.pendingFetch(f));
}

TEST(test_read_multiple_sectors_interrupt_each) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d};
    h.setLba(1000, 3);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    std::vector<uint16_t> got;
    for (int s = 0; s < 3; ++s) {
        Fetch f;
        CHECK(d.pendingFetch(f));
        CHECK_EQ(f.lba, 1000u + s);
        d.fetchDone(f.id, disk.read(f.lba, f.buffer));
        CHECK(d.intrq());
        h.status();  // acknowledge
        CHECK(!d.intrq());
        for (int i = 0; i < 256; ++i) got.push_back(d.read(Reg::Data));
    }
    CHECK(sameAsDisk(got, 1000, 3));
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);
    CHECK(!d.intrq());  // no interrupt after the last block of a read
}

TEST(test_read_count_zero_means_256) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    std::vector<uint16_t> got;
    CHECK(h.readSectors(70000, 0, got));
    CHECK_EQ(got.size(), 256u * 256u);
    CHECK(sameAsDisk(got, 70000, 256));
    CHECK_EQ(d.lastCommand().count, 256);
}

TEST(test_read_last_sector_and_past_the_end) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    std::vector<uint16_t> got;
    CHECK(h.readSectors(131071, 1, got));
    CHECK(sameAsDisk(got, 131071, 1));

    Fetch f;
    h.setLba(131072, 1);  // one past the end
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(h.status() & ST_ERR);
    CHECK_EQ(d.error(), ER_IDNF);
    CHECK(!d.pendingFetch(f));  // nothing fetched, nothing transferred

    h.setLba(131070, 3);  // straddles the end: refused before any data
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(h.status() & ST_ERR);
    CHECK_EQ(d.error(), ER_IDNF);
    CHECK(!d.pendingFetch(f));
    // The task file names the first address that doesn't exist.
    CHECK_EQ(d.read(Reg::LbaLow), 0x00);
    CHECK_EQ(d.read(Reg::LbaMid), 0x00);
    CHECK_EQ(d.read(Reg::LbaHigh), 0x02);  // 131072 = 0x020000
}

TEST(test_read_chs) {
    Device d(INFO);  // 64 MB: 130 cylinders x 16 heads x 63 sectors
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    // Cylinder 1, head 2, sector 3 -> LBA (1 * 16 + 2) * 63 + 2 = 1136
    d.write(Reg::SectorCount, 1);
    d.write(Reg::LbaLow, 3);
    d.write(Reg::LbaMid, 1);
    d.write(Reg::LbaHigh, 0);
    d.write(Reg::Device, 0xA0 | 2);  // CHS, device 0, head 2
    std::vector<uint16_t> got;
    CHECK(h.readAfterCommand(1, got));
    CHECK_EQ(d.lastCommand().lba, 1136u);
    CHECK(sameAsDisk(got, 1136, 1));

    // Sector numbers start at 1: sector 0 is not an address.
    d.write(Reg::LbaLow, 0);
    d.write(Reg::Device, 0xA0);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(h.status() & ST_ERR);
    CHECK_EQ(d.error(), ER_IDNF);
    // Nor is a cylinder beyond the geometry (130 cylinders: 0-129).
    d.write(Reg::LbaLow, 1);
    d.write(Reg::LbaMid, 130);
    d.write(Reg::Device, 0xA0 | 15);  // head 15 is fine
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK_EQ(d.error(), ER_IDNF);
}

struct FailingDisk : GeneratedDisk {
    uint32_t bad;
    explicit FailingDisk(uint32_t bad_lba) : bad(bad_lba) {}
    bool read(uint32_t lba, uint16_t words[256]) override {
        return lba != bad && GeneratedDisk::read(lba, words);
    }
};

TEST(test_read_fetch_failure) {
    Device d(INFO);
    FailingDisk disk(2001);
    VirtualHost h{d, &disk};
    std::vector<uint16_t> got;
    CHECK(!h.readSectors(2000, 3, got));
    CHECK_EQ(got.size(), 256u);  // the first sector arrived; the second failed
    CHECK(h.altStatus() & ST_ERR);
    CHECK_EQ(d.error(), ER_UNC);
    CHECK_EQ(d.read(Reg::LbaLow), 2001 & 0xFF);  // where it failed
    CHECK_EQ(d.read(Reg::LbaMid), 2001 >> 8);
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
}

// ---- WRITE SECTORS -------------------------------------------------------------

static std::vector<uint16_t> pattern(uint32_t sectors, uint16_t seed) {
    std::vector<uint16_t> words(size_t(sectors) * 256);
    for (size_t i = 0; i < words.size(); ++i) words[i] = uint16_t(seed * 31 + i * 7);
    return words;
}

TEST(test_overlay_disk) {
    GeneratedDisk base;
    static RamOverlayDisk<2> disk(base);
    uint16_t got[256], want[256];
    // Unwritten sectors read from the disk underneath.
    CHECK(disk.read(5, got));
    base.read(5, want);
    CHECK(std::equal(got, got + 256, want));
    // Written ones read back.
    std::vector<uint16_t> a = pattern(1, 1), b = pattern(1, 2), c = pattern(1, 3);
    CHECK(disk.write(5, a.data()));
    CHECK(disk.write(9, b.data()));
    CHECK(disk.read(5, got) && std::equal(got, got + 256, a.begin()));
    CHECK(disk.read(9, got) && std::equal(got, got + 256, b.begin()));
    // Full: a new sector is refused; one already held can still be rewritten.
    CHECK(!disk.write(10, c.data()));
    CHECK(disk.write(5, c.data()));
    CHECK(disk.read(5, got) && std::equal(got, got + 256, c.begin()));
    CHECK_EQ(disk.used(), 2u);
    CHECK(!disk.write(base.sectors(), a.data()));  // past the end
    // The generated disk itself is read-only.
    CHECK(!base.write(5, a.data()));
}

TEST(test_write_one_sector) {
    Device d(INFO);
    GeneratedDisk base;
    static RamOverlayDisk<8> disk(base);
    VirtualHost h{d, &disk};
    std::vector<uint16_t> words = pattern(1, 7);
    h.setLba(100, 1);
    d.write(Reg::StatusCommand, CMD_WRITE_SECTORS);
    // Ready for data at once, without an interrupt (ATA PIO data-out).
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);
    CHECK(!d.intrq());
    CHECK_EQ(d.read(Reg::Data), 0);  // nothing to read during a write
    for (int i = 0; i < 255; ++i) d.write(Reg::Data, words[size_t(i)]);
    CHECK(h.altStatus() & ST_DRQ);
    d.write(Reg::Data, words[255]);
    // The sector is in: BSY while core 0 stores it.
    CHECK(h.altStatus() & ST_BSY);
    Fetch f;
    CHECK(d.pendingFetch(f));
    CHECK(f.store);
    CHECK_EQ(f.lba, 100u);
    h.pump();
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);  // complete
    CHECK(d.intrq());
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
    CHECK_EQ(d.lastCommand().command, CMD_WRITE_SECTORS);
    CHECK_EQ(d.lastCommand().lba, 100u);

    std::vector<uint16_t> got;
    CHECK(h.readSectors(100, 1, got));
    CHECK(got == words);
}

TEST(test_write_multiple_sectors_interrupt_each) {
    Device d(INFO);
    GeneratedDisk base;
    static RamOverlayDisk<8> disk(base);
    VirtualHost h{d, &disk};
    std::vector<uint16_t> words = pattern(3, 9);
    h.setLba(2000, 3);
    d.write(Reg::StatusCommand, CMD_WRITE_SECTORS);
    CHECK(!d.intrq());  // no interrupt before the first block
    for (int s = 0; s < 3; ++s) {
        CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);
        CHECK_EQ(d.intrq(), s > 0);  // each later block is announced
        h.status();  // acknowledge
        for (int i = 0; i < 256; ++i) d.write(Reg::Data, words[size_t(s) * 256 + size_t(i)]);
        CHECK(h.altStatus() & ST_BSY);
        h.pump();
    }
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK(d.intrq());  // and the end of the command
    std::vector<uint16_t> got;
    CHECK(h.readSectors(2000, 3, got));
    CHECK(got == words);
    CHECK(h.readSectors(2003, 1, got));  // the next sector is untouched
    CHECK(sameAsDisk(got, 2003, 1));
}

TEST(test_write_count_zero_means_256) {
    Device d(INFO);
    GeneratedDisk base;
    static RamOverlayDisk<256> disk(base);
    VirtualHost h{d, &disk};
    std::vector<uint16_t> words = pattern(256, 3);
    CHECK(h.writeSectors(4096, 0, words));
    CHECK_EQ(disk.used(), 256u);
    std::vector<uint16_t> got;
    CHECK(h.readSectors(4096, 0, got));
    CHECK(got == words);
}

TEST(test_write_past_the_end) {
    Device d(INFO);
    GeneratedDisk base;
    static RamOverlayDisk<8> disk(base);
    VirtualHost h{d, &disk};
    h.setLba(INFO.total_sectors - 1, 2);
    d.write(Reg::StatusCommand, CMD_WRITE_SECTORS);
    CHECK(h.altStatus() & ST_ERR);
    CHECK(!(h.altStatus() & ST_DRQ));
    CHECK_EQ(d.error(), ER_IDNF);
    CHECK_EQ(disk.used(), 0u);
}

TEST(test_write_store_failure) {
    Device d(INFO);
    GeneratedDisk base;
    static RamOverlayDisk<1> disk(base);  // room for one sector only
    VirtualHost h{d, &disk};
    std::vector<uint16_t> words = pattern(2, 5);
    CHECK(!h.writeSectors(300, 2, words));
    CHECK(h.altStatus() & ST_ERR);
    CHECK_EQ(d.error(), ER_ABRT);
    CHECK_EQ(d.read(Reg::LbaLow), 301 & 0xFF);  // where it failed
    CHECK_EQ(d.read(Reg::LbaMid), 301 >> 8);
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
    std::vector<uint16_t> got;  // the first sector made it
    CHECK(h.readSectors(300, 1, got));
    CHECK(std::equal(got.begin(), got.end(), words.begin()));
}

TEST(test_data_writes_outside_a_write_are_ignored) {
    Device d(INFO);
    GeneratedDisk base;
    static RamOverlayDisk<8> disk(base);
    VirtualHost h{d, &disk};
    for (int i = 0; i < 300; ++i) d.write(Reg::Data, 0xBEEF);
    Fetch f;
    CHECK(!d.pendingFetch(f));
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK_EQ(disk.used(), 0u);
}

TEST(test_reset_mid_write_makes_store_stale) {
    Device d(INFO);
    GeneratedDisk base;
    static RamOverlayDisk<8> disk(base);
    VirtualHost h{d, &disk};
    h.setLba(40, 2);
    d.write(Reg::StatusCommand, CMD_WRITE_SECTORS);
    for (int i = 0; i < 256; ++i) d.write(Reg::Data, uint16_t(i));
    Fetch old;
    CHECK(d.pendingFetch(old));
    CHECK(old.store);
    d.hardReset(true);  // the host resets while core 0 is still storing
    d.hardReset(false);
    Fetch f;
    CHECK(!d.pendingFetch(f));
    d.fetchDone(old.id, true);  // core 0 finishes late: ignored
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    for (int i = 0; i < 256; ++i) d.write(Reg::Data, 0xBEEF);  // no data phase now
    CHECK(!d.pendingFetch(f));
}

TEST(test_reset_mid_read_makes_fetch_stale) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d};
    h.setLba(10, 2);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    Fetch old;
    CHECK(d.pendingFetch(old));
    h.softReset();  // the host gives up while core 0 is still fetching
    Fetch f;
    CHECK(!d.pendingFetch(f));
    d.fetchDone(old.id, true);  // core 0 finishes late: ignored
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);

    // A new read gets a new id; the old one still can't complete it.
    h.setLba(20, 1);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(d.pendingFetch(f));
    CHECK(f.id != old.id);
    d.fetchDone(old.id, true);
    CHECK(h.altStatus() & ST_BSY);
    d.fetchDone(f.id, disk.read(f.lba, f.buffer));
    CHECK(h.altStatus() & ST_DRQ);
}

TEST(test_read_with_device1_selected_is_ignored) {
    Device d(INFO);
    VirtualHost h{d};
    h.setLba(0, 1);
    h.select(1);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    Fetch f;
    CHECK(!d.pendingFetch(f));
    CHECK_EQ(d.commandsStarted(), 0u);
}

TEST(test_read_across_cores) {
    // Core 1 (this thread) runs the device and a host reading 300 sectors;
    // core 0 (another thread) serves fetches through the real mailbox.
    Device d(INFO);
    static FetchMailbox mailbox;
    std::atomic<bool> stop{false};
    std::thread core0([&] {
        GeneratedDisk disk;
        Fetch f;
        while (!stop.load()) {
            if (mailbox.takeRequest(f)) {
                bool ok = disk.read(f.lba, f.buffer);
                while (!mailbox.postResult(FetchResult{f.id, ok})) {}
            }
        }
    });

    uint32_t posted = 0;
    auto core1_poll = [&] {
        Fetch f;
        if (d.pendingFetch(f) && f.id != posted && mailbox.post(f)) posted = f.id;
        FetchResult r;
        while (mailbox.takeResult(r)) d.fetchDone(r.id, r.ok);
    };

    std::vector<uint16_t> got;
    bool ok = true;
    for (uint32_t lba = 5000; lba < 5300 && ok; lba += 100) {
        VirtualHost h{d};
        h.setLba(lba, 100);
        d.write(Reg::StatusCommand, CMD_READ_SECTORS);
        for (int s = 0; s < 100 && ok; ++s) {
            int spins = 0;
            while (h.altStatus() & ST_BSY) {
                core1_poll();
                if (++spins > 10000000) { ok = false; break; }
            }
            if (!(h.status() & ST_DRQ)) ok = false;
            for (int i = 0; i < 256 && ok; ++i) got.push_back(d.read(Reg::Data));
        }
    }
    stop.store(true);
    core0.join();
    CHECK(ok);
    CHECK(sameAsDisk(got, 5000, 300));
}

// ---- Housekeeping commands -------------------------------------------------

// Issues a command with the given Features and Sector Count; returns status.
static uint8_t command(Device& d, uint8_t cmd, uint8_t features = 0, uint8_t count = 0) {
    d.write(Reg::ErrorFeatures, features);
    d.write(Reg::SectorCount, count);
    d.write(Reg::StatusCommand, cmd);
    return uint8_t(d.read(Reg::AltStatusDevCtrl));
}

TEST(test_set_features_transfer_mode) {
    Device d(INFO);
    // PIO default and PIO flow-control mode 0: accepted.
    CHECK_EQ(command(d, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, XFER_PIO_DEFAULT), ST_DRDY | ST_DSC);
    CHECK(d.intrq());
    CHECK_EQ(command(d, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, XFER_PIO_FLOW_0), ST_DRDY | ST_DSC);
    // Everything not advertised: aborted.
    const uint8_t refused[] = {
        0x01,        // PIO default with IORDY disabled
        0x0A, 0x0C,  // PIO modes 2 and 4
        0x20, 0x22,  // multiword DMA 0 and 2
        0x40, 0x45,  // Ultra DMA 0 and 5
    };
    for (uint8_t mode : refused) {
        CHECK(command(d, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, mode) & ST_ERR);
        CHECK_EQ(d.error(), ER_ABRT);
    }
}

TEST(test_set_features_other_subcommands_abort) {
    Device d(INFO);
    const uint8_t refused[] = {0x02, 0x82, 0x55, 0xAA, 0x66, 0xCC};  // cache, look-ahead, defaults
    for (uint8_t f : refused) {
        CHECK(command(d, CMD_SET_FEATURES, f, 0) & ST_ERR);
        CHECK_EQ(d.error(), ER_ABRT);
    }
}

TEST(test_initialize_device_parameters) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    // 8 heads x 32 sectors: 131072 / 256 = 512 cylinders.
    d.write(Reg::Device, 0xA0 | (8 - 1));
    CHECK_EQ(command(d, CMD_INITIALIZE_DEVICE_PARAMETERS, 0, 32), ST_DRDY | ST_DSC);
    CHECK_EQ(d.geometry().heads, 8);
    CHECK_EQ(d.geometry().sectors_per_track, 32);
    CHECK_EQ(d.geometry().cylinders, 512);

    // IDENTIFY reports it as the current translation, with a valid checksum.
    uint16_t id[256];
    h.select(0);
    CHECK(h.identify(id));
    CHECK_EQ(id[54], 512);
    CHECK_EQ(id[55], 8);
    CHECK_EQ(id[56], 32);
    CHECK_EQ(uint32_t(id[57]) | (uint32_t(id[58]) << 16), 512u * 8u * 32u);
    CHECK_EQ(id[1], 130);  // the default geometry is unchanged
    uint8_t sum = 0;
    for (int i = 0; i < 256; ++i) sum += uint8_t(id[i] & 0xFF) + uint8_t(id[i] >> 8);
    CHECK_EQ(sum, 0);

    // CHS reads use it: cylinder 1, head 2, sector 3 -> (1*8 + 2)*32 + 2 = 322.
    d.write(Reg::SectorCount, 1);
    d.write(Reg::LbaLow, 3);
    d.write(Reg::LbaMid, 1);
    d.write(Reg::LbaHigh, 0);
    d.write(Reg::Device, 0xA0 | 2);
    std::vector<uint16_t> got;
    CHECK(h.readAfterCommand(1, got));
    CHECK_EQ(d.lastCommand().lba, 322u);
    CHECK(sameAsDisk(got, 322, 1));
}

TEST(test_initialize_device_parameters_survives_soft_reset_not_hard) {
    Device d(INFO);
    VirtualHost h{d};
    d.write(Reg::Device, 0xA0 | 3);
    command(d, CMD_INITIALIZE_DEVICE_PARAMETERS, 0, 17);
    CHECK_EQ(d.geometry().heads, 4);
    h.softReset();
    CHECK_EQ(d.geometry().heads, 4);
    d.hardReset(true);
    d.hardReset(false);
    CHECK_EQ(d.geometry().heads, 16);
    CHECK_EQ(d.geometry().sectors_per_track, 63);
}

TEST(test_initialize_device_parameters_rejects_zero_sectors) {
    Device d(INFO);
    d.write(Reg::Device, 0xA0 | 15);
    CHECK(command(d, CMD_INITIALIZE_DEVICE_PARAMETERS, 0, 0) & ST_ERR);
    CHECK_EQ(d.error(), ER_ABRT);
    CHECK_EQ(d.geometry().heads, 16);  // unchanged
}

TEST(test_read_verify) {
    Device d(INFO);
    VirtualHost h{d};
    h.setLba(1000, 50);
    d.write(Reg::StatusCommand, CMD_READ_VERIFY);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);  // done, with no data phase
    Fetch f;
    CHECK(!d.pendingFetch(f));
    CHECK(d.intrq());
    CHECK_EQ(d.lastCommand().count, 50);
    h.setLba(131000, 0);  // 256 sectors from 131000 runs off the end
    d.write(Reg::StatusCommand, CMD_READ_VERIFY);
    CHECK(h.altStatus() & ST_ERR);
    CHECK_EQ(d.error(), ER_IDNF);
}

TEST(test_seek_and_recalibrate) {
    Device d(INFO);
    VirtualHost h{d};
    h.setLba(131071, 1);
    d.write(Reg::StatusCommand, 0x70);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    h.setLba(131072, 1);
    d.write(Reg::StatusCommand, 0x7F);  // any of 70h-7Fh
    CHECK_EQ(d.error(), ER_IDNF);
    d.write(Reg::StatusCommand, 0x10);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    d.write(Reg::StatusCommand, 0x1F);  // any of 10h-1Fh
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK(d.intrq());
}

TEST(test_power_commands) {
    Device d(INFO);
    VirtualHost h{d};
    CHECK_EQ(command(d, CMD_CHECK_POWER_MODE), ST_DRDY | ST_DSC);
    CHECK_EQ(d.read(Reg::SectorCount), POWER_ACTIVE_OR_IDLE);
    command(d, CMD_STANDBY_IMMEDIATE);
    CHECK(d.standby());
    command(d, CMD_CHECK_POWER_MODE);
    CHECK_EQ(d.read(Reg::SectorCount), POWER_STANDBY);
    // Waking from standby: BSY and no interrupt until finishWake().
    CHECK_EQ(command(d, CMD_IDLE_IMMEDIATE), ST_BSY | ST_DRDY | ST_DSC);
    CHECK(d.waking());
    CHECK(!d.intrq());
    command(d, CMD_CHECK_POWER_MODE);  // ignored while BSY
    d.finishWake();
    CHECK(!d.waking());
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK(d.intrq());
    CHECK_EQ(command(d, CMD_IDLE_IMMEDIATE), ST_DRDY | ST_DSC);  // already awake: at once
    command(d, CMD_CHECK_POWER_MODE_OLD);
    CHECK_EQ(d.read(Reg::SectorCount), POWER_ACTIVE_OR_IDLE);
    // A read wakes a drive in standby.
    command(d, CMD_STANDBY, 0, 12);  // with a timer value: accepted, ignored
    CHECK(d.standby());
    h.setLba(0, 1);
    d.write(Reg::StatusCommand, CMD_READ_VERIFY);
    CHECK(!d.standby());
    const uint8_t also_ok[] = {CMD_STANDBY_OLD, CMD_IDLE, CMD_IDLE_OLD, CMD_IDLE_IMMEDIATE_OLD,
                               CMD_STANDBY_IMMEDIATE_OLD, CMD_FLUSH_CACHE};
    for (uint8_t c : also_ok) {
        uint8_t status = command(d, c);
        if (d.waking()) {  // IDLE after STANDBY
            CHECK_EQ(status, ST_BSY | ST_DRDY | ST_DSC);
            d.finishWake();
            status = h.altStatus();
        }
        CHECK_EQ(status, ST_DRDY | ST_DSC);
    }
}

TEST(test_reset_cancels_waking) {
    Device d(INFO);
    command(d, CMD_STANDBY_IMMEDIATE);
    command(d, CMD_IDLE_IMMEDIATE);
    CHECK(d.waking());
    d.hardReset(true);
    d.hardReset(false);
    CHECK(!d.waking());
    d.finishWake();  // too late: nothing happens
    CHECK(!d.intrq());
}

TEST(test_set_multiple_mode_aborts) {
    Device d(INFO);
    CHECK(command(d, CMD_SET_MULTIPLE_MODE, 0, 16) & ST_ERR);
    CHECK_EQ(d.error(), ER_ABRT);
}

TEST(test_housekeeping_commands_complete_for_the_trace) {
    Device d(INFO);
    const uint8_t cmds[] = {CMD_FLUSH_CACHE, CMD_CHECK_POWER_MODE, CMD_IDLE_IMMEDIATE, 0x10, 0x70,
                            CMD_SET_MULTIPLE_MODE};
    for (uint8_t c : cmds) command(d, c);
    CHECK_EQ(d.commandsStarted(), 6u);
    CHECK_EQ(d.commandsCompleted(), 6u);  // each ends with a DONE line
}

// What a USB bridge typically does once it has found a drive: IDENTIFY, pick
// a transfer mode from it, set the CHS translation, try multiple mode, read.
TEST(test_virtual_host_bridge_setup) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    uint16_t id[256];
    CHECK(h.identify(id));

    // The best mode IDENTIFY allows: no DMA (word 49 bit 8, words 63, 88),
    // no PIO 3/4 (word 64): so PIO flow-control mode 0.
    bool dma = (id[49] & (1u << 8)) || (id[63] & 0x7) || ((id[53] & 4) && (id[88] & 0x7F));
    CHECK(!dma);
    CHECK_EQ(id[64] & 0x3, 0);
    CHECK_EQ(command(d, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, XFER_PIO_FLOW_0) & ST_ERR, 0);
    h.status();

    d.write(Reg::Device, 0xA0 | (id[3] - 1));
    CHECK_EQ(command(d, CMD_INITIALIZE_DEVICE_PARAMETERS, 0, uint8_t(id[6])) & ST_ERR, 0);

    // Multiple mode only if IDENTIFY word 47 offers it (it doesn't).
    CHECK_EQ(id[47] & 0xFF, 0);

    std::vector<uint16_t> got;
    CHECK(h.readSectors(0, 8, got));
    CHECK(sameAsDisk(got, 0, 8));
}

void runAtaDeviceTests() {
    RUN(test_reg_mapping);
    RUN(test_power_on_state);
    RUN(test_hard_reset);
    RUN(test_soft_reset);
    RUN(test_soft_reset_interrupts_a_transfer);
    RUN(test_register_echo);
    RUN(test_absent_device1);
    RUN(test_identify_transfer);
    RUN(test_identify_twice);
    RUN(test_unknown_command_aborts);
    RUN(test_execute_device_diagnostic);
    RUN(test_diagnostic_runs_with_device1_selected);
    RUN(test_command_ignored_while_busy);
    RUN(test_intrq_cleared_by_status_not_alt_status);
    RUN(test_intrq_masked_by_nien_and_device1);
    RUN(test_intrq_cleared_by_new_command);
    RUN(test_virtual_host_probe);
    RUN(test_generated_disk_words);
    RUN(test_read_one_sector);
    RUN(test_read_busy_until_fetched);
    RUN(test_read_multiple_sectors_interrupt_each);
    RUN(test_read_count_zero_means_256);
    RUN(test_read_last_sector_and_past_the_end);
    RUN(test_read_chs);
    RUN(test_read_fetch_failure);
    RUN(test_overlay_disk);
    RUN(test_write_one_sector);
    RUN(test_write_multiple_sectors_interrupt_each);
    RUN(test_write_count_zero_means_256);
    RUN(test_write_past_the_end);
    RUN(test_write_store_failure);
    RUN(test_data_writes_outside_a_write_are_ignored);
    RUN(test_reset_mid_write_makes_store_stale);
    RUN(test_reset_mid_read_makes_fetch_stale);
    RUN(test_read_with_device1_selected_is_ignored);
    RUN(test_read_across_cores);
    RUN(test_set_features_transfer_mode);
    RUN(test_set_features_other_subcommands_abort);
    RUN(test_initialize_device_parameters);
    RUN(test_initialize_device_parameters_survives_soft_reset_not_hard);
    RUN(test_initialize_device_parameters_rejects_zero_sectors);
    RUN(test_read_verify);
    RUN(test_seek_and_recalibrate);
    RUN(test_power_commands);
    RUN(test_reset_cancels_waking);
    RUN(test_set_multiple_mode_aborts);
    RUN(test_housekeeping_commands_complete_for_the_trace);
    RUN(test_virtual_host_bridge_setup);
}
