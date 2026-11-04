// The bb48_hdcd build's device: a hard disk as device 0 and, when
// HDDSYNTH.INI's `device1 = cdrom` asks for one, a CD-ROM drive as device 1
// (docs/two-drives.md, docs/atapi.md). Pure C++, host-tested. Runs on core 1
// behind the same interface as ata::Device (drive_device.h picks it).
//
// Two devices on one cable, as on a real bus: both take every write to the
// task file, Device and Device Control registers, and RESET#; the selected
// one answers reads, runs commands and drives INTRQ. So this passes writes
// to both and everything else to the one selected. Which one is selected is
// kept here, from every Device register write: a device ignores writes while
// it's busy, so its own copy can lag (the host selecting the disk while the
// CD-ROM fetches), and before each command both are given the register again,
// so neither runs a command meant for the other. Without a CD-ROM it is
// the hard disk build's ata::Device (with its own device 1, a second image
// or a folder, if any), plus one branch per access.
//
// Fetches: the CD-ROM's carry unit 1 (core 0 reads device 1's disc) and ids
// with the top bit set, so a result goes back to the device that asked: each
// is offered both, and each takes only its own id. (The hard disk's ids
// would reach the top bit only after 2^31 fetches, a terabyte or more.)
#pragma once

#include <cstdint>

#include "ata_device.h"
#include "atapi_device.h"
#include "identify.h"

namespace hdcd {

using ata::Reg;

// What the trace reports about a command: which device ran it, and for the
// CD-ROM's PACKET the packet's opcode.
struct CommandInfo {
    uint8_t command;
    uint32_t lba;
    uint16_t count;
    uint8_t unit;    // 0: device 0, 1: device 1 (a second disk or the CD-ROM)
    uint8_t packet;  // PACKET's opcode (the CD-ROM), else 0
};

class Device {
public:
    // `disk1`: device 1 as a second hard disk, or nullptr; `cdrom`: device 1
    // as a CD-ROM drive (its `total_sectors` the disc's, 0 for no disc), or
    // nullptr. Not both: `cdrom` wins.
    Device(const IdentifyInfo& disk0, const IdentifyInfo* disk1, const IdentifyInfo* cdrom);
    __attribute__((always_inline)) bool hasCdrom() const { return cd_on_; }

    // ---- Bus side (as ata::Device) ------------------------------------------

    // The CD-ROM is selected: it answers. Always inlined, as every access asks
    // (core 1's code must stay in RAM, as for ata::Device).
    __attribute__((always_inline)) bool cdSelected() const { return cd_on_ && (device_reg_ & ata::DEV_DEV); }

    __attribute__((always_inline)) uint16_t read(Reg reg) {
        uint16_t value = peek(reg);
        consumeRead(reg);
        return value;
    }
    __attribute__((always_inline)) uint16_t peek(Reg reg) const {
        return cdSelected() ? cd_.peek(reg) : hd_.peek(reg);
    }
    __attribute__((always_inline)) void consumeRead(Reg reg) {
        if (cdSelected())
            cd_.consumeRead(reg);
        else
            hd_.consumeRead(reg);
    }
    __attribute__((always_inline)) bool dataInRemaining(const uint16_t*& next, uint16_t& left) const {
        return cdSelected() ? cd_.dataInRemaining(next, left) : hd_.dataInRemaining(next, left);
    }
    __attribute__((always_inline)) void consumeDataWords(uint16_t n) {
        if (cdSelected())
            cd_.consumeDataWords(n);
        else
            hd_.consumeDataWords(n);
    }

    // Both devices see every write; a Command write is run by the one selected
    // (and EXECUTE DEVICE DIAGNOSTIC by both).
    void write(Reg reg, uint16_t value);
    void queueCommand(uint8_t command);
    __attribute__((always_inline)) bool commandQueued() const { return hd_.commandQueued() || (cd_on_ && cd_.commandQueued()); }
    void runQueuedCommand();

    __attribute__((always_inline)) void setReadAhead(bool on) { hd_.setReadAhead(on); }
    __attribute__((always_inline)) void setWriteBehind(bool on) { hd_.setWriteBehind(on); }
    __attribute__((always_inline)) bool dataOutHeld() const { return hd_.dataOutHeld(); }
    __attribute__((always_inline)) void releaseDataOut() { hd_.releaseDataOut(); }

    void hardReset(bool asserted);

    __attribute__((always_inline)) bool intrq() const { return cdSelected() ? cd_.intrq() : hd_.intrq(); }
    __attribute__((always_inline)) bool intrqDriven() const { return cdSelected() ? cd_.intrqDriven() : hd_.intrqDriven(); }

    // ---- Sector fetches (handed to core 0) ---------------------------------

    bool pendingFetch(ata::Fetch& fetch);
    __attribute__((always_inline)) uint32_t pendingFetchId() const {
        uint32_t id = hd_.pendingFetchId();
        return id || !cd_on_ ? id : cd_.pendingFetchId();
    }
    void fetchDone(uint32_t id, bool ok);

    // ---- DMA -------------------------------------------------------------------

    __attribute__((always_inline)) bool dmaCommand() const { return hd_.dmaCommand() || (cd_on_ && cd_.dmaCommand()); }
    bool dmaBurstReady(ata::DmaBurst& burst);
    void dmaBurstDone();

    // ---- The CD-ROM's disc (atapi::Device's) ---------------------------------

    bool discIdle() const { return cd_on_ && cd_.idle(); }
    void changeDisc(uint32_t total_sectors) {
        if (cd_on_) cd_.changeDisc(total_sectors);
    }

    // ---- The hard disk's alone ---------------------------------------------------

    __attribute__((always_inline)) bool waking() const { return hd_.waking(); }
    __attribute__((always_inline)) void finishWake() { hd_.finishWake(); }
    __attribute__((always_inline)) bool standby() const { return hd_.standby(); }

    // ---- State for tests and the trace ---------------------------------------

    __attribute__((always_inline)) uint8_t status() const { return cdSelected() ? cd_.status() : hd_.status(); }
    __attribute__((always_inline)) uint8_t error() const { return cdSelected() ? cd_.error() : hd_.error(); }
    __attribute__((always_inline)) uint32_t commandsStarted() const { return hd_.commandsStarted() + cd_.commandsStarted(); }
    __attribute__((always_inline)) uint32_t commandsCompleted() const { return hd_.commandsCompleted() + cd_.commandsCompleted(); }
    // The command started last, by either device.
    const CommandInfo& lastCommand() const;
    __attribute__((always_inline)) bool transferring() const { return cdSelected() ? cd_.transferring() : hd_.transferring(); }

    const ata::Device& disk() const { return hd_; }
    const atapi::Device& cdrom() const { return cd_; }

private:
    ata::Device hd_;
    atapi::Device cd_;
    bool cd_on_;
    void noteStarts(uint32_t hd_before, uint32_t cd_before);
    void resyncDevice();  // both devices told the Device register again, before a command
    bool cd_last_ = false;  // the CD-ROM started the latest command (else the disk)
    uint8_t device_reg_ = 0;     // the Device register as the host last wrote it (0 after a reset)
    uint8_t device_control_ = 0;  // Device Control, for SRST's release
    bool diag_queued_ = false;   // EXECUTE DEVICE DIAGNOSTIC queued: both then select device 0
    mutable CommandInfo last_{};
};

}  // namespace hdcd
