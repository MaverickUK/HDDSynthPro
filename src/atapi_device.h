// The CD-ROM build's device (docs/atapi.md): an ATAPI CD-ROM drive serving a
// data disc, in place of ata::Device. Pure C++, host-tested. Runs on core 1
// behind the same interface as ata::Device, so ide_bus and loop1 drive it
// unchanged (drive_device.h picks one per build); the write and standby parts
// of that interface are there but never used.
//
// What makes it a packet device:
//   - its reset signature (LBA Mid/High 14h/EBh), and IDENTIFY DEVICE aborted;
//   - IDENTIFY PACKET DEVICE (A1h) in its place;
//   - PACKET (A0h): the host writes a 12-byte command packet through the data
//     register, then the device answers it (mmc.h) with data blocks of up to
//     the byte count the host allows (LBA Mid/High), an interrupt for each,
//     and an interrupt at the end; Sector Count holds the interrupt reason.
//     With PACKET's DMA bit (multiword DMA, when IDENTIFY PACKET DEVICE
//     offers it) the packet still comes by PIO, but the data goes in DMA
//     bursts, one per block, with no byte count limit and no interrupt until
//     the status phase: ide_dma moves them, as for the hard disk's READ DMA.
//
// A disc read goes one 512-byte DRQ block per fetch, as the hard disk does:
// CD frame n is sectors 4n to 4n+3 of the image, so ImageDisk serves an ISO
// unchanged and the fast lane serves its blocks. With read-ahead (on unless
// console `A` or `read_ahead = off`), the next sector is fetched into the
// other buffer while the host reads one, so it's usually in at once; off,
// each is fetched under BSY once the host has read the one before.
#pragma once

#include <cstdint>

#include "ata_device.h"
#include "identify.h"
#include "mmc.h"

namespace atapi {

using ata::Reg;

// Commands a packet device handles beyond the hard disk's.
constexpr uint8_t CMD_DEVICE_RESET = 0x08;
constexpr uint8_t CMD_PACKET = 0xA0;
constexpr uint8_t CMD_IDENTIFY_PACKET_DEVICE = 0xA1;

// Interrupt reason (Sector Count during PACKET)
constexpr uint8_t IR_COD = 0x01;  // a command packet (1), or data (0)
constexpr uint8_t IR_IO = 0x02;   // to the host (1), or from it (0)

constexpr uint8_t FEATURES_DMA = 0x01;  // PACKET's Features bit 0: the data by DMA (if offered)

// 512-byte sectors of the image per CD frame.
constexpr uint32_t SECTORS_PER_FRAME = mmc::FRAME_BYTES / 512;

// What the trace reports about a command: ata::CommandInfo's fields, and for
// PACKET the packet's opcode, with a READ's frames in `lba` and `count`.
// (A struct of its own, so ata::Device's stays as it was.)
struct CommandInfo {
    uint8_t command;
    uint32_t lba;
    uint16_t count;
    uint8_t packet;
};

class Device {
public:
    // `info.total_sectors`: the image's 512-byte sectors (four per frame), or
    // 0 for no disc in the drive. `position`: 0, the only drive (the CD-ROM
    // build), or 1, device 1 beside a hard disk (hdcd::Device, the bb48_hdcd
    // build): it answers when DEV is 1, its fetches carry unit 1, and their
    // ids have the top bit set, apart from the hard disk's.
    explicit Device(const IdentifyInfo& info, uint8_t position = 0);
    // Whether the host has this drive selected (DEV matches its position).
    bool selected() const { return !notSelected(); }

    // ---- Bus side (as ata::Device) ------------------------------------------

    uint16_t read(Reg reg) {
        uint16_t value = peek(reg);
        consumeRead(reg);
        return value;
    }
    uint16_t peek(Reg reg) const;
    void consumeRead(Reg reg);

