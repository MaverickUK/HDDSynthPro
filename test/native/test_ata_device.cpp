// Tests for the ATA register file and command state machine (ata_device),
// including a "virtual host" that plays a typical start-up probe.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
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
        if (dev.dataOutHeld()) dev.releaseDataOut();  // core 1's timer
        if (source && dev.pendingFetch(f)) {
            bool ok = f.store ? source->write(f.lba, f.buffer) : f.flush ? true : source->read(f.lba, f.buffer);
            dev.fetchDone(f.id, ok && (!f.flush || source->flush()));
        }
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

TEST(test_fast_lane_matches_data_reads) {
    // Core 1's fast lane: take the block's remaining words, answer reads from
    // them, then consume. Here split by an ALTSTATUS read mid-sector, which
    // must see DRQ and leave the interrupt pending.
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d};
    h.setLba(2000, 2);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    std::vector<uint16_t> got;
    for (int s = 0; s < 2; ++s) {
        Fetch f;
        CHECK(d.pendingFetch(f));
        d.fetchDone(f.id, disk.read(f.lba, f.buffer));
        h.status();
        const uint16_t* next;
        uint16_t left;
        CHECK(d.dataInRemaining(next, left));
        CHECK_EQ(left, 256);
        for (int i = 0; i < 100; ++i) got.push_back(next[i]);
        d.consumeDataWords(100);
        CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);
        CHECK(d.dataInRemaining(next, left));
        CHECK_EQ(left, 156);
        for (int i = 0; i < 156; ++i) got.push_back(next[i]);
        d.consumeDataWords(156);
        CHECK(!d.dataInRemaining(next, left));  // block done: BSY for the next fetch, or complete
    }
    CHECK(sameAsDisk(got, 2000, 2));
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
}

TEST(test_fast_lane_only_for_a_data_in_block) {
    Device d(INFO);
    const uint16_t* next;
    uint16_t left;
    CHECK(!d.dataInRemaining(next, left));  // idle
    VirtualHost h{d};
    h.setLba(10, 1);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(!d.dataInRemaining(next, left));  // BSY: fetching
    Fetch f;
    CHECK(d.pendingFetch(f));
    GeneratedDisk disk;
    d.fetchDone(f.id, disk.read(f.lba, f.buffer));
    CHECK(d.dataInRemaining(next, left));
    d.write(Reg::Device, DEV_DEV);  // device 1 selected: its DATA reads return 0
    CHECK(!d.dataInRemaining(next, left));
    d.consumeDataWords(5);  // ignored, as consumeRead would be
    d.write(Reg::Device, 0);
    CHECK(d.dataInRemaining(next, left));
    CHECK_EQ(left, 256);
}

TEST(test_read_ahead_hides_the_fetch) {
    // While the host reads a sector, the next one is fetched into the other
    // buffer; when the host finishes, DRQ and INTRQ for it come at once.
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d};
    h.setLba(3000, 3);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    Fetch f;
    CHECK(d.pendingFetch(f));
    d.fetchDone(f.id, disk.read(f.lba, f.buffer));
    std::vector<uint16_t> got;
    for (int s = 0; s < 3; ++s) {
        CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);
        CHECK(d.intrq());
        h.status();
        const uint16_t* next;
        uint16_t left;
        CHECK(d.dataInRemaining(next, left));
        Fetch ahead;
        bool more = s < 2;
        CHECK_EQ(d.pendingFetch(ahead), more);
        if (more) {
            CHECK_EQ(ahead.lba, 3001u + s);
            CHECK(ahead.buffer != next);  // never the buffer the host is reading
            d.fetchDone(ahead.id, disk.read(ahead.lba, ahead.buffer));
            CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);  // the host's sector is untouched
        }
        for (int i = 0; i < 256; ++i) got.push_back(d.read(Reg::Data));
    }
    CHECK(sameAsDisk(got, 3000, 3));
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
}

