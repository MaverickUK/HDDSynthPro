#include "auto_bus.h"

namespace auto_bus {

const char* whyName(Why why) {
    switch (why) {
        case Why::Cdrom: return "cdrom";          // the lane doesn't serve a CD-ROM
        case Why::Dma: return "dma";              // DMA offered: the program doesn't matter for data
        case Why::Unknown: return "no-evidence";  // the observer or IORDY check isn't running
        case Why::Isa: return "isa";              // DIOR# with no chip select
        case Why::NotWaited: return "not-waited"; // a read the host didn't wait for
        case Why::NoProof: return "no-proof";     // too few long holds to show it waits
        case Why::Detecting: return "detecting";  // too soon after the reset
        case Why::Ok: return "ok";
    }
    return "?";
}

void Advisor::reset(const Counters& now, uint32_t now_ms) {
    base_ = now;
    reset_ms_ = changed_ms_ = now_ms;
    caught_ = false;
    sectors_ = long_reads_ = 0;
    choice_ = Choice::Early;
    why_ = Why::Detecting;
}

bool Advisor::update(const Counters& now, uint32_t now_ms) {
    sectors_ = now.sectors - base_.sectors;
    long_reads_ = now.long_reads - base_.long_reads;
    if (!caught_) {
        if (now.no_cs_known && now.dior_no_cs != base_.dior_no_cs) {
            caught_ = true;
            caught_why_ = Why::Isa;
        } else if (now.iordy_known && now.iordy_ignored != base_.iordy_ignored) {
            caught_ = true;
            caught_why_ = Why::NotWaited;
        }
    }
    Why why;
    if (now.cdrom)
        why = Why::Cdrom;
    else if (now.dma_offered)
        why = Why::Dma;
    else if (caught_)
        why = caught_why_;
    else if (!now.no_cs_known || !now.iordy_known)
        why = Why::Unknown;
    else if (long_reads_ < config_.min_long_reads)
        why = Why::NoProof;
    else if (sectors_ < config_.min_sectors)
        why = Why::Detecting;
    else
        why = Why::Ok;
    Choice choice = why == Why::Ok ? Choice::Lane : Choice::Early;
    bool changed = choice != choice_ || (choice == Choice::Early && why != why_ &&
                                         (why == Why::Isa || why == Why::NotWaited));
    if (choice != choice_) changed_ms_ = now_ms;
    choice_ = choice;
    why_ = why;
    return changed;
}

}  // namespace auto_bus
