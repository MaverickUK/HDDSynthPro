// The emulated drive's ATA register file and command state machine.
// Pure C++, host-tested. Runs on core 1: the bus layer calls read() and
// write() for every register access the host makes, so both must stay fast
// (no allocation, no I/O, no waiting).
//
// One device, configured as device 0 (master), with no device 1 on the
// cable. ATA lets device 0 answer for an absent device 1, which this does:
// see read() and write().
#pragma once

#include <cstdint>

#include "identify.h"

namespace ata {

// Register addresses as the bus decodes them (block + DA2-0).
enum class Reg : uint8_t {
    Data,          // CMD 0
    ErrorFeatures, // CMD 1: Error on read, Features on write
    SectorCount,   // CMD 2
    LbaLow,        // CMD 3 (Sector Number in CHS)
    LbaMid,        // CMD 4 (Cylinder Low)
    LbaHigh,       // CMD 5 (Cylinder High)
    Device,        // CMD 6 (Device/Head)
    StatusCommand, // CMD 7: Status on read, Command on write
    AltStatusDevCtrl, // CTL 6: Alternate Status on read, Device Control on write
    None,          // anything else: reads return 0, writes are ignored
};

// Maps a decoded bus access to a register.
Reg regFor(bool command_block, bool control_block, unsigned address);

// Status register bits
constexpr uint8_t ST_BSY = 0x80;
constexpr uint8_t ST_DRDY = 0x40;
constexpr uint8_t ST_DF = 0x20;
constexpr uint8_t ST_DSC = 0x10;  // obsolete "seek complete"; older hosts expect it set
constexpr uint8_t ST_DRQ = 0x08;
constexpr uint8_t ST_ERR = 0x01;

// Error register bits
constexpr uint8_t ER_ABRT = 0x04;

// Device register bits
constexpr uint8_t DEV_DEV = 0x10;  // 1 = device 1 (slave) selected
constexpr uint8_t DEV_LBA = 0x40;

// Device Control register bits
constexpr uint8_t DC_SRST = 0x04;
constexpr uint8_t DC_NIEN = 0x02;  // 1 = INTRQ disabled

// Commands handled so far
constexpr uint8_t CMD_EXECUTE_DEVICE_DIAGNOSTIC = 0x90;
constexpr uint8_t CMD_IDENTIFY_DEVICE = 0xEC;

class Device {
public:
    explicit Device(const IdentifyInfo& info);

    // Host reads a register. Reading DATA during a data-in transfer returns
    // the next word; reading STATUS clears a pending interrupt.
    uint16_t read(Reg reg);

    // Host writes a register (only the low byte matters except for DATA).
    void write(Reg reg, uint16_t value);

    // RESET# asserted (true) or released (false).
    void hardReset(bool asserted);

    // Level the INTRQ line should have: true = assert.
    bool intrq() const;

    // State for tests and the trace.
    uint8_t status() const { return status_; }
    uint8_t error() const { return error_; }
    uint8_t lastCommand() const { return last_command_; }
    uint32_t commandsStarted() const { return commands_started_; }

private:
    bool device1Selected() const { return (device_ & DEV_DEV) != 0; }
    void resetToSignature();          // registers and status as after a reset
    void startCommand(uint8_t command);
    void abortCommand();              // ABRT error, command complete
    void startDataIn(const uint16_t* words, uint16_t count);
    void finishDataIn();

    IdentifyInfo info_;
    uint16_t identify_[256];

    // Task file
    uint8_t features_ = 0;
    uint8_t sector_count_ = 0;
    uint8_t lba_low_ = 0;
    uint8_t lba_mid_ = 0;
    uint8_t lba_high_ = 0;
    uint8_t device_ = 0;
    uint8_t status_ = 0;
    uint8_t error_ = 0;
    uint8_t device_control_ = 0;

    // Data-in transfer (device to host)
    const uint16_t* data_ = nullptr;
    uint16_t data_count_ = 0;
    uint16_t data_index_ = 0;

    bool irq_pending_ = false;
    bool in_reset_ = false;  // RESET# held or SRST set: BSY until released

    uint8_t last_command_ = 0;
    uint32_t commands_started_ = 0;
};

}  // namespace ata