TEST(test_read_ahead_failure_reported_at_its_sector) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d};
    h.setLba(4000, 3);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    Fetch f;
    CHECK(d.pendingFetch(f));
    d.fetchDone(f.id, disk.read(f.lba, f.buffer));
    h.status();
    CHECK(d.pendingFetch(f));
    CHECK_EQ(f.lba, 4001u);
    d.fetchDone(f.id, false);  // the card failed on the next sector
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);  // sector 4000 still reads
    std::vector<uint16_t> got;
    for (int i = 0; i < 256; ++i) got.push_back(d.read(Reg::Data));
    CHECK(sameAsDisk(got, 4000, 1));
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC | ST_ERR);
    CHECK_EQ(d.read(Reg::ErrorFeatures), ER_UNC);
    CHECK_EQ(d.read(Reg::LbaLow), 4001 & 0xFF);  // the failing sector
    CHECK_EQ(d.read(Reg::LbaMid), (4001 >> 8) & 0xFF);
    CHECK(!d.pendingFetch(f));
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
}

TEST(test_read_ahead_stale_after_reset) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d};
    h.setLba(5000, 2);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    Fetch f;
    CHECK(d.pendingFetch(f));
    d.fetchDone(f.id, disk.read(f.lba, f.buffer));
    Fetch ahead;
    CHECK(d.pendingFetch(ahead));
    d.hardReset(true);
    d.hardReset(false);
    d.fetchDone(ahead.id, disk.read(ahead.lba, ahead.buffer));  // late: ignored
    CHECK(!(h.altStatus() & ST_DRQ));
    const uint16_t* next;
    uint16_t left;
    CHECK(!d.dataInRemaining(next, left));
    CHECK(!d.pendingFetch(f));
}

TEST(test_command_abandons_unread_block) {
    // The host gives up on a READ partway through a sector and writes the
    // command again, with no reset (the JMicron did, 27 Oct 2026). The new
    // command must start clean: a stale data pointer looked like a transfer
    // under way (so loop1 never took the fetch result) and made the new
    // sector look like a read-ahead, and the drive stayed BSY for good.
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    h.setLba(7, 4);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(h.waitNotBusy());
    h.status();
    for (int i = 0; i < 100; ++i) d.read(Reg::Data);  // part of sector 7
    Fetch ahead;
    CHECK(d.pendingFetch(ahead));  // the read-ahead of sector 8, under way on core 0
    CHECK(d.transferring());

    h.setLba(7, 4);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(!d.transferring());  // the old block is gone
    d.fetchDone(ahead.id, disk.read(ahead.lba, ahead.buffer));  // late: ignored
    std::vector<uint16_t> got;
    for (int s = 0; s < 4; ++s) {
        CHECK(h.waitNotBusy());
        CHECK_EQ(h.status() & (ST_DRQ | ST_ERR), ST_DRQ);
        for (int i = 0; i < 256; ++i) got.push_back(d.read(Reg::Data));
    }
    CHECK(h.waitNotBusy());
    CHECK(sameAsDisk(got, 7, 4));
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);
}

TEST(test_command_abandons_unread_identify) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    d.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
    h.status();
    for (int i = 0; i < 10; ++i) d.read(Reg::Data);
    std::vector<uint16_t> got;
    CHECK(h.readSectors(42, 2, got));
    CHECK(sameAsDisk(got, 42, 2));
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());  // the IDENTIFY counts as ended: not busy
}

TEST(test_reset_ends_the_running_command) {
    // The activity LED and the link's busy flag go off when the counts match.
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    h.setLba(100, 4);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(h.waitNotBusy());
    CHECK(d.commandsCompleted() != d.commandsStarted());
    d.hardReset(true);
    d.hardReset(false);
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());

    h.setLba(100, 4);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    CHECK(h.waitNotBusy());
    h.softReset();
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
}

