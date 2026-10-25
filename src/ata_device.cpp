#include "ata_device.h"

#include "core1_ram.h"

namespace ata {

CORE1_RAM Reg regFor(bool command_block, bool control_block, unsigned address) {
    if (command_block && !control_block) return static_cast<Reg>(address & 7);
    if (control_block && !command_block && address == 6) return Reg::AltStatusDevCtrl;
    return Reg::None;
}

Device::Device(const IdentifyInfo& info)
    : info_(info), geometry_(defaultGeometry(info.total_sectors)) {
    buildIdentify(info_, identify_);
    resetToSignature();
}

CORE1_RAM void Device::resetToSignature() {
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
    waking_ = false;
    command_queued_ = false;
    // Abandon any read or write; a fetch still running on core 0 becomes stale.
    reading_ = false;
    writing_ = false;
    receiving_ = false;
    flushing_ = false;
    fetch_pending_ = false;
    ++fetch_id_;
}

CORE1_RAM void Device::consumeRead(Reg reg) {
    if (device1Selected()) return;  // device 0 answers 00h and keeps its state
    if (reg == Reg::Data) {
        if (!(status_ & ST_DRQ) || !data_) return;
        if (++data_index_ >= data_count_) finishDataIn();
    } else if (reg == Reg::StatusCommand) {
        irq_pending_ = false;  // reading Status acknowledges the interrupt
    }
}

CORE1_RAM void Device::consumeDataWords(uint16_t n) {
    if (n == 0 || device1Selected() || !(status_ & ST_DRQ) || !data_) return;
    data_index_ = uint16_t(data_index_ + n);
    if (data_index_ >= data_count_) finishDataIn();
}

CORE1_RAM void Device::write(Reg reg, uint16_t value) {
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
            command_queued_ = false;
        } else if (was_srst) {
            in_reset_ = false;
            resetToSignature();
        }
        return;
    }
    // The host must not write the command block while BSY; ignore it if it does.
    if (status_ & ST_BSY) return;

    switch (reg) {
        case Reg::Data:
            if (!receiving_ || device1Selected()) break;  // no data-out phase: ignored
            sectors_[0][received_++] = value;
            if (received_ >= SECTOR_WORDS) finishDataOut();
            break;
        case Reg::ErrorFeatures: features_ = v; break;
        case Reg::SectorCount: sector_count_ = v; break;
        case Reg::LbaLow: lba_low_ = v; break;
        case Reg::LbaMid: lba_mid_ = v; break;
        case Reg::LbaHigh: lba_high_ = v; break;
        case Reg::Device: device_ = v; break;
        case Reg::StatusCommand:
            if (acceptsCommand(v)) startCommand(v);
            break;
        default: break;
    }
}

CORE1_RAM bool Device::acceptsCommand(uint8_t command) const {
    // Not while BSY (the host must not write the command block then), and
    // commands for the absent device 1 are not ours to run, except EXECUTE
    // DEVICE DIAGNOSTIC, which both devices act on.
    if (status_ & ST_BSY) return false;
    return !device1Selected() || command == CMD_EXECUTE_DEVICE_DIAGNOSTIC;
}

CORE1_RAM void Device::queueCommand(uint8_t command) {
    if (!acceptsCommand(command)) return;
    queued_command_ = command;
    command_queued_ = true;
    status_ = ST_BSY | ST_DRDY | ST_DSC;
    irq_pending_ = false;  // writing Command clears a pending interrupt
}

CORE1_RAM void Device::runQueuedCommand() {
    if (!command_queued_) return;
    command_queued_ = false;
    startCommand(queued_command_);
}

CORE1_RAM void Device::hardReset(bool asserted) {
    if (asserted) {
        in_reset_ = true;
        status_ = ST_BSY;
        irq_pending_ = false;
        waking_ = false;
        command_queued_ = false;
        device_control_ = 0;
        reading_ = false;
        writing_ = false;
        receiving_ = false;
        flushing_ = false;
        fetch_pending_ = false;
        ++fetch_id_;
        // A hardware reset (not a soft one) restores the power-on CHS
        // translation and write cache setting.
        setGeometry(defaultGeometry(info_.total_sectors));
        write_cache_ = true;
        setWriteCacheEnabled(identify_, true);
        standby_ = false;
    } else if (in_reset_) {
        in_reset_ = false;
        resetToSignature();
    }
}

