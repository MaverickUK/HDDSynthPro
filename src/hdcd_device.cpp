#include "hdcd_device.h"

#include "core1_ram.h"

namespace hdcd {

Device::Device(const IdentifyInfo& disk0, const IdentifyInfo* disk1, const IdentifyInfo* cdrom, uint8_t cd_unit)
    : hd_(disk0, cdrom ? nullptr : disk1),
      cd_(cdrom ? *cdrom : disk0, cdrom && cd_unit == 0 ? 0 : 1),
      cd_on_(cdrom != nullptr),
      cd_unit_(cdrom && cd_unit == 0 ? 0 : 1),
      hd_on_(!(cdrom && cd_unit == 0)),
      cd_mask_(!cdrom ? 0 : cd_unit == 0 ? 0 : ata::DEV_DEV),
      cd_match_(!cdrom ? 1 : cd_unit == 0 ? 0 : ata::DEV_DEV) {}  // no CD-ROM: 0 never equals 1

// Commands start only here and in write(Command): which device started the
// latest, for lastCommand(). EXECUTE DEVICE DIAGNOSTIC starts on both: the
// disk's, device 0 reporting.
CORE1_RAM void Device::noteStarts(uint32_t hd_before, uint32_t cd_before) {
    if (hd_.commandsStarted() != hd_before)
        cd_last_ = false;
    else if (cd_.commandsStarted() != cd_before)
        cd_last_ = true;
}

CORE1_RAM void Device::resyncDevice() {
    if (!cd_on_) return;  // the disk alone keeps its own register, as in bb48_drive
    if (hd_on_) hd_.write(Reg::Device, device_reg_);  // ignored by one that's busy: it won't take a command either
    cd_.write(Reg::Device, device_reg_);
}

CORE1_RAM void Device::write(Reg reg, uint16_t value) {
    uint8_t v = uint8_t(value);
    if (reg == Reg::Device) {
        device_reg_ = v;
    } else if (reg == Reg::AltStatusDevCtrl) {
        if ((device_control_ & ata::DC_SRST) && !(v & ata::DC_SRST)) device_reg_ = 0;  // SRST released
        device_control_ = v;
    } else if (reg == Reg::StatusCommand) {
        resyncDevice();
    }
    uint32_t hd_before = hd_.commandsStarted(), cd_before = cd_.commandsStarted();
    if (hd_on_) hd_.write(reg, value);
    if (cd_on_) cd_.write(reg, value);
    noteStarts(hd_before, cd_before);
    if (reg == Reg::StatusCommand && v == ata::CMD_EXECUTE_DEVICE_DIAGNOSTIC) device_reg_ = 0;
}

CORE1_RAM void Device::queueCommand(uint8_t command) {
    resyncDevice();
    if (hd_on_) hd_.queueCommand(command);  // each takes it only when selected (or for EXECUTE DEVICE DIAGNOSTIC)
    if (cd_on_) cd_.queueCommand(command);
    if (command == ata::CMD_EXECUTE_DEVICE_DIAGNOSTIC) diag_queued_ = true;
}

CORE1_RAM void Device::runQueuedCommand() {
    uint32_t hd_before = hd_.commandsStarted(), cd_before = cd_.commandsStarted();
    if (hd_.commandQueued()) hd_.runQueuedCommand();
    if (cd_on_ && cd_.commandQueued()) cd_.runQueuedCommand();
    noteStarts(hd_before, cd_before);
    if (diag_queued_) {
        diag_queued_ = false;
        device_reg_ = 0;  // both reset to their signatures, device 0 selected
    }
}

CORE1_RAM void Device::hardReset(bool asserted) {
    if (hd_on_) hd_.hardReset(asserted);
    if (cd_on_) cd_.hardReset(asserted);
    if (!asserted) {
        device_reg_ = 0;
        device_control_ = 0;
        diag_queued_ = false;
    }
}

CORE1_RAM bool Device::pendingFetch(ata::Fetch& fetch) {
    return hd_.pendingFetch(fetch) || (cd_on_ && cd_.pendingFetch(fetch));
}

CORE1_RAM void Device::fetchDone(uint32_t id, bool ok) {
    if (hd_on_) hd_.fetchDone(id, ok);  // each takes only its own id: the CD-ROM's as device 1 have the top bit set
    if (cd_on_) cd_.fetchDone(id, ok);
}

CORE1_RAM bool Device::dmaBurstReady(ata::DmaBurst& burst) {
    return hd_.dmaBurstReady(burst) || (cd_on_ && cd_.dmaBurstReady(burst));
}

CORE1_RAM void Device::dmaBurstDone() {
    if (hd_on_) hd_.dmaBurstDone();  // only the one with a burst running does anything
    if (cd_on_) cd_.dmaBurstDone();
}

CORE1_RAM const CommandInfo& Device::lastCommand() const {
    if (cd_last_) {
        const atapi::CommandInfo& c = cd_.lastCommand();
        last_ = CommandInfo{c.command, c.lba, c.count, cd_unit_, c.packet};
    } else {
        const ata::CommandInfo& c = hd_.lastCommand();
        last_ = CommandInfo{c.command, c.lba, c.count, c.unit, 0};
    }
    return last_;
}

}  // namespace hdcd