TEST(test_read_ahead_off) {
    // Off, the next sector is fetched under BSY after the host reads one.
    Device d(INFO);
    d.setReadAhead(false);
    GeneratedDisk disk;
    VirtualHost h{d};
    h.setLba(6000, 2);
    d.write(Reg::StatusCommand, CMD_READ_SECTORS);
    Fetch f;
    CHECK(d.pendingFetch(f));
    d.fetchDone(f.id, disk.read(f.lba, f.buffer));
    CHECK(!d.pendingFetch(f));  // nothing ahead
    std::vector<uint16_t> got;
    for (int i = 0; i < 256; ++i) got.push_back(d.read(Reg::Data));
    CHECK(h.altStatus() & ST_BSY);
    CHECK(d.pendingFetch(f));
    CHECK_EQ(f.lba, 6001u);
    d.fetchDone(f.id, disk.read(f.lba, f.buffer));
    for (int i = 0; i < 256; ++i) got.push_back(d.read(Reg::Data));
    CHECK(sameAsDisk(got, 6000, 2));
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);
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

// An image file in memory, standing in for HDDSYNTH.IMG on the SD card.
struct MemoryImage : ImageFile {
    std::vector<uint8_t> bytes;
    bool fail = false;
    explicit MemoryImage(size_t size) : bytes(size) {}
    uint64_t size() const override { return bytes.size(); }
    bool readAt(uint64_t offset, void* data, size_t n) override {
        if (fail || offset + n > bytes.size()) return false;
        std::memcpy(data, bytes.data() + offset, n);
        return true;
    }
    bool writeAt(uint64_t offset, const void* data, size_t n) override {
        if (fail || offset + n > bytes.size()) return false;
        std::memcpy(bytes.data() + offset, data, n);
        return true;
    }
    int syncs = 0;
    bool sync() override {
        ++syncs;
        return !fail;
    }
};

TEST(test_image_disk) {
    MemoryImage file(16 * 512 + 100);  // a stray tail that isn't a whole sector
    ImageDisk disk(file);
    CHECK_EQ(disk.sectors(), 16u);
    // The file's bytes are the disk's bytes: word n is bytes 2n (low) and 2n+1.
    file.bytes[3 * 512 + 0] = 0x34;
    file.bytes[3 * 512 + 1] = 0x12;
    uint16_t got[256];
    CHECK(disk.read(3, got));
    CHECK_EQ(got[0], 0x1234);
    std::vector<uint16_t> words = pattern(1, 4);
    CHECK(disk.write(15, words.data()));  // the last whole sector
    CHECK(disk.read(15, got) && std::equal(got, got + 256, words.begin()));
    CHECK_EQ(file.bytes[15 * 512], uint8_t(words[0]));
    CHECK(!disk.read(16, got));  // past the end, the partial tail included
    CHECK(!disk.write(16, words.data()));
    CHECK(disk.flush());  // FLUSH CACHE syncs the file
    CHECK_EQ(file.syncs, 1);
    file.fail = true;  // an SD error reaches the device as a failed fetch
    CHECK(!disk.read(0, got));
    CHECK(!disk.write(0, words.data()));
    CHECK(!disk.flush());
}

TEST(test_read_and_write_through_an_image) {
    // The drive end to end on an image: what the host writes, it reads back,
    // and the image file holds it.
    MemoryImage file(256 * 512);
    ImageDisk disk(file);
    IdentifyInfo info{"HDDSYNTH PROTO", "HSP-000001", "0.2.0", disk.sectors()};
    Device d(info);
    VirtualHost h{d, &disk};
    std::vector<uint16_t> words = pattern(3, 8);
    CHECK(h.writeSectors(100, 3, words));
    std::vector<uint16_t> got;
    CHECK(h.readSectors(100, 3, got));
    CHECK(got == words);
    CHECK_EQ(file.bytes[100 * 512 + 1], uint8_t(words[0] >> 8));
    CHECK(h.readSectors(0, 1, got));  // a fresh image reads as zeros
    CHECK(std::all_of(got.begin(), got.end(), [](uint16_t w) { return w == 0; }));
}