CORE1_RAM bool Device::intrq() const {
    // INTRQ belongs to the selected device; nIEN masks it.
    return irq_pending_ && !device1Selected() && !(device_control_ & DC_NIEN);
}

// ---- Commands ---------------------------------------------------------------

CORE1_RAM void Device::startCommand(uint8_t command) {
    last_ = CommandInfo{command, 0, 0};
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
            ++commands_completed_;
            break;
        case CMD_READ_SECTORS:
        case CMD_READ_SECTORS_NO_RETRY:
            standby_ = false;
            startRead();
            break;
        case CMD_WRITE_SECTORS:
        case CMD_WRITE_SECTORS_NO_RETRY:
            standby_ = false;
            startWrite();
            break;
        case CMD_READ_VERIFY:
        case CMD_READ_VERIFY_NO_RETRY:
            standby_ = false;
            readVerify();
            break;
        case CMD_SET_FEATURES:
            setFeatures();
            break;
        case CMD_INITIALIZE_DEVICE_PARAMETERS:
            initializeDeviceParameters();
            break;
        case CMD_FLUSH_CACHE:
            // The drive keeps nothing back itself, but its storage may (SdFat's
            // cache, the card's own buffer), so core 0 flushes it under BSY.
            flushing_ = true;
            requestFetch();
            break;
        case CMD_STANDBY_IMMEDIATE:
        case CMD_STANDBY:
        case CMD_STANDBY_IMMEDIATE_OLD:
        case CMD_STANDBY_OLD:
            // STANDBY's Sector Count sets a standby timer; there is no spindle,
            // so the timer is accepted and ignored.
            standby_ = true;
            completeNoData();
            break;
        case CMD_IDLE_IMMEDIATE:
        case CMD_IDLE:
        case CMD_IDLE_IMMEDIATE_OLD:
        case CMD_IDLE_OLD:
            if (standby_) {
                // Waking: busy for a moment, then the interrupt, as a CF card
                // does. The NS1081 reads Status straight after the command, so
                // an interrupt raised at once is cleared before it waits for it.
                standby_ = false;
                waking_ = true;
                status_ = ST_BSY | ST_DRDY | ST_DSC;
            } else {
                completeNoData();
            }
            break;
        case CMD_CHECK_POWER_MODE:
        case CMD_CHECK_POWER_MODE_OLD:
            sector_count_ = standby_ ? POWER_STANDBY : POWER_ACTIVE_OR_IDLE;
            completeNoData();
            break;
        default:
            if (command >= CMD_RECALIBRATE && command <= CMD_RECALIBRATE + 0x0F) {
                standby_ = false;
                completeNoData();  // nothing to move: back at cylinder 0 at once
            } else if (command >= CMD_SEEK && command <= CMD_SEEK + 0x0F) {
                standby_ = false;
                seek();
            } else {
                fail(ER_ABRT);  // includes SET MULTIPLE MODE: not advertised
            }
            break;
    }
}

CORE1_RAM void Device::finishWake() {
    if (!waking_) return;
    waking_ = false;
    completeNoData();
}

CORE1_RAM void Device::completeNoData() {
    status_ = ST_DRDY | ST_DSC;
    irq_pending_ = true;
    ++commands_completed_;
}

CORE1_RAM bool Device::checkRange(uint32_t count, uint32_t& lba) {
    bool valid = decodeAddress(lba);
    last_.lba = lba;
    last_.count = uint16_t(count);
    if (!valid || lba >= info_.total_sectors || count > info_.total_sectors - lba) {
        // Out of range: fail before doing anything, with the first
        // unreachable address in the task file.
        if (valid) setTaskFileLba(lba < info_.total_sectors ? info_.total_sectors : lba);
        fail(ER_IDNF);
        return false;
    }
    return true;
}

CORE1_RAM void Device::readVerify() {
    // Like READ SECTORS, but the data stays on the drive. Generated data
    // can't fail to read, so a range check is the whole job.
    uint32_t lba;
    if (checkRange(sector_count_ ? sector_count_ : 256, lba)) completeNoData();
}

