#include "ata_device.h"

namespace ata {

Reg regFor(bool command_block, bool control_block, unsigned address) {
    if (command_block && !control_block) return static_cast<Reg>(address & 7);
    if (control_block && !command_block && address == 6) return Reg::AltStatusDevCtrl;
    return Reg::None;
}

Device::Device(const IdentifyInfo& info) : info_(info) {
    buildIdentify(info_, identify_);
    resetToSignature();
}

void Device::resetToSignature() {
    // The ATA device signature: what a host checks after a reset to tell an
    // ATA drive (0000h in the LBA mid/high pair) from ATAPI (EB14h).
    sector_count_ = 1;
    lba_low_ = 1;
    lba_mid_ = 0;
    lba_high_ = 0;
    device_ = 0;
    error_ = 0x01;  // diagnostic code: device 0 passed, device 1 absent
    status_ = ST_DRDY | ST_DSC;
    data_ = nullptr;
    data_count_ = data_index_ = 0;
    irq_pending_ = false;
}

uint16_t Device::read(Reg reg) {
    // ATA: with device 1 selected and absent, device 0 returns 00h for
    // Status and Alternate Status, so the host sees "no device".
    switch (reg) {
        case Reg::Data: {
            if (device1Selected() || !(status_ & ST_DRQ) || !data_) return 0;
            uint16_t word = data_[data_index_++];
            if (data_index_ >= data_count_) finishDataIn();
            return word;
        }
        case Reg::ErrorFeatures: return error_;
        case Reg::SectorCount: return sector_count_;
        case Reg::LbaLow: return lba_low_;
        case Reg::LbaMid: return lba_mid_;
        case Reg::LbaHigh: return lba_high_;
        case Reg::Device: return device_;
        case Reg::StatusCommand:
            if (device1Selected()) return 0;
            irq_pending_ = false;  // reading Status acknowledges the interrupt
            return status_;
        case Reg::AltStatusDevCtrl:
            return device1Selected() ? 0 : status_;  // no side effects
        default: return 0;
    }
}

void Device::write(Reg reg, uint16_t value) {
    uint8_t v = uint8_t(value);
    if (reg == Reg::AltStatusDevCtrl) {
        // Device Control is honoured even while busy: it is how a host
        // recovers a hung drive.
        bool was_srst = (device_control_ & DC_SRST) != 0;
        device_control_ = v;
        if (v & DC_SRST) {
            in_reset_ = true;
            status_ = ST_BSY;
            irq_pending_ = false;
        } else if (was_srst) {
            in_reset_ = false;
            resetToSignature();
        }
        return;
    }
    // The host must not write the command block while BSY; ignore it if it does.
    if (status_ & ST_BSY) return;

    switch (reg) {
        case Reg::Data: break;  // no data-out commands yet
        case Reg::ErrorFeatures: features_ = v; break;
        case Reg::SectorCount: sector_count_ = v; break;
        case Reg::LbaLow: lba_low_ = v; break;
        case Reg::LbaMid: lba_mid_ = v; break;
        case Reg::LbaHigh: lba_high_ = v; break;
        case Reg::Device: device_ = v; break;
        case Reg::StatusCommand:
            // Commands for the absent device 1 are not ours to run, except
            // EXECUTE DEVICE DIAGNOSTIC, which both devices act on.
            if (device1Selected() && v != CMD_EXECUTE_DEVICE_DIAGNOSTIC) return;
            startCommand(v);
            break;
        default: break;
    }
}

void Device::hardReset(bool asserted) {
    if (asserted) {
        in_reset_ = true;
        status_ = ST_BSY;
        irq_pending_ = false;
        device_control_ = 0;
    } else if (in_reset_) {
        in_reset_ = false;
        resetToSignature();
    }
}

bool Device::intrq() const {
    // INTRQ belongs to the selected device; nIEN masks it.
    return irq_pending_ && !device1Selected() && !(device_control_ & DC_NIEN);
}

void Device::startCommand(uint8_t command) {
    last_command_ = command;
    ++commands_started_;
    irq_pending_ = false;  // writing Command clears a pending interrupt
    error_ = 0;
    switch (command) {
        case CMD_IDENTIFY_DEVICE:
            startDataIn(identify_, 256);
            break;
        case CMD_EXECUTE_DEVICE_DIAGNOSTIC:
            resetToSignature();  // signature, error 01h, and an interrupt
            irq_pending_ = true;
            break;
        default:
            abortCommand();
            break;
    }
}

void Device::abortCommand() {
    error_ = ER_ABRT;
    status_ = ST_DRDY | ST_DSC | ST_ERR;
    irq_pending_ = true;
}

void Device::startDataIn(const uint16_t* words, uint16_t count) {
    data_ = words;
    data_count_ = count;
    data_index_ = 0;
    status_ = ST_DRDY | ST_DSC | ST_DRQ;
    irq_pending_ = true;  // PIO data-in: interrupt when each block is ready
}

void Device::finishDataIn() {
    data_ = nullptr;
    data_count_ = data_index_ = 0;
    status_ = ST_DRDY | ST_DSC;  // no interrupt at the end of a data-in transfer
}

}  // namespace ata