// The NS1081's detection, as the drive's trace shows it every time (Windows,
// 26-27 Oct 2026): two RESET# pulses, IDENTIFY twice, SET FEATURES for PIO
// flow-control mode 4 (refused), then mode 0, then the host's media commands
// carry on. It waits for INTRQ after each command and reads Status, which
// acknowledges it. The detection is the most fragile moment with the
// adapter and needs a physical replug to see on the rig; this keeps it in CI.
TEST(test_ns1081_detection) {
    Device d(INFO);
    GeneratedDisk base;
    static RamOverlayDisk<8> disk(base);
    VirtualHost h{d, &disk};
    for (int pulse = 0; pulse < 2; ++pulse) {
        d.hardReset(true);
        CHECK(h.altStatus() & ST_BSY);
        d.hardReset(false);
    }
    CHECK(h.waitNotBusy());
    CHECK(h.ataSignature());

    h.select(0);
    for (int i = 0; i < 2; ++i) {
        d.write(Reg::StatusCommand, CMD_IDENTIFY_DEVICE);
        CHECK(d.intrq());                     // data ready: the adapter's cue
        CHECK_EQ(h.status(), ST_DRDY | ST_DSC | ST_DRQ);
        CHECK(!d.intrq());                    // reading Status acknowledged it
        uint16_t id[256];
        for (auto& w : id) w = d.read(Reg::Data);
        CHECK_EQ(uint32_t(id[60]) | (uint32_t(id[61]) << 16), 131072u);
        CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    }

    // PIO flow-control mode 4 (0Ch): IDENTIFY offers mode 0 only, so ABRT,
    // with an interrupt, as the trace shows ("DONE ef 51 04").
    d.write(Reg::ErrorFeatures, SF_SET_TRANSFER_MODE);
    d.write(Reg::SectorCount, 0x0C);
    d.write(Reg::StatusCommand, CMD_SET_FEATURES);
    CHECK(d.intrq());
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC | ST_ERR);
    CHECK_EQ(d.read(Reg::ErrorFeatures), ER_ABRT);
    // Then flow-control mode 0 (08h): accepted ("DONE ef 50 00").
    d.write(Reg::SectorCount, XFER_PIO_FLOW_0);
    d.write(Reg::StatusCommand, CMD_SET_FEATURES);  // Features still 03h
    CHECK(d.intrq());
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);
    CHECK_EQ(d.read(Reg::ErrorFeatures), 0);

    // The host's media commands carry on: a write, read back.
    std::vector<uint16_t> words = pattern(2, 31), got;
    CHECK(h.writeSectors(100000, 2, words));
    CHECK(h.readSectors(100000, 2, got));
    CHECK(got == words);
    CHECK(h.readSectors(0, 1, got));
    CHECK(sameAsDisk(got, 0, 1));
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
    CHECK(d.transferring());
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
    CHECK(!d.transferring());
    CHECK(d.intrq());
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
    CHECK_EQ(d.lastCommand().command, CMD_WRITE_SECTORS);
    CHECK_EQ(d.lastCommand().lba, 100u);

    std::vector<uint16_t> got;
    CHECK(h.readSectors(100, 1, got));
    CHECK(got == words);
}

