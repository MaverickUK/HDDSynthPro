// What to do when a read or write on the SD card fails outside device 0's
// image path: restart the card, reopen device 1's image, and retry once.
//
// sd_card.cpp talks to the card (SdFat, SPI); these decide what to try and in
// which order, so they can be host-tested with a fake card. Templates over the
// card's operations, header only, so the firmware calls them directly.
//
// Pure C++, host-tested (test_card_retry.cpp).
#pragma once

#include <cstdint>

#include "media_watch.h"

namespace card_retry {

// The card's operations, as sd_card.cpp provides them:
//   uint32_t now();           // millis()
//   bool answers();           // a CMD0: is a card in, answering in SPI mode?
//   bool start();             // mount it (SdFat's begin)
//   bool reattach(uint32_t);  // restart it for device 0's image: true only
//                             // for the same card with the same image
//   void closeDevice1();      // device 1's file (after CMD0: no writes reach the card)
//   bool reopenDevice1();     // device 1's file again, if it was closed (reopen() below)
//   void countRestart();      // `s`'s card_restarts

// Restarts a card that stopped answering on a path other than device 0's
// image reads (device 1's disk, `S`, `M`). With device 0's image on the card
// it's restarted as an image read would (the same card and image only); with
// none there's nothing to identify the card by, so any card that answers is
// started. Lost or changed: no, serviceMedia() looks for the right card. True
// if the card is usable again.
template <typename Card>
bool restart(media_watch::Watch& watch, Card& card) {
    uint32_t now = card.now();
    switch (watch.state()) {
        case media_watch::State::Present:
            watch.failed(now);
            return card.reattach(now);  // counts the restart itself
        case media_watch::State::None:
            if (!card.answers()) return false;  // no card: under a millisecond, not SdFat's 2 s
            card.closeDevice1();
            if (!card.start()) return false;
            card.countRestart();
            return true;
        default:
            return false;
    }
}

// A device 1 read, write or sync. One that fails is retried once after
// restarting the card, so a card that stopped answering for a moment costs
// one slow read rather than every read until the drive restarts (seen on the
// Evesham, 4 Nov 2026). While device 0's card is lost or changed they fail
// at once, as device 0's do.
template <typename Card, typename Op>
bool device1Attempt(media_watch::Watch& watch, Card& card, Op&& op) {
    if (!watch.usable() || !card.reopenDevice1()) return false;
    if (op()) return true;
    if (!restart(watch, card) || !card.reopenDevice1()) return false;
    return op();
}

// Reopens device 1's image after a restart closed it: only the same file at
// the same size, so a different card's file of that name isn't served as it.
// `File` is SdFat's FsFile (isOpen, open, fileSize, close); an empty name
// means device 1 has no image on the card.
template <typename File, typename Flags>
bool reopen(File& file, const char* name, Flags flags, uint64_t size) {
    if (file.isOpen()) return true;
    if (!name[0] || !file.open(name, flags)) return false;
    if (uint64_t(file.fileSize()) == size) return true;
    file.close();
    return false;
}

}  // namespace card_retry