    __attribute__((always_inline)) bool dataInRemaining(const uint16_t*& next, uint16_t& left) const {
        if (notSelected() || !(status_ & ata::ST_DRQ) || !data_ || dma_) return false;
        next = data_ + data_index_;
        left = uint16_t(data_count_ - data_index_);
        return true;
    }
    void consumeDataWords(uint16_t n);

    void write(Reg reg, uint16_t value);

    // A Command write, and (here also) a command packet's last word: queued
    // under BSY, then run as core 1's next job.
    void queueCommand(uint8_t command);
    bool commandQueued() const { return command_queued_ || packet_queued_; }
    void runQueuedCommand();

    void setReadAhead(bool on) { read_ahead_ = on; }
    void setWriteBehind(bool) {}
    bool dataOutHeld() const { return false; }
    void releaseDataOut() {}

    void hardReset(bool asserted);

    bool intrq() const;
    bool intrqDriven() const { return !notSelected() && !(device_control_ & ata::DC_NIEN); }

    // ---- Sector fetches (handed to core 0) ---------------------------------

    bool pendingFetch(ata::Fetch& fetch);
    uint32_t pendingFetchId() const { return fetch_pending_ ? (fetch_id_ | id_tag_) : 0; }
    void fetchDone(uint32_t id, bool ok);

    // ---- DMA (PACKET with its DMA bit) ----------------------------------------

    // True while a packet's data goes by DMA: core 1 keeps DMARQ an output.
    bool dmaCommand() const { return dma_; }
    // A block ready for core 1 to move (ide_dma), or false (BSY: a fetch first);
    // dmaBurstDone() once the host has taken every word.
    bool dmaBurstReady(ata::DmaBurst& burst);
    void dmaBurstDone();

    // ---- Not offered: standby -------------------------------------------------

    bool waking() const { return false; }
    void finishWake() {}
    bool standby() const { return false; }

    // ---- State for tests and the trace ---------------------------------------

    uint8_t status() const { return status_; }
    uint8_t error() const { return error_; }
    uint32_t commandsStarted() const { return commands_started_; }
    uint32_t commandsCompleted() const { return commands_completed_; }
    // PACKET counts as started once its packet is in.
    const CommandInfo& lastCommand() const { return last_; }
    // A PIO DRQ block (core 1 then only serves the bus); not a DMA one.
    bool transferring() const { return !dma_ && (data_ != nullptr || receiving_packet_); }
    uint32_t discFrames() const { return disc_.frames; }
    const mmc::Sense& sense() const { return mmc_.sense; }

private:
    bool notSelected() const { return (device_ & ata::DEV_DEV) != dev_bit_; }
    bool acceptsCommand(uint8_t command) const;
    void resetToSignature(uint8_t device = 0);  // `device`: the Device register after it
    void abandonCommand() { commands_completed_ = commands_started_; }
    void beginCommand(uint8_t command);  // counted as started, for the trace
    void startCommand(uint8_t command);
    void startPacket();
    void runPacket();
    void setFeatures();
    void abortWithSignature();  // IDENTIFY DEVICE and READ SECTORS: not a hard disk
    void completeNoData();      // an ATA command's success, with an interrupt
    void fail(uint8_t error);   // an ATA command's error, with an interrupt
    void packetDone();          // a packet's status phase: good
    void packetFail();          // ...CHECK CONDITION, with mmc_.sense
    void startDataIn(const uint16_t* words, uint16_t count);
    void sendBlock();           // the next DRQ block of a packet's reply or sector
    void finishDataIn();
    void requestFetch(bool busy = true);  // busy: BSY until it's in
    void deliverSector(bool ok);          // a fetched sector to the host, or its error

