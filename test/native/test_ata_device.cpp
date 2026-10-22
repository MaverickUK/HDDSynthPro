// Tests for the ATA register file and command state machine (ata_device),
// including a "virtual host" that plays a typical start-up probe.
#include <cstdint>

#include "ata_device.h"
#include "check.h"
#include "identify.h"

using namespace ata;

static const IdentifyInfo INFO{"HDDSYNTH PROTO", "HSP-000001", "0.2.0", 131072};

// ---- A minimal virtual host, as a host driver would talk to the drive ----

struct VirtualHost {
    Device& dev;
    uint32_t polls = 0;

    uint8_t status() { return uint8_t(dev.read(Reg::StatusCommand)); }
    uint8_t altStatus() { return uint8_t(dev.read(Reg::AltStatusDevCtrl)); }
    void select(int device) { dev.write(Reg::Device, device ? 0xB0 : 0xA0); }

    // Polls Alternate Status until BSY clears (no side effects), like a host
    // waiting for a command. Returns false if it never clears.
    bool waitNotBusy(int max_polls = 1000) {
        for (int i = 0; i < max_polls; ++i, ++polls)
            if (!(altStatus() & ST_BSY)) return true;
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
    CHECK_EQ(d.lastCommand(), CMD_IDENTIFY_DEVICE);
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
}