TEST(test_write_multiple_sectors_interrupt_each) {
    // Write-behind off: BSY while each sector is stored, as before 0.8.16.
    Device d(INFO);
    d.setWriteBehind(false);
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

TEST(test_write_behind_overlaps_the_store) {
    // The host sends sector 2 into the other buffer while sector 1 is stored.
    Device d(INFO);
    VirtualHost h{d};
    std::vector<uint16_t> words = pattern(3, 21);
    GeneratedDisk base;
    RamOverlayDisk<8> disk(base);
    h.setLba(3000, 3);
    d.write(Reg::StatusCommand, CMD_WRITE_SECTORS);
    for (int i = 0; i < 256; ++i) d.write(Reg::Data, words[size_t(i)]);
    Fetch store1;
    CHECK(d.pendingFetch(store1));  // sector 1's store is under way...
    CHECK(store1.store);
    CHECK_EQ(store1.lba, 3000u);
    // The NS1081 reads Status straight after a block: BSY, and no interrupt
    // yet for it to clear. Sector 2 is wanted once core 1 releases the hold.
    CHECK_EQ(h.status(), ST_BSY | ST_DRDY | ST_DSC);
    CHECK(d.dataOutHeld());
    d.releaseDataOut();
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);  // ...while sector 1 is still storing
    CHECK(d.intrq());
    h.status();
    for (int i = 0; i < 100; ++i) d.write(Reg::Data, words[256 + size_t(i)]);
    CHECK(disk.write(store1.lba, store1.buffer));
    d.fetchDone(store1.id, true);  // stored while the host is mid-sector
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);
    for (int i = 100; i < 256; ++i) d.write(Reg::Data, words[256 + size_t(i)]);
    Fetch store2;
    CHECK(d.pendingFetch(store2));
    CHECK_EQ(store2.lba, 3001u);
    CHECK(store2.buffer != store1.buffer);
    CHECK_EQ(h.altStatus(), ST_BSY | ST_DRDY | ST_DSC);  // the hold, though a buffer is free
    CHECK(!d.intrq());
    d.releaseDataOut();
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);  // sector 3, into the buffer just stored
    CHECK(d.intrq());
    h.status();
    for (int i = 0; i < 256; ++i) d.write(Reg::Data, words[512 + size_t(i)]);
    CHECK(h.altStatus() & ST_BSY);  // both buffers busy: sector 2 storing, sector 3 waiting
    CHECK(disk.write(store2.lba, store2.buffer));
    d.fetchDone(store2.id, true);
    Fetch store3;
    CHECK(d.pendingFetch(store3));
    CHECK_EQ(store3.lba, 3002u);
    CHECK(h.altStatus() & ST_BSY);  // no more sectors to receive: BSY until the last store
    CHECK(!d.intrq());
    CHECK(disk.write(store3.lba, store3.buffer));
    d.fetchDone(store3.id, true);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);  // all stored: done
    CHECK(d.intrq());
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
    std::vector<uint16_t> got(256 * 3);
    for (uint32_t s2 = 0; s2 < 3; ++s2) CHECK(disk.read(3000 + s2, got.data() + 256 * s2));
    CHECK(got == words);
}

TEST(test_write_behind_store_failure) {
    // Sector 1's store fails while the host sends sector 2: the error comes
    // when that block ends, at sector 1, and sector 2 is never stored.
    Device d(INFO);
    VirtualHost h{d};
    std::vector<uint16_t> words = pattern(3, 22);
    h.setLba(3100, 3);
    d.write(Reg::StatusCommand, CMD_WRITE_SECTORS);
    for (int i = 0; i < 256; ++i) d.write(Reg::Data, words[size_t(i)]);
    Fetch f;
    CHECK(d.pendingFetch(f));
    d.releaseDataOut();
    for (int i = 0; i < 50; ++i) d.write(Reg::Data, words[256 + size_t(i)]);
    d.fetchDone(f.id, false);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC | ST_DRQ);  // the host finishes its block
    for (int i = 50; i < 256; ++i) d.write(Reg::Data, words[256 + size_t(i)]);
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC | ST_ERR);
    CHECK_EQ(d.error(), ER_ABRT);
    CHECK_EQ(d.read(Reg::LbaLow), 3100 & 0xFF);
    CHECK_EQ(d.read(Reg::LbaMid), (3100 >> 8) & 0xFF);
    CHECK(!d.pendingFetch(f));  // nothing more stored
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());
}