CORE1_RAM void Device::seek() {
    uint32_t lba;
    if (checkRange(1, lba)) completeNoData();
}

CORE1_RAM void Device::setFeatures() {
    last_.lba = features_;  // for the trace: which subcommand, and its argument
    last_.count = sector_count_;
    if (features_ == SF_SET_TRANSFER_MODE &&
        (sector_count_ == XFER_PIO_DEFAULT || sector_count_ == XFER_PIO_FLOW_0)) {
        completeNoData();  // the only mode there is
        return;
    }
    if (features_ == SF_ENABLE_WRITE_CACHE || features_ == SF_DISABLE_WRITE_CACHE) {
        write_cache_ = features_ == SF_ENABLE_WRITE_CACHE;
        setWriteCacheEnabled(identify_, write_cache_);
        completeNoData();
        return;
    }
    // Faster PIO modes and every DMA mode are not advertised; nor is turning
    // IORDY off (01h), look-ahead or the power-on defaults.
    fail(ER_ABRT);
}

CORE1_RAM void Device::initializeDeviceParameters() {
    // Sector Count = sectors per track, Device bits 3-0 = heads - 1.
    uint16_t spt = sector_count_;
    uint16_t heads = uint16_t((device_ & 0x0F) + 1);
    if (spt == 0) {
        fail(ER_ABRT);
        return;
    }
    uint32_t cylinders = info_.total_sectors / (uint32_t(heads) * spt);
    if (cylinders > 65535) cylinders = 65535;
    if (cylinders == 0) {
        fail(ER_ABRT);
        return;
    }
    setGeometry(Geometry{uint16_t(cylinders), heads, spt});
    completeNoData();
}

CORE1_RAM void Device::setGeometry(const Geometry& g) {
    geometry_ = g;
    setCurrentGeometry(identify_, g);
}

CORE1_RAM bool Device::decodeAddress(uint32_t& lba) const {
    if (device_ & DEV_LBA) {
        lba = (uint32_t(device_ & 0x0F) << 24) | (uint32_t(lba_high_) << 16) |
              (uint32_t(lba_mid_) << 8) | lba_low_;
        return true;
    }
    // CHS, in the current translation (default, or INITIALIZE DEVICE PARAMETERS).
    uint32_t cylinder = (uint32_t(lba_high_) << 8) | lba_mid_;
    uint32_t head = device_ & 0x0F;
    uint32_t sector = lba_low_;  // 1-based
    if (sector == 0 || sector > geometry_.sectors_per_track || head >= geometry_.heads ||
        cylinder >= geometry_.cylinders)
        return false;
    lba = (cylinder * geometry_.heads + head) * geometry_.sectors_per_track + (sector - 1);
    return true;
}

CORE1_RAM void Device::startRead() {
    uint32_t count = sector_count_ ? sector_count_ : 256;
    uint32_t lba = 0;
    if (!checkRange(count, lba)) return;
    reading_ = true;
    read_lba_ = lba;
    read_left_ = count;
    ahead_ok_ = ahead_failed_ = false;
    requestFetch();
}

CORE1_RAM void Device::startWrite() {
    uint32_t count = sector_count_ ? sector_count_ : 256;
    uint32_t lba = 0;
    if (!checkRange(count, lba)) return;
    writing_ = true;
    write_lba_ = lba;
    write_left_ = count;
    startDataOut();  // no interrupt before the first block
}

CORE1_RAM void Device::startDataOut() {
    receiving_ = true;
    received_ = 0;
    status_ = ST_DRDY | ST_DSC | ST_DRQ;
}

CORE1_RAM void Device::finishDataOut() {
    receiving_ = false;
    requestFetch();  // BSY while core 0 stores the sector
}

CORE1_RAM void Device::requestFetch(bool busy) {
    if (busy) status_ = ST_BSY | ST_DRDY | ST_DSC;
    fetch_pending_ = true;
    if (++fetch_id_ == 0) ++fetch_id_;  // never 0: pendingFetchId() uses it for "none"
    fetch_lba_ = writing_ ? write_lba_ : read_lba_;
    // A read fills the buffer the host isn't reading; a write stores the one
    // it was written into.
    fetch_buf_ = writing_ ? 0 : (data_ == sectors_[0] ? 1 : 0);
}

