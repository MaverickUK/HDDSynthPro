// The emulated drive's ATA register file and command state machine.
// Pure C++, host-tested. Runs on core 1: the bus layer calls read() and
// write() for every register access the host makes, so both must stay fast
// (no allocation, no I/O, no waiting).
//
// One device, configured as device 0 (master), with no device 1 on the
// cable. ATA lets device 0 answer for an absent device 1, which this does:
// see read() and write().
//
// Sector data lives elsewhere (core 0: a generated disk with a RAM overlay
// now, the SD card later). While core 0 moves a sector the device shows BSY,
// as a real drive does while it seeks.
// Reads:
//   1. A read command sets BSY and posts a Fetch (pendingFetch()).
//   2. Core 0 fills fetch.buffer with the sector, then calls fetchDone().
//   3. The device sets DRQ and the host reads the 256 words.
//   4. Repeat per sector; the last one ends the command.
// Writes (WRITE SECTORS) run the other way:
//   1. The command sets DRQ (no interrupt) and the host writes 256 words.
//   2. The device sets BSY and posts a Fetch with `store` set.
//   3. Core 0 writes fetch.buffer to the disk, then calls fetchDone().
//   4. DRQ and an interrupt for the next sector, or, after the last, the
//      command completes with an interrupt.
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
constexpr uint8_t ER_UNC = 0x40;   // uncorrectable data error (a read's fetch failed)
constexpr uint8_t ER_IDNF = 0x10;  // address not found (out of range)
constexpr uint8_t ER_ABRT = 0x04;  // command aborted (not supported, or a write's store failed)

// Device register bits
constexpr uint8_t DEV_DEV = 0x10;  // 1 = device 1 (slave) selected
constexpr uint8_t DEV_LBA = 0x40;  // 1 = LBA addressing, 0 = CHS

// Device Control register bits
constexpr uint8_t DC_SRST = 0x04;
constexpr uint8_t DC_NIEN = 0x02;  // 1 = INTRQ disabled

// Commands handled so far. Anything else is aborted (ABRT).
constexpr uint8_t CMD_RECALIBRATE = 0x10;  // 10h-1Fh: obsolete, all alike
constexpr uint8_t CMD_READ_SECTORS = 0x20;
constexpr uint8_t CMD_READ_SECTORS_NO_RETRY = 0x21;  // obsolete alias
constexpr uint8_t CMD_WRITE_SECTORS = 0x30;
constexpr uint8_t CMD_WRITE_SECTORS_NO_RETRY = 0x31;  // obsolete alias
constexpr uint8_t CMD_READ_VERIFY = 0x40;
constexpr uint8_t CMD_READ_VERIFY_NO_RETRY = 0x41;
constexpr uint8_t CMD_SEEK = 0x70;  // 70h-7Fh: obsolete, all alike
constexpr uint8_t CMD_EXECUTE_DEVICE_DIAGNOSTIC = 0x90;
constexpr uint8_t CMD_INITIALIZE_DEVICE_PARAMETERS = 0x91;
constexpr uint8_t CMD_SET_MULTIPLE_MODE = 0xC6;  // aborted: no READ/WRITE MULTIPLE
constexpr uint8_t CMD_STANDBY_IMMEDIATE = 0xE0;
constexpr uint8_t CMD_IDLE_IMMEDIATE = 0xE1;
constexpr uint8_t CMD_STANDBY = 0xE2;
constexpr uint8_t CMD_IDLE = 0xE3;
constexpr uint8_t CMD_CHECK_POWER_MODE = 0xE5;
constexpr uint8_t CMD_FLUSH_CACHE = 0xE7;  // BSY while core 0 flushes the storage
constexpr uint8_t CMD_IDENTIFY_DEVICE = 0xEC;
constexpr uint8_t CMD_SET_FEATURES = 0xEF;
// The ATA-1 era opcodes for the power commands (94h-98h), still sent by some hosts.
constexpr uint8_t CMD_STANDBY_IMMEDIATE_OLD = 0x94;
constexpr uint8_t CMD_IDLE_IMMEDIATE_OLD = 0x95;
constexpr uint8_t CMD_STANDBY_OLD = 0x96;
constexpr uint8_t CMD_IDLE_OLD = 0x97;
constexpr uint8_t CMD_CHECK_POWER_MODE_OLD = 0x98;

// SET FEATURES subcommand (Features register) and transfer mode values
// (Sector Count register) that this device accepts.
constexpr uint8_t SF_SET_TRANSFER_MODE = 0x03;
constexpr uint8_t SF_ENABLE_WRITE_CACHE = 0x02;
constexpr uint8_t SF_DISABLE_WRITE_CACHE = 0x82;
constexpr uint8_t XFER_PIO_DEFAULT = 0x00;  // PIO default mode
constexpr uint8_t XFER_PIO_FLOW_0 = 0x08;   // PIO flow-control mode 0 (with IORDY)

