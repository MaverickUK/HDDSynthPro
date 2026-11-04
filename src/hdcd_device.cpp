#include "hdcd_device.h"

#include "core1_ram.h"

namespace hdcd {

Device::Device(const IdentifyInfo& disk0, const IdentifyInfo* disk1, const IdentifyInfo* cdrom)
    : hd_(disk0, cdrom ? nullptr : disk1), cd_(cdrom ? *cdrom : disk0, 1), cd_on_(cdrom != nullptr) {}

// Commands start only here and in write(Command): which device started the
// latest, for lastCommand(). EXECUTE DEVICE DIAGNOSTIC starts on both: the
// disk's, device 0 reporting.
CORE1_RAM void Device::noteStarts(uint32_t hd_before, uint32_t cd_before) {
    if (hd_.commandsStarted() != hd_before)
        cd_last_ = false;
    else if (cd_.commandsStarted() != cd_before)
        cd_last_ = true;
}

CORE1_RAM void Device::write(Reg reg, uint16_t value) {
    uint32_t hd_before = hd_.commandsStarted(), cd_before = cd_.commandsStarted();
    hd_.write(reg, value);
    if (cd_on_) cd_.write(reg, value);
    noteStarts(hd_before, cd_before);
}

CORE1_RAM void Device::queueCommand(uint8_t command) {
    hd_.queueCommand(command);  // each takes it only when selected (or for EXECUTE DEVICE DIAGNOSTIC)
    if (cd_on_) cd_.queueCommand(command);
}

CORE1_RAM void Device::runQueuedCommand() {
    uint32_t hd_before = hd_.commandsStarted(), cd_before = cd_.commandsStarted();
    if (hd_.commandQueued()) hd_.runQueuedCommand();
    if (cd_on_ && cd_.commandQueued()) cd_.runQueuedCommand();
    noteStarts(hd_before, cd_before);
}

CORE1_RAM void Device::hardReset(bool asserted) {
    hd_.hardReset(asserted);
    if (cd_on_) cd_.hardReset(asserted);
}

CORE1_RAM bool Device::pendingFetch(ata::Fetch& fetch) {
    return hd_.pendingFetch(fetch) || (cd_on_ && cd_.pendingFetch(fetch));
}

CORE1_RAM void Device::fetchDone(uint32_t id, bool ok) {
    hd_.fetchDone(id, ok);  // each takes only its own id: the CD-ROM's have the top bit set
    if (cd_on_) cd_.fetchDone(id, ok);
}

CORE1_RAM bool Device::dmaBurstReady(ata::DmaBurst& burst) {
    return hd_.dmaBurstReady(burst) || (cd_on_ && cd_.dmaBurstReady(burst));
}

CORE1_RAM void Device::dmaBurstDone() {
    hd_.dmaBurstDone();  // only the one with a burst running does anything
    if (cd_on_) cd_.dmaBurstDone();
}

CORE1_RAM const CommandInfo& Device::lastCommand() const {
    if (cd_last_) {
        const atapi::CommandInfo& c = cd_.lastCommand();
        last_ = CommandInfo{c.command, c.lba, c.count, 1, c.packet};
    } else {
        const ata::CommandInfo& c = hd_.lastCommand();
        last_ = CommandInfo{c.command, c.lba, c.count, c.unit, 0};
    }
    return last_;
}

}  // namespace hdcd
