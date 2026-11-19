#include "ata_device.h"

#include "core1_ram.h"

namespace ata {

CORE1_RAM Reg regFor(bool command_block, bool control_block, unsigned address) {
    if (command_block && !control_block) return static_cast<Reg>(address & 7);
    if (control_block && !command_block && address == 6) return Reg::AltStatusDevCtrl;
    return Reg::None;
}

Device::Device(const IdentifyInfo& info, const IdentifyInfo* device1) : has_device1_(device1 != nullptr) {
    for (unsigned n = 0; n < (has_device1_ ? 2u : 1u); ++n) {
        Unit& unit = units_[n];
        unit.info = n == 0 ? info : *device1;
        unit.geometry = startGeometry(unit.info);
        buildIdentify(unit.info, unit.identify);
        if (s_no_multiple) offerNoMultiple(unit.identify);
    }
    resetToSignature();
}

// READ/WRITE MULTIPLE offered or not, for devices made from now on: not with
// the data lane, whose reads blocks of 8 made 15% slower on the Evesham
// (docs/performance.md, item 1). Only IDENTIFY changes, from flash, so core
// 1's code stays as measured; a host that asks for blocks anyway still gets
// them, slower on the lane but right.
bool Device::s_no_multiple = false;

void Device::offerMultiple(bool on) { s_no_multiple = !on; }

void Device::offerDma(uint8_t mdma_mode, const IdentifyPatch& unit0, const IdentifyPatch& unit1) {
    for (unsigned n = 0; n < (has_device1_ ? 2u : 1u); ++n) {
        Unit& unit = units_[n];
        unit.info.mdma_mode = mdma_mode;
        applyPatch(unit.identify, n == 0 ? unit0 : unit1);
    }
}

CORE1_RAM_RARE void Device::setDisk(const IdentifyInfo& info, const uint16_t identify[256]) {
    Unit& unit = units_[0];
    unit.info = info;
    unit.geometry = startGeometry(unit.info);
    for (unsigned i = 0; i < 256; ++i) {
        unit.identify[i] = identify[i];
        __asm__ volatile("" ::: "memory");  // a plain copy, not a call to memcpy (in flash)
    }
}

void Device::identifyFor(const IdentifyInfo& info, uint16_t out[256]) {
    buildIdentify(info, out);
    if (s_no_multiple) offerNoMultiple(out);
}

// The host changed the Device register: which unit answers from now on.
CORE1_RAM void Device::deviceWritten() {
    uint8_t unit = (device_ & DEV_DEV) ? 1 : 0;
    if (!has_device1_) {
        absent_selected_ = unit == 1;
        return;
    }
    // Not mid-transfer: ATA forbids changing DEV then, and the command's
    // state belongs to the unit running it. (Writes while BSY never get here.)
    if (unit != unit_ && !(status_ & ST_DRQ)) selectUnit(unit);
}

CORE1_RAM void Device::selectUnit(uint8_t unit) {
    Unit& from = units_[unit_];
    from.status = status_;
    from.error = error_;
    from.irq = irq_pending_;
    unit_ = unit;
    status_ = units_[unit].status;
    error_ = units_[unit].error;
    irq_pending_ = units_[unit].irq;
}

CORE1_RAM void Device::resetToSignature() {
    // The ATA device signature: what a host checks after a reset to tell an
    // ATA drive (0000h in the LBA mid/high pair) from ATAPI (EB14h).
    sector_count_ = 1;
    lba_low_ = 1;
    lba_mid_ = 0;
    lba_high_ = 0;
    device_ = 0;
    error_ = 0x01;  // diagnostic code: device 0 passed, and device 1 passed or absent
    status_ = ST_DRDY | ST_DSC;
    // Both units set the signature, and device 0 is selected.
    unit_ = 0;
    absent_selected_ = false;
    units_[1].status = ST_DRDY | ST_DSC;
    units_[1].error = 0x01;
    units_[1].irq = false;
    data_ = nullptr;
    data_count_ = data_index_ = 0;
    irq_pending_ = false;
    waking_ = false;
    command_queued_ = false;
    // Abandon any read or write; a fetch still running on core 0 becomes stale.
    reading_ = false;
    writing_ = false;
    receiving_ = false;
    out_hold_ = false;
    flushing_ = false;
    fetch_pending_ = false;
    if (++fetch_id_ == 0) ++fetch_id_;  // never 0: pendingFetchId() uses it for "none"
}

CORE1_RAM void Device::consumeRead(Reg reg) {
    if (absent_selected_) return;  // device 0 answers 00h and keeps its state
    if (reg == Reg::Data) {
        if (!(status_ & ST_DRQ) || !data_) return;
        if (++data_index_ >= data_count_) finishDataIn();
    } else if (reg == Reg::StatusCommand) {
        irq_pending_ = false;  // reading Status acknowledges the interrupt
    }
}

CORE1_RAM void Device::consumeDataWords(uint16_t n) {
    if (n == 0 || absent_selected_ || !(status_ & ST_DRQ) || !data_) return;
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
            abandonCommand();
            dma_ = dma_burst_running_ = false;
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
            if (!receiving_ || absent_selected_) break;  // no data-out phase: ignored
            sectors_[recv_buf_][received_++] = value;
            if (received_ >= recv_words_) finishDataOut();
            break;
        case Reg::ErrorFeatures: features_ = v; break;
        case Reg::SectorCount: sector_count_ = v; break;
        case Reg::LbaLow: lba_low_ = v; break;
        case Reg::LbaMid: lba_mid_ = v; break;
        case Reg::LbaHigh: lba_high_ = v; break;
        case Reg::Device:
            device_ = v;
            deviceWritten();
            break;
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
    return !absent_selected_ || command == CMD_EXECUTE_DEVICE_DIAGNOSTIC;
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
        abandonCommand();
        in_reset_ = true;
        status_ = ST_BSY;
        irq_pending_ = false;
        waking_ = false;
        dma_ = dma_burst_running_ = false;
        command_queued_ = false;
        device_control_ = 0;
        reading_ = false;
        writing_ = false;
        receiving_ = false;
        out_hold_ = false;
        flushing_ = false;
        fetch_pending_ = false;
        if (++fetch_id_ == 0) ++fetch_id_;
        // A hardware reset (not a soft one) restores both units' power-on
        // CHS translation and write cache setting.
        for (unsigned n = 0; n < (has_device1_ ? 2u : 1u); ++n) {
            Unit& unit = units_[n];
            unit.geometry = startGeometry(unit.info);
            setCurrentGeometry(unit.identify, unit.geometry);
            unit.write_cache = true;
            setWriteCacheEnabled(unit.identify, true);
            unit.multiple = 0;  // multiple mode off, as at power-on
            setMultipleSectors(unit.identify, 0);
            unit.standby = false;
        }
    } else if (in_reset_) {
        in_reset_ = false;
        resetToSignature();
    }
}