// CHECK POWER MODE results (Sector Count register)
constexpr uint8_t POWER_STANDBY = 0x00;
constexpr uint8_t POWER_ACTIVE_OR_IDLE = 0xFF;

constexpr uint16_t SECTOR_WORDS = 256;

// A sector the device needs core 0 to move: fetched into `buffer` for a read,
// or stored from it for a write (`store`). `id` changes with every request,
// so one that finishes after a reset (or a newer request) is recognised as
// stale and ignored.
struct Fetch {
    uint32_t id;
    uint32_t lba;
    uint16_t* buffer;  // SECTOR_WORDS words, in bus order (low byte first on disk)
    bool store;        // true: write `buffer` to `lba`; false: fill it from `lba`
    bool flush;        // true: then make everything stored so far permanent. Alone: FLUSH
                       // CACHE; with store: a write with the write cache off
};

// What the trace reports about a command.
struct CommandInfo {
    uint8_t command;
    uint32_t lba;    // decoded start address (LBA, or CHS converted); SET FEATURES: the subcommand
    uint16_t count;  // sectors (0 in the register means 256); SET FEATURES: its Sector Count
};

class Device {
public:
    explicit Device(const IdentifyInfo& info);

    // ---- Bus side -----------------------------------------------------------

    // Host reads a register. Reading DATA during a data-in transfer returns
    // the next word; reading STATUS clears a pending interrupt.
    uint16_t read(Reg reg) {
        uint16_t value = peek(reg);
        consumeRead(reg);
        return value;
    }

    // read() in two halves, so the bus layer can answer the host first
    // (peek: what the read returns, no side effects) and catch up afterwards
    // (consumeRead: the next DATA word, or acknowledging the interrupt).
    uint16_t peek(Reg reg) const;
    void consumeRead(Reg reg);

    // Core 1's fast lane for a data-in block: the words the host has still to
    // read (`next`, `left`), if a block is being read from device 0. Answering
    // the next n DATA reads from them and then calling consumeDataWords(n) is
    // the same as n read(Reg::Data) calls. Any other access in between must
    // come after consumeDataWords().
    bool dataInRemaining(const uint16_t*& next, uint16_t& left) const {
        if (device1Selected() || !(status_ & ST_DRQ) || !data_) return false;
        next = data_ + data_index_;
        left = uint16_t(data_count_ - data_index_);
        return true;
    }
    void consumeDataWords(uint16_t n);

    // Host writes a register (only the low byte matters except for DATA).
    void write(Reg reg, uint16_t value);

    // The bus layer's way in for a Command write: queueCommand() only sets BSY
    // and keeps the command (nanoseconds), and runQueuedCommand() runs it as
    // core 1's next job. A status poll straight after the write then sees BSY
    // at once instead of waiting for the command to start, as with a real
    // drive. write(StatusCommand, ...) does both at once.
    void queueCommand(uint8_t command);
    bool commandQueued() const { return command_queued_; }
    void runQueuedCommand();

    // Read-ahead on (the default) or off, from the next read on. Off, each
    // sector after the first is fetched under BSY once the host has read the
    // previous one, as before read-ahead (console `A`, for comparisons).
    void setReadAhead(bool on) { read_ahead_ = on; }

    // RESET# asserted (true) or released (false).
    void hardReset(bool asserted);

    // Level the INTRQ line should have: true = assert.
    bool intrq() const;

    // ---- Sector fetches (handed to core 0) ---------------------------------

    // True while a fetch (or store) is wanted; fills `fetch`. Stays true
    // (same id) until fetchDone() or a reset.
    bool pendingFetch(Fetch& fetch);

    // Core 0 finished fetch `id`: the sector was moved (ok), or it failed.
    void fetchDone(uint32_t id, bool ok);

    // ---- Waking from standby -------------------------------------------------

    // True while IDLE (IMMEDIATE) is waking the drive from standby. The drive
    // shows BSY, as a real one does while it spins up, until finishWake()
    // completes the command; the caller times it (cfg::WAKE_BUSY_US).
    bool waking() const { return waking_; }
    void finishWake();

    // ---- State for tests and the trace ---------------------------------------