CORE1_RAM void Device::readAhead() {
    if (read_ahead_ && reading_ && read_left_ > 0) requestFetch(false);  // no BSY: the host carries on reading
}

CORE1_RAM void Device::deliverSector(bool ok) {
    if (!ok) {
        setTaskFileLba(fetch_lba_);
        reading_ = false;
        fail(ER_UNC);
        return;
    }
    startDataIn(sectors_[fetch_buf_], SECTOR_WORDS);
    readAhead();
}

CORE1_RAM bool Device::pendingFetch(Fetch& fetch) {
    if (!fetch_pending_) return false;
    fetch = Fetch{fetch_id_, fetch_lba_, sectors_[fetch_buf_], writing_, flushing_ || (writing_ && !write_cache_)};
    return true;
}

CORE1_RAM void Device::fetchDone(uint32_t id, bool ok) {
    if (!fetch_pending_ || id != fetch_id_) return;  // stale: a reset came first
    fetch_pending_ = false;
    if (flushing_) {
        flushing_ = false;
        if (ok)
            completeNoData();
        else
            fail(ER_ABRT);
        return;
    }
    if (writing_) {
        if (!ok) {
            setTaskFileLba(write_lba_);
            fail(ER_ABRT);
            return;
        }
        ++write_lba_;
        if (--write_left_ > 0) {
            startDataOut();
            irq_pending_ = true;  // PIO data-out: interrupt when each later block is wanted
        } else {
            writing_ = false;
            completeNoData();  // written: the command ends with an interrupt
        }
        return;
    }
    ++read_lba_;
    --read_left_;
    if (data_) {  // a read-ahead, done while the host reads the previous sector
        ahead_ok_ = ok;
        ahead_failed_ = !ok;
        return;
    }
    deliverSector(ok);  // the host is waiting for it (BSY)
}

CORE1_RAM void Device::setTaskFileLba(uint32_t lba) {
    if (device_ & DEV_LBA) {
        lba_low_ = uint8_t(lba);
        lba_mid_ = uint8_t(lba >> 8);
        lba_high_ = uint8_t(lba >> 16);
        device_ = uint8_t((device_ & 0xF0) | ((lba >> 24) & 0x0F));
    } else {
        uint32_t per_cyl = uint32_t(geometry_.heads) * geometry_.sectors_per_track;
        uint32_t cylinder = lba / per_cyl;
        uint32_t rest = lba % per_cyl;
        lba_low_ = uint8_t(rest % geometry_.sectors_per_track + 1);
        lba_mid_ = uint8_t(cylinder);
        lba_high_ = uint8_t(cylinder >> 8);
        device_ = uint8_t((device_ & 0xF0) | (rest / geometry_.sectors_per_track));
    }
}

CORE1_RAM void Device::fail(uint8_t error) {
    error_ = error;
    status_ = ST_DRDY | ST_DSC | ST_ERR;
    irq_pending_ = true;
    reading_ = false;
    writing_ = false;
    receiving_ = false;
    ++commands_completed_;
}

CORE1_RAM void Device::complete() {
    status_ = ST_DRDY | ST_DSC;
    ++commands_completed_;
}

CORE1_RAM void Device::startDataIn(const uint16_t* words, uint16_t count) {
    data_ = words;
    data_count_ = count;
    data_index_ = 0;
    status_ = ST_DRDY | ST_DSC | ST_DRQ;
    irq_pending_ = true;  // PIO data-in: interrupt when each block is ready
}

CORE1_RAM void Device::finishDataIn() {
    data_ = nullptr;
    data_count_ = data_index_ = 0;
    if (reading_ && (ahead_ok_ || ahead_failed_)) {  // the next sector is in: on at once
        bool ok = ahead_ok_;
        ahead_ok_ = ahead_failed_ = false;
        deliverSector(ok);  // or its error, at the sector it belongs to
        return;
    }
    if (reading_ && fetch_pending_) {
        status_ = ST_BSY | ST_DRDY | ST_DSC;  // the read-ahead isn't in yet: wait for it
        return;
    }
    if (reading_ && read_left_ > 0) {
        requestFetch();  // read-ahead off: the next sector, under BSY
        return;
    }
    reading_ = false;
    complete();  // no interrupt at the end of a data-in transfer
}

}  // namespace ata