    IdentifyInfo info_;
    uint8_t position_;     // 0 or 1: which device it is
    uint8_t dev_bit_;      // the Device register's DEV bit that selects it
    uint32_t id_tag_;      // or'ed into fetch ids: 0, or the top bit as device 1
    uint16_t identify_[256];
    // A fetched sector, and a packet's reply, apart: after DEVICE RESET cuts a
    // read short, core 0 may still be filling the sector buffer for the stale
    // fetch. Bytes in bus order: byte 2n is word n's low byte, as on a
    // little-endian CPU.
    uint16_t sectors_[2][ata::SECTOR_WORDS];  // read-ahead fills one while the host reads the other
    uint16_t reply_[mmc::MAX_REPLY / 2];
    mmc::Disc disc_;
    mmc::State mmc_;

    // Task file
    uint8_t features_ = 0;
    uint8_t sector_count_ = 0;  // during PACKET: the interrupt reason
    uint8_t lba_low_ = 0;
    uint8_t lba_mid_ = 0;       // during PACKET: the byte count, low
    uint8_t lba_high_ = 0;      // ...and high
    uint8_t device_ = 0;
    uint8_t status_ = 0;
    uint8_t error_ = 0;
    uint8_t device_control_ = 0;

    // Data-in block (device to host)
    const uint16_t* data_ = nullptr;
    uint16_t data_count_ = 0;
    uint16_t data_index_ = 0;

    // PACKET
    bool receiving_packet_ = false;  // DRQ for the command packet
    uint8_t packet_words_ = 0;
    uint8_t packet_[mmc::PACKET_BYTES] = {};
    bool packet_queued_ = false;     // all in: BSY until runQueuedCommand()
    bool packet_command_ = false;    // the data-in belongs to a packet (blocks, interrupt reason)
    uint16_t limit_ = 0;             // the most bytes the host takes per DRQ block
    const uint16_t* buf_ = nullptr;  // what's being sent: one of sectors_, or reply_
    uint16_t buf_bytes_ = 0;         // its length in bytes
    uint16_t buf_pos_ = 0;           // ...sent so far
    uint16_t block_bytes_ = 0;       // the block being read
    bool dma_ = false;               // the packet's data by DMA (Features bit 0)
    bool dma_burst_running_ = false; // between dmaBurstReady() and dmaBurstDone()

    // Disc read in progress, in the image's 512-byte sectors
    bool reading_ = false;
    uint32_t read_lba_ = 0;   // the next sector to fetch
    uint32_t read_left_ = 0;  // sectors not yet fetched
    bool read_ahead_ = true;
    // A read-ahead that finished while the host was still taking the sector
    // before: it worked, or it failed, to hand over when that one's done.
    bool ahead_ok_ = false;
    bool ahead_failed_ = false;
    uint32_t command_lba_ = 0;
    uint16_t command_sectors_ = 0;

    bool fetch_pending_ = false;
    uint32_t fetch_id_ = 0;
    uint32_t fetch_lba_ = 0;  // the sector it reads
    uint8_t fetch_buf_ = 0;   // into sectors_[fetch_buf_]

    bool irq_pending_ = false;
    bool command_queued_ = false;
    uint8_t queued_command_ = 0;
    bool in_reset_ = false;

    CommandInfo last_{};
    uint32_t commands_started_ = 0;
    uint32_t commands_completed_ = 0;
};

// Inline: on core 1 it sits between a read arriving and its answer.
__attribute__((always_inline)) inline uint16_t Device::peek(Reg reg) const {
    switch (reg) {
        case Reg::Data:
            if (notSelected() || !(status_ & ata::ST_DRQ) || !data_) return 0;
            return data_[data_index_];
        case Reg::ErrorFeatures: return error_;
        case Reg::SectorCount: return sector_count_;
        case Reg::LbaLow: return lba_low_;
        case Reg::LbaMid: return lba_mid_;
        case Reg::LbaHigh: return lba_high_;
        case Reg::Device: return device_;
        case Reg::StatusCommand:
        case Reg::AltStatusDevCtrl: return notSelected() ? 0 : status_;
        default: return 0;
    }
}

}  // namespace atapi