    uint8_t status() const { return status_; }
    uint8_t error() const { return error_; }
    // Counters: a change means a command started / completed since last look.
    uint32_t commandsStarted() const { return commands_started_; }
    uint32_t commandsCompleted() const { return commands_completed_; }
    const CommandInfo& lastCommand() const { return last_; }
    // The CHS translation in use: the default, or set by INITIALIZE DEVICE PARAMETERS.
    const Geometry& geometry() const { return geometry_; }
    bool standby() const { return standby_; }
    // True mid-sector: the host is reading or writing the 256 words of a DRQ block.
    bool transferring() const { return data_ != nullptr || receiving_; }

private:
    bool device1Selected() const { return (device_ & DEV_DEV) != 0; }
    bool acceptsCommand(uint8_t command) const;
    void resetToSignature();  // registers and status as after a reset
    void startCommand(uint8_t command);
    void startRead();
    void startWrite();
    void readVerify();
    void seek();
    void setFeatures();
    void initializeDeviceParameters();
    void setGeometry(const Geometry& g);
    // Decodes the task-file address and checks `count` sectors from it exist.
    // On failure, fails the command with IDNF and returns false.
    bool checkRange(uint32_t count, uint32_t& lba);
    bool decodeAddress(uint32_t& lba) const;  // false if a CHS address is invalid
    void completeNoData();  // success, with an interrupt
    void requestFetch(bool busy = true);  // busy: BSY until it's done
    void readAhead();                     // fetch the read's next sector, if any
    void deliverSector(bool ok);          // a fetched sector to the host, or its error
    void fail(uint8_t error);  // ERR + error bits, command complete
    void complete();           // success, no data left
    void startDataIn(const uint16_t* words, uint16_t count);
    void finishDataIn();
    void startDataOut();   // DRQ for the next sector of a write
    void finishDataOut();  // all 256 words in: BSY while core 0 stores them
    void setTaskFileLba(uint32_t lba);  // where an error happened

    IdentifyInfo info_;
    Geometry geometry_;
    uint16_t identify_[256];
    // Two sector buffers: while the host reads one, core 0 fills the other
    // with the next sector of the read (read-ahead). Writes use the first.
    uint16_t sectors_[2][SECTOR_WORDS];

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

    // Data-out transfer (host to device), into sectors_[0]
    bool receiving_ = false;
    uint16_t received_ = 0;  // words of the current sector so far

    // Multi-sector read in progress
    bool reading_ = false;
    uint32_t read_lba_ = 0;       // next sector to fetch
    uint32_t read_left_ = 0;      // sectors not yet fetched
    // Multi-sector write in progress
    bool writing_ = false;
    uint32_t write_lba_ = 0;      // next sector to store
    uint32_t write_left_ = 0;     // sectors not yet stored

    bool fetch_pending_ = false;  // a fetch or store is wanted, or under way
    uint32_t fetch_id_ = 0;
    uint32_t fetch_lba_ = 0;
    uint8_t fetch_buf_ = 0;       // which of sectors_ it fills or stores
    // A read-ahead that finished while the host was still reading the
    // previous sector, waiting to be handed over: it worked, or it failed.
    bool ahead_ok_ = false;
    bool ahead_failed_ = false;
    bool flushing_ = false;  // FLUSH CACHE: its fetch is a flush
    bool write_cache_ = true;
    bool read_ahead_ = true;  // SET FEATURES 02h/82h; off: each sector synced as it's stored

    bool irq_pending_ = false;
    bool command_queued_ = false;  // see queueCommand()
    uint8_t queued_command_ = 0;
    bool in_reset_ = false;  // RESET# held or SRST set: BSY until released
    bool standby_ = false;   // after STANDBY; any media access wakes it
    bool waking_ = false;    // IDLE from standby: BSY until finishWake()

    CommandInfo last_{};
    uint32_t commands_started_ = 0;
    uint32_t commands_completed_ = 0;
};

// Inline: on core 1 it sits between a read arriving and its answer.
// ATA: with device 1 selected and absent, device 0 returns 00h for Status and
// Alternate Status, so the host sees "no device".
__attribute__((always_inline)) inline uint16_t Device::peek(Reg reg) const {
    switch (reg) {
        case Reg::Data:
            if (device1Selected() || !(status_ & ST_DRQ) || !data_) return 0;
            return data_[data_index_];
        case Reg::ErrorFeatures: return error_;
        case Reg::SectorCount: return sector_count_;
        case Reg::LbaLow: return lba_low_;
        case Reg::LbaMid: return lba_mid_;
        case Reg::LbaHigh: return lba_high_;
        case Reg::Device: return device_;
        case Reg::StatusCommand:
        case Reg::AltStatusDevCtrl: return device1Selected() ? 0 : status_;
        default: return 0;
    }
}

}  // namespace ata
