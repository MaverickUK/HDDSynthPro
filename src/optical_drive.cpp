#include "optical_drive.h"

namespace optical {

bool readsDisc(uint8_t packet) {
    switch (packet) {
        case 0x28:  // READ(10)
        case 0xA8:  // READ(12)
        case 0xBE:  // READ CD
        case 0xB9:  // READ CD MSF
        case 0x2B:  // SEEK(10)
        case 0x43:  // READ TOC/PMA/ATIP
        case 0x44:  // READ HEADER
        case 0x45:  // PLAY AUDIO(10)
        case 0x47:  // PLAY AUDIO MSF
        case 0xA5:  // PLAY AUDIO(12)
            return true;
        default:
            return false;
    }
}

void Drive::begin(uint32_t now_ms, bool disc) {
    disc_ = disc;
    if (disc) spin(now_ms);
}

void Drive::spin(uint32_t now_ms) {
    if (!motor_) ++spin_ups_;
    motor_ = true;
    last_read_ms_ = now_ms;
}

void Drive::onCommand(uint32_t now_ms, uint32_t t_us, uint8_t packet, uint32_t lba, uint32_t frames) {
    last_was_read_ = readsDisc(packet);
    if (!last_was_read_ || tray_open_ || !disc_) return;  // with no disc a read fails at once
    spin(now_ms);
    ++reads_count_;
    active_ = true;
    busy_until_ms_ = now_ms + cfg_.busy_min_ms;
    reads_.onCommand(t_us, lba, packet == 0x2B ? 0 : frames);
}

void Drive::onCommandEnd(uint32_t now_ms, uint32_t t_us) {
    if (!last_was_read_ || !active_) return;
    last_was_read_ = active_ = false;
    last_read_ms_ = now_ms;
    reads_.onCommandEnd(t_us);
}

void Drive::eject(uint32_t now_ms) {
    if (tray_open_ && !closing_) return;
    motor_ = false;  // it brakes, then the tray opens: the synth plays them in turn
    active_ = false;
    tray_open_ = true;
    closing_ = false;
    disc_ = false;
    opened_ms_ = now_ms;
}

void Drive::load(uint32_t now_ms, bool disc) {
    if (!tray_open_) eject(now_ms);
    disc_ = disc;
    closing_ = true;
}

void Drive::update(uint32_t now_ms, uint32_t now_us) {
    reads_.update(now_us);
    if (closing_ && now_ms - opened_ms_ >= cfg_.tray_min_open_ms) {
        tray_open_ = closing_ = false;
        if (disc_) spin(now_ms);  // it reads the new disc's table of contents
    }
    if (motor_ && !active_ && now_ms - last_read_ms_ >= cfg_.spin_down_ms) motor_ = false;
}

State Drive::state(uint32_t now_ms) const {
    State s;
    s.busy = active_ || int32_t(busy_until_ms_ - now_ms) > 0;
    s.reads = reads_.state();
    s.motor = motor_;
    s.tray_open = tray_open_;
    return s;
}

}  // namespace optical