TEST(test_write_behind_through_the_virtual_host) {
    Device d(INFO);
    GeneratedDisk base;
    static RamOverlayDisk<64> disk(base);
    VirtualHost h{d, &disk};
    std::vector<uint16_t> words = pattern(40, 23);
    CHECK(h.writeSectors(9000, 40, words));
    std::vector<uint16_t> got;
    CHECK(h.readSectors(9000, 40, got));
    CHECK(got == words);
    CHECK(h.readSectors(9040, 1, got));
    CHECK(sameAsDisk(got, 9040, 1));
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

TEST(test_set_features_pio_modes_offered) {
    // Offering PIO mode 2: flow-control modes 0-2 accepted, 3 and 4 aborted.
    IdentifyInfo info = INFO;
    info.pio_mode = 2;
    Device d(info);
    for (uint8_t mode : {uint8_t(0x08), uint8_t(0x09), uint8_t(0x0A), XFER_PIO_DEFAULT})
        CHECK_EQ(command(d, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, mode), ST_DRDY | ST_DSC);
    for (uint8_t mode : {uint8_t(0x0B), uint8_t(0x0C), uint8_t(0x22)}) {
        CHECK(command(d, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, mode) & ST_ERR);
        CHECK_EQ(d.error(), ER_ABRT);
    }
    // Offering mode 3: 3 accepted; 4, which the adapter asks for first (0Ch),
    // aborted, and even asked for an IDENTIFY that can't offer it.
    info.pio_mode = 3;
    Device d3(info);
    CHECK_EQ(command(d3, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, 0x0B), ST_DRDY | ST_DSC);
    CHECK(command(d3, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, 0x0C) & ST_ERR);
    info.pio_mode = 4;
    Device d4(info);
    CHECK(command(d4, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, 0x0C) & ST_ERR);
    // Offering mode 1: 2 aborted.
    info.pio_mode = 1;
    Device d1(info);
    CHECK_EQ(command(d1, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, 0x09), ST_DRDY | ST_DSC);
    CHECK(command(d1, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, 0x0A) & ST_ERR);
}

TEST(test_set_features_transfer_mode) {
    Device d(INFO);  // offers PIO mode 0 only
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

TEST(test_set_features_traced_with_its_subcommand) {
    Device d(INFO);
    command(d, CMD_SET_FEATURES, SF_SET_TRANSFER_MODE, XFER_PIO_FLOW_0);
    CHECK_EQ(d.lastCommand().command, CMD_SET_FEATURES);
    CHECK_EQ(d.lastCommand().lba, uint32_t(SF_SET_TRANSFER_MODE));
    CHECK_EQ(d.lastCommand().count, XFER_PIO_FLOW_0);
}

TEST(test_set_features_other_subcommands_abort) {
    Device d(INFO);
    const uint8_t refused[] = {0x55, 0xAA, 0x66, 0xCC};  // look-ahead, power-on defaults
    for (uint8_t f : refused) {
        CHECK(command(d, CMD_SET_FEATURES, f, 0) & ST_ERR);
        CHECK_EQ(d.error(), ER_ABRT);
    }
}

// Writable storage that counts flushes, and can fail them.
struct FlushCounter : BlockSource {
    GeneratedDisk base;
    RamOverlayDisk<16> ram{base};
    uint32_t sectors() const override { return ram.sectors(); }
    bool read(uint32_t lba, uint16_t words[256]) override { return ram.read(lba, words); }
    bool write(uint32_t lba, const uint16_t words[256]) override { return ram.write(lba, words); }
    int flushes = 0;
    bool fail = false;
    bool flush() override {
        ++flushes;
        return !fail;
    }
};

static bool identifyChecksumOk(const uint16_t w[256]) {
    uint8_t sum = 0;
    for (int i = 0; i < 256; ++i) sum = uint8_t(sum + (w[i] & 0xFF) + (w[i] >> 8));
    return sum == 0 && (w[255] & 0xFF) == 0xA5;
}

TEST(test_write_cache_on_and_off) {
    Device d(INFO);
    FlushCounter disk;
    VirtualHost h{d, &disk};
    uint16_t id[256];
    CHECK(h.identify(id));
    CHECK(id[85] & (1u << 5));  // on after power-up
    CHECK_EQ(command(d, CMD_SET_FEATURES, SF_DISABLE_WRITE_CACHE, 0), ST_DRDY | ST_DSC);
    CHECK(h.identify(id));
    CHECK(!(id[85] & (1u << 5)));
    CHECK(identifyChecksumOk(id));
    // Off: each sector is synced as it's stored.
    std::vector<uint16_t> words = pattern(2, 512);
    CHECK(h.writeSectors(7000, 2, words));
    CHECK_EQ(disk.flushes, 2);
    CHECK_EQ(command(d, CMD_SET_FEATURES, SF_ENABLE_WRITE_CACHE, 0), ST_DRDY | ST_DSC);
    CHECK(h.writeSectors(7000, 2, words));
    CHECK_EQ(disk.flushes, 2);  // on: stored, flushed only on FLUSH CACHE
    command(d, CMD_SET_FEATURES, SF_DISABLE_WRITE_CACHE, 0);
    d.hardReset(true);
    d.hardReset(false);
    CHECK(h.identify(id));
    CHECK(id[85] & (1u << 5));  // a hardware reset turns it back on
    CHECK(identifyChecksumOk(id));
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
                               CMD_STANDBY_IMMEDIATE_OLD};
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

TEST(test_queued_command) {
    Device d(INFO);
    GeneratedDisk disk;
    VirtualHost h{d, &disk};
    h.setLba(7, 1);
    d.queueCommand(CMD_READ_SECTORS);
    // At once: BSY, nothing started, no interrupt.
    CHECK(d.commandQueued());
    CHECK_EQ(h.altStatus(), ST_BSY | ST_DRDY | ST_DSC);
    CHECK_EQ(d.commandsStarted(), 0u);
    CHECK(!d.intrq());
    d.write(Reg::LbaLow, 99);  // ignored while BSY
    d.queueCommand(CMD_IDENTIFY_DEVICE);  // ignored while BSY
    d.runQueuedCommand();
    CHECK(!d.commandQueued());
    CHECK_EQ(d.lastCommand().command, CMD_READ_SECTORS);
    CHECK_EQ(d.lastCommand().lba, 7u);
    std::vector<uint16_t> got;
    CHECK(h.waitNotBusy());
    CHECK(h.status() & ST_DRQ);
    for (int i = 0; i < 256; ++i) got.push_back(d.read(Reg::Data));
    CHECK(sameAsDisk(got, 7, 1));
    d.runQueuedCommand();  // nothing queued: nothing happens
    CHECK_EQ(d.commandsStarted(), 1u);
}

TEST(test_queued_command_rules) {
    Device d(INFO);
    VirtualHost h{d};
    // Device 1 selected: not ours, except EXECUTE DEVICE DIAGNOSTIC.
    h.select(1);
    d.queueCommand(CMD_IDENTIFY_DEVICE);
    CHECK(!d.commandQueued());
    d.queueCommand(CMD_EXECUTE_DEVICE_DIAGNOSTIC);
    CHECK(d.commandQueued());
    d.runQueuedCommand();
    CHECK_EQ(d.lastCommand().command, CMD_EXECUTE_DEVICE_DIAGNOSTIC);
    // A soft or hard reset drops a queued command.
    h.select(0);
    d.queueCommand(CMD_IDENTIFY_DEVICE);
    h.softReset();
    CHECK(!d.commandQueued());
    d.queueCommand(CMD_IDENTIFY_DEVICE);
    d.write(Reg::AltStatusDevCtrl, DC_SRST);  // reset held: still dropped before release
    CHECK(!d.commandQueued());
    d.write(Reg::AltStatusDevCtrl, 0);
    d.queueCommand(CMD_IDENTIFY_DEVICE);
    d.hardReset(true);
    d.hardReset(false);
    CHECK(!d.commandQueued());
    uint32_t started = d.commandsStarted();
    d.runQueuedCommand();
    CHECK_EQ(d.commandsStarted(), started);
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
    const uint8_t cmds[] = {CMD_CHECK_POWER_MODE, CMD_IDLE_IMMEDIATE, 0x10, 0x70, CMD_SET_MULTIPLE_MODE};
    for (uint8_t c : cmds) command(d, c);
    CHECK_EQ(d.commandsStarted(), 5u);
    CHECK_EQ(d.commandsCompleted(), 5u);  // each ends with a DONE line
}

TEST(test_flush_cache_flushes_the_storage) {
    Device d(INFO);
    FlushCounter disk;
    VirtualHost h{d};
    CHECK_EQ(command(d, CMD_FLUSH_CACHE), ST_BSY | ST_DRDY | ST_DSC);  // BSY while core 0 flushes
    CHECK(!d.intrq());
    Fetch f;
    CHECK(d.pendingFetch(f));
    CHECK(f.flush);
    CHECK(!f.store);
    d.fetchDone(f.id, disk.flush());
    CHECK_EQ(disk.flushes, 1);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK(d.intrq());
    CHECK_EQ(d.commandsCompleted(), d.commandsStarted());

    disk.fail = true;  // the card couldn't
    command(d, CMD_FLUSH_CACHE);
    CHECK(d.pendingFetch(f));
    d.fetchDone(f.id, disk.flush());
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC | ST_ERR);
    CHECK_EQ(d.error(), ER_ABRT);

    command(d, CMD_FLUSH_CACHE);  // a reset makes a late flush result stale
    CHECK(d.pendingFetch(f));
    d.hardReset(true);
    d.hardReset(false);
    d.fetchDone(f.id, true);
    CHECK_EQ(h.altStatus(), ST_DRDY | ST_DSC);
    CHECK(!d.pendingFetch(f));
}

TEST(test_flush_cache_through_the_virtual_host) {
    Device d(INFO);
    FlushCounter disk;
    VirtualHost h{d, &disk};  // serves fetches, flushes included, when polled
    d.write(Reg::StatusCommand, CMD_FLUSH_CACHE);
    CHECK(h.waitNotBusy());
    CHECK_EQ(disk.flushes, 1);
    CHECK_EQ(h.status(), ST_DRDY | ST_DSC);
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
    RUN(test_ns1081_detection);
    RUN(test_generated_disk_words);
    RUN(test_read_one_sector);
    RUN(test_read_busy_until_fetched);
    RUN(test_read_multiple_sectors_interrupt_each);
    RUN(test_fast_lane_matches_data_reads);
    RUN(test_fast_lane_only_for_a_data_in_block);
    RUN(test_read_ahead_hides_the_fetch);
    RUN(test_read_ahead_failure_reported_at_its_sector);
    RUN(test_read_ahead_stale_after_reset);
    RUN(test_command_abandons_unread_block);
    RUN(test_command_abandons_unread_identify);
    RUN(test_reset_ends_the_running_command);
    RUN(test_read_ahead_off);
    RUN(test_read_count_zero_means_256);
    RUN(test_read_last_sector_and_past_the_end);
    RUN(test_read_chs);
    RUN(test_read_fetch_failure);
    RUN(test_overlay_disk);
    RUN(test_image_disk);
    RUN(test_read_and_write_through_an_image);
    RUN(test_write_one_sector);
    RUN(test_write_multiple_sectors_interrupt_each);
    RUN(test_write_behind_overlaps_the_store);
    RUN(test_write_behind_store_failure);
    RUN(test_write_behind_through_the_virtual_host);
    RUN(test_write_count_zero_means_256);
    RUN(test_write_past_the_end);
    RUN(test_write_store_failure);
    RUN(test_data_writes_outside_a_write_are_ignored);
    RUN(test_reset_mid_write_makes_store_stale);
    RUN(test_reset_mid_read_makes_fetch_stale);
    RUN(test_read_with_device1_selected_is_ignored);
    RUN(test_read_across_cores);
    RUN(test_set_features_pio_modes_offered);
    RUN(test_set_features_transfer_mode);
    RUN(test_set_features_traced_with_its_subcommand);
    RUN(test_set_features_other_subcommands_abort);
    RUN(test_initialize_device_parameters);
    RUN(test_initialize_device_parameters_survives_soft_reset_not_hard);
    RUN(test_initialize_device_parameters_rejects_zero_sectors);
    RUN(test_read_verify);
    RUN(test_seek_and_recalibrate);
    RUN(test_power_commands);
    RUN(test_queued_command);
    RUN(test_queued_command_rules);
    RUN(test_reset_cancels_waking);
    RUN(test_set_multiple_mode_aborts);
    RUN(test_housekeeping_commands_complete_for_the_trace);
    RUN(test_flush_cache_flushes_the_storage);
    RUN(test_flush_cache_through_the_virtual_host);
    RUN(test_write_cache_on_and_off);
    RUN(test_virtual_host_bridge_setup);
}
