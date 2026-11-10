#include "atapi_device.h"

#include "core1_ram.h"

namespace atapi {

using namespace ata;  // the registers' bits and the commands both kinds of device share

Device::Device(const IdentifyInfo& info, uint8_t position)
    : info_(info),
      position_(position),
      dev_bit_(position ? DEV_DEV : 0),
      id_tag_(position ? 0x80000000u : 0),
      disc_{info.total_sectors / SECTORS_PER_FRAME} {
    buildIdentifyPacket(info_, identify_);
    resetToSignature();
    mmc::powerOn(mmc_, disc_);
}

CORE1_RAM_CD void Device::resetToSignature(uint8_t device) {
    // The packet device signature, which tells a host this is ATAPI. Status
    // is 00h: DRDY stays clear until a command has run (ATA/ATAPI-5).
    sector_count_ = 1;
    lba_low_ = 1;
    lba_mid_ = 0x14;
    lba_high_ = 0xEB;
    device_ = device;
    error_ = 0x01;  // diagnostic code: passed (and, as device 0, device 1 absent)
    status_ = 0;
    data_ = nullptr;
    data_count_ = data_index_ = 0;
    irq_pending_ = false;
    command_queued_ = false;
    receiving_packet_ = false;
    packet_queued_ = false;
    packet_command_ = false;
    dma_ = dma_burst_running_ = false;
    reading_ = false;
    ahead_ok_ = ahead_failed_ = false;
    fetch_pending_ = false;  // a fetch still on core 0 becomes stale
    ++fetch_id_;
}

CORE1_RAM_CD void Device::consumeRead(Reg reg) {
    if (notSelected()) return;
    if (reg == Reg::Data) {
        if (!(status_ & ST_DRQ) || !data_) return;
        if (++data_index_ >= data_count_) finishDataIn();
    } else if (reg == Reg::StatusCommand) {
        irq_pending_ = false;
    }
}

CORE1_RAM_CD void Device::consumeDataWords(uint16_t n) {
    if (n == 0 || notSelected() || !(status_ & ST_DRQ) || !data_) return;
    data_index_ = uint16_t(data_index_ + n);
    if (data_index_ >= data_count_) finishDataIn();
}

CORE1_RAM_CD void Device::write(Reg reg, uint16_t value) {
    uint8_t v = uint8_t(value);
    if (reg == Reg::AltStatusDevCtrl) {
        bool was_srst = (device_control_ & DC_SRST) != 0;
        device_control_ = v;
        if (v & DC_SRST) {
            abandonCommand();
            in_reset_ = true;
            status_ = ST_BSY;
            irq_pending_ = false;
            command_queued_ = packet_queued_ = false;
            dma_ = dma_burst_running_ = false;  // core 1 then ends any burst
        } else if (was_srst) {
            in_reset_ = false;
            resetToSignature();
        }
        return;
    }
    if (reg == Reg::StatusCommand) {  // DEVICE RESET is taken even while BSY
        queueCommand(v);
        runQueuedCommand();
        return;
    }
    if (status_ & ST_BSY) return;

    switch (reg) {
        case Reg::Data:
            if (!receiving_packet_ || notSelected()) break;
            packet_[2 * packet_words_] = uint8_t(value);  // low byte first
            packet_[2 * packet_words_ + 1] = uint8_t(value >> 8);
            if (++packet_words_ == mmc::PACKET_BYTES / 2) {
                receiving_packet_ = false;
                packet_queued_ = true;
                status_ = ST_BSY | (status_ & ST_DRDY);
            }
            break;
        case Reg::ErrorFeatures: features_ = v; break;
        case Reg::SectorCount: sector_count_ = v; break;
        case Reg::LbaLow: lba_low_ = v; break;
        case Reg::LbaMid: lba_mid_ = v; break;
        case Reg::LbaHigh: lba_high_ = v; break;
        case Reg::Device: device_ = v; break;
        default: break;
    }
}

CORE1_RAM_CD bool Device::acceptsCommand(uint8_t command) const {
    if (in_reset_) return false;
    if (notSelected()) return command == CMD_EXECUTE_DEVICE_DIAGNOSTIC;
    // DEVICE RESET is how a host recovers a packet device that's stuck busy.
    return !(status_ & ST_BSY) || command == CMD_DEVICE_RESET;
}

CORE1_RAM_CD void Device::queueCommand(uint8_t command) {
    if (!acceptsCommand(command)) return;
    queued_command_ = command;
    command_queued_ = true;
    status_ = ST_BSY | (status_ & ST_DRDY);
    irq_pending_ = false;  // writing Command clears a pending interrupt
}

CORE1_RAM_CD void Device::runQueuedCommand() {
    if (command_queued_) {
        command_queued_ = false;
        startCommand(queued_command_);
    } else if (packet_queued_) {
        packet_queued_ = false;
        runPacket();
    }
}

CORE1_RAM_CD void Device::hardReset(bool asserted) {
    if (asserted) {
        abandonCommand();
        in_reset_ = true;
        status_ = ST_BSY;
        irq_pending_ = false;
        command_queued_ = packet_queued_ = false;
        device_control_ = 0;
        receiving_packet_ = false;
        dma_ = dma_burst_running_ = false;
        reading_ = false;
        ahead_ok_ = ahead_failed_ = false;
        fetch_pending_ = false;
        ++fetch_id_;
    } else if (in_reset_) {
        in_reset_ = false;
        resetToSignature();
        mmc::powerOn(mmc_, disc_);  // as a real drive does: a unit attention, a new-media event
    }
}

CORE1_RAM_CD bool Device::intrq() const {
    return irq_pending_ && !notSelected() && !(device_control_ & DC_NIEN);
}

// ---- ATA commands -------------------------------------------------------------

CORE1_RAM_CD void Device::beginCommand(uint8_t command) {
    last_ = CommandInfo{command, 0, 0, 0};
    ++commands_started_;
}

CORE1_RAM_CD void Device::startCommand(uint8_t command) {
    // Whatever was running ends here (a host that gave up on a block, as
    // ata::Device describes, or DEVICE RESET on a stuck one).
    abandonCommand();
    data_ = nullptr;
    data_count_ = data_index_ = 0;
    receiving_packet_ = false;
    packet_command_ = false;
    dma_ = dma_burst_running_ = false;
    reading_ = false;
    ahead_ok_ = ahead_failed_ = false;
    if (fetch_pending_) {
        fetch_pending_ = false;
        if (++fetch_id_ == 0) ++fetch_id_;
    }
    irq_pending_ = false;
    error_ = 0;
    if (command == CMD_PACKET) {
        startPacket();  // counted as started once the packet is in
        return;
    }
    beginCommand(command);
    switch (command) {
        case CMD_IDENTIFY_PACKET_DEVICE:
            startDataIn(identify_, 256);
            break;
        case CMD_IDENTIFY_DEVICE:
        case CMD_READ_SECTORS:
        case CMD_READ_SECTORS_NO_RETRY:
            abortWithSignature();
            break;
        case CMD_EXECUTE_DEVICE_DIAGNOSTIC:
            resetToSignature();
            irq_pending_ = true;
            ++commands_completed_;
            break;
        case CMD_DEVICE_RESET:
            resetToSignature(dev_bit_);  // no interrupt; still the device selected, as device 1 too
            ++commands_completed_;
            break;
        case CMD_SET_FEATURES:
            setFeatures();
            break;
        case CMD_CHECK_POWER_MODE:
        case CMD_CHECK_POWER_MODE_OLD:
            sector_count_ = POWER_ACTIVE_OR_IDLE;
            completeNoData();
            break;
        case CMD_STANDBY_IMMEDIATE:
        case CMD_IDLE_IMMEDIATE:
        case CMD_STANDBY:
        case CMD_IDLE:
        case CMD_STANDBY_IMMEDIATE_OLD:
        case CMD_IDLE_IMMEDIATE_OLD:
        case CMD_STANDBY_OLD:
        case CMD_IDLE_OLD:
            completeNoData();  // no spindle to stop: accepted
            break;
        default:
            fail(ER_ABRT);
            break;
    }
}

CORE1_RAM_CD void Device::setFeatures() {
    last_.lba = features_;
    last_.count = sector_count_;
    uint8_t pio_mode = info_.pio_mode > MAX_PIO_MODE ? MAX_PIO_MODE : info_.pio_mode;
    uint8_t mdma = info_.mdma_mode > MAX_MDMA_MODE ? MAX_MDMA_MODE : info_.mdma_mode;
    bool ok = false;
    switch (features_) {
        case SF_SET_TRANSFER_MODE:  // PIO default, flow-control modes, or multiword DMA, up to those offered
            ok = sector_count_ == XFER_PIO_DEFAULT ||
                 (sector_count_ >= XFER_PIO_FLOW_0 && sector_count_ <= XFER_PIO_FLOW_0 + pio_mode);
            if (info_.mdma_mode != NO_DMA && sector_count_ >= XFER_MDMA_0 && sector_count_ <= XFER_MDMA_0 + mdma) {
                setMdmaModeSelected(identify_, uint8_t(sector_count_ - XFER_MDMA_0));
                ok = true;
            }
            break;
        case 0x55: case 0xAA:  // read look-ahead off, on
        case 0x66: case 0xCC:  // power-on defaults: keep, revert
            ok = true;  // nothing to change
            break;
        default:
            break;
    }
    if (ok)
        completeNoData();
    else
        fail(ER_ABRT);
}

CORE1_RAM_CD void Device::abortWithSignature() {
    // ATA: a packet device aborts these with its signature in the registers,
    // so a host that probes for a hard disk learns what it found.
    sector_count_ = 1;
    lba_low_ = 1;
    lba_mid_ = 0x14;
    lba_high_ = 0xEB;
    fail(ER_ABRT);
}

CORE1_RAM_CD void Device::completeNoData() {
    status_ = ST_DRDY;
    irq_pending_ = true;
    ++commands_completed_;
}

CORE1_RAM_CD void Device::fail(uint8_t error) {
    data_ = nullptr;
    dma_ = dma_burst_running_ = false;
    error_ = error;
    status_ = ST_DRDY | ST_ERR;
    irq_pending_ = true;
    ++commands_completed_;
}

// ---- PACKET -------------------------------------------------------------------

CORE1_RAM_CD void Device::startPacket() {
    if (features_ & FEATURES_DMA) {
        if (info_.mdma_mode == NO_DMA) {  // not offered
            beginCommand(CMD_PACKET);
            fail(ER_ABRT);
            return;
        }
        dma_ = true;  // the packet by PIO, the data by DMA; the byte count isn't used
    }
    limit_ = uint16_t(lba_high_ << 8 | lba_mid_);
    if (limit_ == 0 || limit_ == 0xFFFF) limit_ = 0xFFFE;  // 0 isn't allowed; FFFFh means "any"
    if (limit_ < 2) limit_ = 2;
    receiving_packet_ = true;
    packet_words_ = 0;
    sector_count_ = IR_COD;  // a command, to the device
    status_ = uint8_t((status_ & ST_DRDY) | ST_DRQ);  // no interrupt: DRQ within 50 us (IDENTIFY PACKET word 0)
}

CORE1_RAM_CD void Device::runPacket() {
    last_ = CommandInfo{CMD_PACKET, 0, 0, packet_[0]};
    ++commands_started_;
    packet_command_ = true;
    mmc::Reply r = mmc::execute(packet_, disc_, mmc_, reinterpret_cast<uint8_t*>(reply_), sizeof reply_);
    switch (r.kind) {
        case mmc::Reply::Kind::Good:
            packetDone();
            break;
        case mmc::Reply::Kind::Data:
            if (r.bytes == 0) {
                packetDone();
                break;
            }
            buf_ = reply_;
            buf_bytes_ = r.bytes;
            buf_pos_ = 0;
            sendBlock();
            break;
        case mmc::Reply::Kind::Read:
            last_.lba = r.lba;
            last_.count = uint16_t(r.frames > 0xFFFF ? 0xFFFF : r.frames);
            reading_ = true;
            ahead_ok_ = ahead_failed_ = false;
            read_lba_ = r.lba * SECTORS_PER_FRAME;
            read_left_ = r.frames * SECTORS_PER_FRAME;
            command_lba_ = read_lba_;
            command_sectors_ = uint16_t(read_left_ > 0xFFFF ? 0xFFFF : read_left_);
            requestFetch();
            break;
        case mmc::Reply::Kind::Error:
            packetFail();
            break;
    }
}

CORE1_RAM_CD void Device::packetDone() {
    packet_command_ = false;
    dma_ = dma_burst_running_ = false;  // the status phase: core 1 ends the DMA command
    sector_count_ = IR_COD | IR_IO;  // status
    status_ = ST_DRDY;
    irq_pending_ = true;
    ++commands_completed_;
}

CORE1_RAM_CD void Device::packetFail() {
    // CHECK CONDITION: the sense key in the Error register's top four bits;
    // ABRT too for a packet the drive refused.
    packet_command_ = false;
    dma_ = dma_burst_running_ = false;
    reading_ = false;
    ahead_ok_ = ahead_failed_ = false;
    data_ = nullptr;
    uint8_t key = mmc_.sense.key;
    error_ = uint8_t(key << 4 | (key == mmc::KEY_ILLEGAL_REQUEST ? ER_ABRT : 0));
    sector_count_ = IR_COD | IR_IO;
    status_ = ST_DRDY | ST_ERR;
    irq_pending_ = true;
    ++commands_completed_;
}

CORE1_RAM_CD void Device::startDataIn(const uint16_t* words, uint16_t count) {
    data_ = words;
    data_count_ = count;
    data_index_ = 0;
    status_ = ST_DRDY | ST_DRQ;
    irq_pending_ = !dma_;  // in DMA, DMARQ asks for each block; the interrupt is the status phase's
}

CORE1_RAM_CD void Device::sendBlock() {
    uint16_t left = uint16_t(buf_bytes_ - buf_pos_);
    uint16_t block = left;
    if (block > limit_ && !dma_) block = uint16_t(limit_ & ~1u);  // a block that isn't the last stays whole words
    block_bytes_ = block;
    lba_mid_ = uint8_t(block);
    lba_high_ = uint8_t(block >> 8);
    sector_count_ = IR_IO;  // data, to the host
    startDataIn(buf_ + buf_pos_ / 2, uint16_t((block + 1) / 2));
}

CORE1_RAM_CD void Device::finishDataIn() {
    data_ = nullptr;
    data_count_ = data_index_ = 0;
    if (!packet_command_) {  // IDENTIFY PACKET DEVICE: no interrupt at the end
        status_ = ST_DRDY;
        ++commands_completed_;
        return;
    }
    buf_pos_ = uint16_t(buf_pos_ + block_bytes_);
    if (buf_pos_ < buf_bytes_) {
        sendBlock();  // the host's byte count was smaller than the reply
    } else if (reading_ && (ahead_ok_ || ahead_failed_)) {
        bool ok = ahead_ok_;  // the next sector is in: on at once
        ahead_ok_ = ahead_failed_ = false;
        deliverSector(ok);
    } else if (reading_ && fetch_pending_) {
        status_ = ST_BSY | ST_DRDY;  // the read-ahead isn't in yet: wait for it
    } else if (reading_ && read_left_ > 0) {
        requestFetch();  // read-ahead off: the next sector, under BSY
    } else {
        reading_ = false;
        packetDone();
    }
}

CORE1_RAM_CD void Device::requestFetch(bool busy) {
    if (busy) status_ = ST_BSY | ST_DRDY;
    fetch_pending_ = true;
    if (++fetch_id_ == 0) ++fetch_id_;
    fetch_lba_ = read_lba_++;
    --read_left_;
    fetch_buf_ = buf_ == sectors_[0] ? 1 : 0;  // never the sector being sent
}

CORE1_RAM_CD bool Device::pendingFetch(Fetch& fetch) {
    if (!fetch_pending_) return false;
    fetch = Fetch{fetch_id_ | id_tag_, fetch_lba_, sectors_[fetch_buf_], false, false, command_sectors_, command_lba_,
                  position_};
    return true;
}

CORE1_RAM_CD void Device::fetchDone(uint32_t id, bool ok) {
    if (!fetch_pending_ || id != (fetch_id_ | id_tag_)) return;  // stale: a reset or a newer command
    fetch_pending_ = false;
    if (data_) {  // a read-ahead, done while the host takes the sector before
        ahead_ok_ = ok;
        ahead_failed_ = !ok;
        return;
    }
    deliverSector(ok);  // the host is waiting for it (BSY)
}

CORE1_RAM_CD void Device::deliverSector(bool ok) {
    if (!ok) {
        mmc_.sense = mmc::SENSE_READ_ERROR;
        packetFail();
        return;
    }
    buf_ = sectors_[fetch_buf_];
    buf_bytes_ = uint16_t(ata::SECTOR_WORDS * 2);
    buf_pos_ = 0;
    sendBlock();
    if (read_ahead_ && read_left_ > 0) requestFetch(false);  // no BSY: the host carries on reading
}

// ---- Changing the disc ------------------------------------------------------------

CORE1_RAM_CD void Device::changeDisc(uint32_t total_sectors) {
    info_.total_sectors = total_sectors;
    disc_.frames = total_sectors / SECTORS_PER_FRAME;
    mmc::discChanged(mmc_, disc_);
}

// ---- DMA -----------------------------------------------------------------------

CORE1_RAM_CD bool Device::dmaBurstReady(DmaBurst& burst) {
    if (!dma_ || dma_burst_running_ || !data_ || !(status_ & ST_DRQ) || notSelected()) return false;
    burst = DmaBurst{data_ + data_index_, nullptr, uint16_t(data_count_ - data_index_)};
    dma_burst_running_ = true;
    return true;
}

CORE1_RAM_CD void Device::dmaBurstDone() {
    if (!dma_burst_running_) return;
    dma_burst_running_ = false;
    if (data_) {
        data_index_ = data_count_;
        finishDataIn();  // the next block, a fetch under BSY, or the status phase
    }
}

}  // namespace atapi