CORE1_RAM bool Device::intrq() const {
    // INTRQ belongs to the selected device; nIEN masks it.
    return irq_pending_ && !absent_selected_ && !(device_control_ & DC_NIEN);
}

// ---- Commands ---------------------------------------------------------------

CORE1_RAM void Device::startCommand(uint8_t command) {
    // A command written while a data-in block is unread (DRQ): the host gave
    // up on it. ATA says it mustn't, but the JMicron does. Abandon it, or the
    // stale block looks like a transfer under way (loop1 then never takes a
    // fetch result) and the new command's first sector like a read-ahead,
    // and the drive stays BSY for good. A write can't be left like this: it
    // ends only when every sector is stored, and holds BSY until then.
    abandonCommand();  // whatever was running ends here
    dma_ = dma_burst_running_ = false;
    if (data_ || reading_) {
        data_ = nullptr;
        data_count_ = data_index_ = 0;
        reading_ = false;
        ahead_ok_ = ahead_failed_ = false;
        if (fetch_pending_) {  // a read-ahead under way: its result is stale now
            fetch_pending_ = false;
            if (++fetch_id_ == 0) ++fetch_id_;
        }
    }
    last_ = CommandInfo{command, 0, 0, unit_};
    ++commands_started_;
    irq_pending_ = false;  // writing Command clears a pending interrupt
    error_ = 0;
    switch (command) {
        case CMD_IDENTIFY_DEVICE:
            startDataIn(u().identify, 256);
            break;
        case CMD_EXECUTE_DEVICE_DIAGNOSTIC:
            resetToSignature();  // signature, error 01h, and an interrupt
            irq_pending_ = true;
            ++commands_completed_;
            break;
        case CMD_READ_SECTORS:
        case CMD_READ_SECTORS_NO_RETRY:
            u().standby = false;
            startRead();
            break;
        case CMD_READ_DMA:
        case CMD_READ_DMA_NO_RETRY:
            if (u().info.mdma_mode == NO_DMA) {
                fail(ER_ABRT);  // not offered
                break;
            }
            u().standby = false;
            dma_ = true;
            startRead();  // the same fetches and read-ahead; the words go by DMA
            break;
        case CMD_WRITE_DMA:
        case CMD_WRITE_DMA_NO_RETRY:
            if (u().info.mdma_mode == NO_DMA) {
                fail(ER_ABRT);
                break;
            }
            u().standby = false;
            dma_ = true;
            startWrite();
            break;
        case CMD_WRITE_SECTORS:
        case CMD_WRITE_SECTORS_NO_RETRY:
            u().standby = false;
            startWrite();
            break;
        case CMD_READ_MULTIPLE:
        case CMD_WRITE_MULTIPLE:
            if (!u().multiple) {
                fail(ER_ABRT);  // SET MULTIPLE MODE first
                break;
            }
            u().standby = false;
            if (command == CMD_READ_MULTIPLE)
                startRead(u().multiple);
            else
                startWrite(u().multiple);
            break;
        case CMD_SET_MULTIPLE_MODE:
            setMultipleMode();
            break;
        case CMD_READ_VERIFY:
        case CMD_READ_VERIFY_NO_RETRY:
            u().standby = false;
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
            u().standby = true;
            completeNoData();
            break;
        case CMD_IDLE_IMMEDIATE:
        case CMD_IDLE:
        case CMD_IDLE_IMMEDIATE_OLD:
        case CMD_IDLE_OLD:
            if (u().standby) {
                // Waking: busy for a moment, then the interrupt, as a CF card
                // does. The NS1081 reads Status straight after the command, so
                // an interrupt raised at once is cleared before it waits for it.
                u().standby = false;
                waking_ = true;
                status_ = ST_BSY | ST_DRDY | ST_DSC;
            } else {
                completeNoData();
            }
            break;
        case CMD_CHECK_POWER_MODE:
        case CMD_CHECK_POWER_MODE_OLD:
            sector_count_ = u().standby ? POWER_STANDBY : POWER_ACTIVE_OR_IDLE;
            completeNoData();
            break;
        default:
            if (command >= CMD_RECALIBRATE && command <= CMD_RECALIBRATE + 0x0F) {
                u().standby = false;
                completeNoData();  // nothing to move: back at cylinder 0 at once
            } else if (command >= CMD_SEEK && command <= CMD_SEEK + 0x0F) {
                u().standby = false;
                seek();
            } else {
                otherCommand();  // SynthMinder's, or ABRT
                // 0.35.0's size plus 32 bytes, a whole RAM bank period, so the
                // per-sector code after this function keeps its banks (the
                // compiler leaves it 10 bytes over without these 12 no-ops, on
                // a path almost never taken). Check with check_core1_layout.py.
                __asm__ volatile("nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop");
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
    if (!valid || lba >= u().info.total_sectors || count > u().info.total_sectors - lba) {
        // Out of range: fail before doing anything, with the first
        // unreachable address in the task file.
        if (valid) setTaskFileLba(lba < u().info.total_sectors ? u().info.total_sectors : lba);
        fail(ER_IDNF);
        return false;
    }
    return true;
}

CORE1_RAM void Device::readVerify() {
    // Like READ SECTORS, but the data stays on the drive. Generated data
    // can't fail to read, so a range check is the whole job.
    uint32_t lba = 0;  // decodeAddress leaves it alone for a CHS address out of range
    if (checkRange(sector_count_ ? sector_count_ : 256, lba)) completeNoData();
}

CORE1_RAM void Device::seek() {
    uint32_t lba = 0;
    if (checkRange(1, lba)) completeNoData();
}

CORE1_RAM void Device::setFeatures() {
    last_.lba = features_;  // for the trace: which subcommand, and its argument
    last_.count = sector_count_;
    // PIO default, or PIO flow-control mode 0 up to the one IDENTIFY offers.
    // The bus program serves every mode it offers the same way, so there's
    // nothing to switch.
    uint8_t pio_mode = u().info.pio_mode > MAX_PIO_MODE ? MAX_PIO_MODE : u().info.pio_mode;
    if (features_ == SF_SET_TRANSFER_MODE &&
        (sector_count_ == XFER_PIO_DEFAULT ||
         (sector_count_ >= XFER_PIO_FLOW_0 && sector_count_ <= XFER_PIO_FLOW_0 + pio_mode))) {
        completeNoData();
        return;
    }
    // Multiword DMA mode 0 up to the one IDENTIFY offers.
    uint8_t mdma = u().info.mdma_mode > MAX_MDMA_MODE ? MAX_MDMA_MODE : u().info.mdma_mode;
    if (features_ == SF_SET_TRANSFER_MODE && u().info.mdma_mode != NO_DMA &&
        sector_count_ >= XFER_MDMA_0 && sector_count_ <= XFER_MDMA_0 + mdma) {
        setMdmaModeSelected(u().identify, uint8_t(sector_count_ - XFER_MDMA_0));
        completeNoData();
        return;
    }
    if (features_ == SF_ENABLE_WRITE_CACHE || features_ == SF_DISABLE_WRITE_CACHE) {
        u().write_cache = features_ == SF_ENABLE_WRITE_CACHE;
        setWriteCacheEnabled(u().identify, u().write_cache);
        completeNoData();
        return;
    }
    // Faster PIO modes and every DMA mode aren't offered; nor is turning
    // IORDY off (01h), look-ahead or the power-on defaults.
    fail(ER_ABRT);
}

// SET MULTIPLE MODE: Sector Count is the sectors per DRQ block for READ and
// WRITE MULTIPLE: a power of two up to MAX_MULTIPLE, or 0 to turn them off.
CORE1_RAM void Device::setMultipleMode() {
    uint8_t n = sector_count_;
    last_.count = n;  // for the trace: what the host asked for
    if (n > MAX_MULTIPLE || (n & (n - 1)) != 0) {
        fail(ER_ABRT);  // the setting stays as it was
        return;
    }
    u().multiple = n;
    setMultipleSectors(u().identify, n);
    completeNoData();
}

CORE1_RAM void Device::initializeDeviceParameters() {
    // Sector Count = sectors per track, Device bits 3-0 = heads - 1.
    uint16_t spt = sector_count_;
    uint16_t heads = uint16_t((device_ & 0x0F) + 1);
    if (spt == 0) {
        fail(ER_ABRT);
        return;
    }
    uint32_t cylinders = u().info.total_sectors / (uint32_t(heads) * spt);
    if (cylinders > 65535) cylinders = 65535;
    if (cylinders == 0) {
        fail(ER_ABRT);
        return;
    }
    setGeometry(Geometry{uint16_t(cylinders), heads, spt});
    completeNoData();
}

CORE1_RAM void Device::setGeometry(const Geometry& g) {
    u().geometry = g;
    setCurrentGeometry(u().identify, g);
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
    if (sector == 0 || sector > u().geometry.sectors_per_track || head >= u().geometry.heads ||
        cylinder >= u().geometry.cylinders)
        return false;
    lba = (cylinder * u().geometry.heads + head) * u().geometry.sectors_per_track + (sector - 1);
    return true;
}

// After the range check: inlined into startRead and synthMinder, so startRead
// is the code it was before SynthMinder (core 1's layout, CLAUDE.md).
__attribute__((always_inline)) inline void Device::beginRead(uint32_t lba, uint32_t count, uint8_t block) {
    block_ = block;
    reading_ = true;
    read_lba_ = lba;
    read_left_ = count;
    ahead_ok_ = ahead_failed_ = false;
    requestFetch();
}

CORE1_RAM void Device::startRead(uint8_t block) {
    uint32_t count = sector_count_ ? sector_count_ : 256;
    uint32_t lba = 0;
    if (!checkRange(count, lba)) return;
    beginRead(lba, count, block);
}

__attribute__((always_inline)) inline void Device::beginWrite(uint32_t lba, uint32_t count, uint8_t block) {
    block_ = block;
    writing_ = true;
    write_lba_ = lba;
    write_left_ = count;
    recv_left_ = count;
    filled_ = false;
    store_failed_ = false;
    out_hold_ = false;
    recv_buf_ = 0;
    startDataOut();  // no interrupt before the first block
}

CORE1_RAM void Device::startWrite(uint8_t block) {
    uint32_t count = sector_count_ ? sector_count_ : 256;
    uint32_t lba = 0;
    if (!checkRange(count, lba)) return;
    beginWrite(lba, count, block);
}

// A command startCommand's switch doesn't name: SynthMinder's 8Dh, else
// ABRT. Out here, in core 1's rare code, so startCommand is 0.35.0's size and
// the per-sector code after it in core 1's block sits where it did: a case of
// its own made startCommand 20 bytes longer, which moved them to other RAM
// banks (docs/speed-log.md, 19 Nov 2026).
CORE1_RAM_RARE void Device::otherCommand() {
    if (last_.command == CMD_SYNTHMINDER)  // the command startCommand recorded: no argument to pass
        synthMinder();
    else
        fail(ER_ABRT);
}

// SynthMinder's 8Dh: one sector each way, through the same fetches as a
// one-sector WRITE or READ SECTORS, so the host sees what it would for them
// (DRQ, then BSY while core 0 takes the request; BSY until the reply is
// ready, then DRQ). No disk is touched: the fetch's command_lba is
// SM_MESSAGE_LBA, which tells core 0 (isMessage). In core 1's rare code, out of
// the per-access block, so the bus loops sit where they did before it.
CORE1_RAM_RARE void Device::synthMinder() {
    last_.lba = features_;  // for the trace: SEND or RECEIVE
    last_.count = sector_count_;
    if ((features_ != SM_SEND && features_ != SM_RECEIVE) || sector_count_ != 1 || lba_low_ != SM_LBA_LOW ||
        lba_mid_ != SM_LBA_MID || lba_high_ != SM_VERSION) {
        fail(ER_ABRT);  // not SynthMinder's
        return;
    }
    last_.lba = SM_MESSAGE_LBA | features_;  // the fetch's command_lba: SynthMinder's, not a disk's
    if (features_ == SM_SEND)
        beginWrite(0, 1, 1);
    else
        beginRead(0, 1, 1);
}

CORE1_RAM void Device::startDataOut() {
    receiving_ = true;
    received_ = 0;
    recv_count_ = uint8_t(recv_left_ < block_ ? recv_left_ : block_);  // the last block may be short
    recv_words_ = uint16_t(recv_count_ * SECTOR_WORDS);
    status_ = ST_DRDY | ST_DSC | ST_DRQ;
}

CORE1_RAM void Device::finishDataOut() {
    receiving_ = false;
    recv_left_ -= recv_count_;
    filled_ = true;
    filled_buf_ = recv_buf_;
    filled_count_ = recv_count_;
    if (recv_left_ > 0 && !dma_) out_hold_ = true;  // PIO: BSY before the next block is wanted
    settleWrite();
}

// Only the register table's loop calls it (docs/register-table.md): in its
// section, so the hard disk's core 1 code stays where it is measured.
CORE1_RAM_TABLE void Device::consumeDataOut(uint16_t n) {
    if (!receiving_ || absent_selected_) return;
    received_ = uint16_t(received_ + n > recv_words_ ? recv_words_ : received_ + n);
    if (received_ >= recv_words_) finishDataOut();
}

CORE1_RAM void Device::releaseDataOut() {
    if (!out_hold_) return;
    out_hold_ = false;
    if (writing_) settleWrite();
}

// A write's next step, after the host finishes a sector or core 0 a store:
// start the waiting store, ask for the next sector if a buffer is free, or
// finish. One store at a time; with write-behind the host fills the other
// buffer meanwhile. The command ends only when every sector is stored.
CORE1_RAM void Device::settleWrite() {
    if (store_failed_) {
        filled_ = false;  // nothing after the failed sector is stored
        out_hold_ = false;
        if (receiving_ || fetch_pending_) return;  // the block or store under way ends first
        writing_ = false;
        setTaskFileLba(store_failed_lba_);
        fail(ER_ABRT);
        return;
    }
    if (filled_ && !fetch_pending_) {  // its store, without BSY
        filled_ = false;
        fetch_buf_ = filled_buf_;
        fetch_lba_ = write_lba_;
        fetch_count_ = filled_count_;
        fetch_pending_ = true;
        if (++fetch_id_ == 0) ++fetch_id_;
    }
    if (!receiving_ && !out_hold_ && recv_left_ > 0 && !filled_ && (write_behind_ || !fetch_pending_)) {
        recv_buf_ = fetch_pending_ ? uint8_t(fetch_buf_ ^ 1) : 0;  // the buffer not being stored
        startDataOut();
        if (!dma_) irq_pending_ = true;  // PIO data-out: interrupt when each later block is wanted
        return;
    }
    if (receiving_) return;
    if (write_left_ == 0) {
        writing_ = false;
        dma_ = false;
        completeNoData();  // all stored: the command ends with an interrupt
        return;
    }
    status_ = ST_BSY | ST_DRDY | ST_DSC;  // waiting for a store
}

CORE1_RAM void Device::requestFetch(bool busy) {
    if (busy) status_ = ST_BSY | ST_DRDY | ST_DSC;
    fetch_pending_ = true;
    if (++fetch_id_ == 0) ++fetch_id_;  // never 0: pendingFetchId() uses it for "none"
    fetch_lba_ = writing_ ? write_lba_ : read_lba_;
    // A read's next block (the last may be short); FLUSH CACHE moves nothing.
    fetch_count_ = !reading_ ? 1 : uint8_t(read_left_ < block_ ? read_left_ : block_);
    // A read fills the buffer the host isn't reading; a write stores the one
    // it was written into.
    fetch_buf_ = writing_ ? 0 : (data_ == sectors_[0] ? 1 : 0);
}

CORE1_RAM void Device::readAhead() {
    if (read_ahead_ && reading_ && read_left_ > 0) requestFetch(false);  // no BSY: the host carries on reading
}

CORE1_RAM void Device::deliverSector(bool ok) {
    if (!ok) {
        // The whole block fails, none of it transferred: the address is the
        // sector that couldn't be read.
        setTaskFileLba(fetch_lba_ + fetch_failed_);
        reading_ = false;
        fail(ER_UNC);
        return;
    }
    startDataIn(sectors_[fetch_buf_], uint16_t(fetch_count_ * SECTOR_WORDS));
    readAhead();
}

CORE1_RAM bool Device::pendingFetch(Fetch& fetch) {
    if (!fetch_pending_) return false;
    fetch = Fetch{fetch_id_, fetch_lba_, sectors_[fetch_buf_], writing_, flushing_ || (writing_ && !u().write_cache),
                  last_.count, last_.lba, unit_, fetch_count_};
    return true;
}

CORE1_RAM void Device::fetchDone(uint32_t id, bool ok, uint8_t failed) {
    if (!fetch_pending_ || id != fetch_id_) return;  // stale: a reset came first
    fetch_pending_ = false;
    fetch_failed_ = ok ? 0 : failed;
    if (flushing_) {
        flushing_ = false;
        if (ok)
            completeNoData();
        else
            fail(ER_ABRT);
        return;
    }
    if (writing_) {
        if (ok) {
            write_lba_ += fetch_count_;
            write_left_ -= fetch_count_;
        } else {
            store_failed_ = true;
            store_failed_lba_ = write_lba_ + fetch_failed_;
        }
        settleWrite();
        return;
    }
    read_lba_ += fetch_count_;
    read_left_ -= fetch_count_;
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
        uint32_t per_cyl = uint32_t(u().geometry.heads) * u().geometry.sectors_per_track;
        uint32_t cylinder = lba / per_cyl;
        uint32_t rest = lba % per_cyl;
        lba_low_ = uint8_t(rest % u().geometry.sectors_per_track + 1);
        lba_mid_ = uint8_t(cylinder);
        lba_high_ = uint8_t(cylinder >> 8);
        device_ = uint8_t((device_ & 0xF0) | (rest / u().geometry.sectors_per_track));
    }
}

CORE1_RAM void Device::fail(uint8_t error) {
    dma_ = dma_burst_running_ = false;
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
    if (!dma_) irq_pending_ = true;  // PIO data-in: interrupt when each block is ready
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
    if (dma_) {
        dma_ = false;
        completeNoData();  // DMA: the interrupt comes at the end
        return;
    }
    complete();  // no interrupt at the end of a PIO data-in transfer
}

CORE1_RAM bool Device::dmaBurstReady(DmaBurst& burst) {
    if (!dma_ || dma_burst_running_ || absent_selected_) return false;
    if (reading_ && data_ && (status_ & ST_DRQ)) {
        burst = DmaBurst{data_ + data_index_, nullptr, uint16_t(data_count_ - data_index_)};
    } else if (writing_ && receiving_) {
        burst = DmaBurst{nullptr, sectors_[recv_buf_] + received_, uint16_t(recv_words_ - received_)};
    } else {
        return false;  // BSY: a fetch, a store or a full buffer first
    }
    dma_burst_running_ = true;
    return true;
}

CORE1_RAM void Device::dmaBurstDone() {
    if (!dma_burst_running_) return;
    dma_burst_running_ = false;
    if (data_) {
        data_index_ = data_count_;
        finishDataIn();  // the next sector at once if read-ahead has it, else BSY
    } else if (receiving_) {
        received_ = recv_words_;
        finishDataOut();  // its store, and the next buffer
    }
}

}  // namespace ata
