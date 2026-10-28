// Whether the SD card the disk image is on is still the card being served:
// it notices a card pulled out (an image read or write failed) and decides
// whether a card put back may be served again.
//
// A host caches what it knows of a disk's file system. If a different card
// came back, or the same image after a computer edited it, and the drive went
// on serving it, the host's next writes would corrupt it. So only the same
// card (its serial number) with the same image, unchanged (size, place on the
// card, modify time), is served again; anything else waits for a restart.
//
// Pure C++, host-tested. sd_card.cpp talks to the card and drives this.
#pragma once

#include <cstdint>

namespace media_watch {

// The card and image being served: compared when a card comes back.
struct Identity {
    uint8_t maker = 0;        // the card's CID: manufacturer ID
    uint32_t serial = 0;      // and product serial number
    uint64_t size = 0;        // the image's size in bytes
    uint32_t first_sector = 0;  // where the image starts on the card
    uint16_t date = 0, time = 0;  // its FAT modify date and time (the drive never sets them)
};

enum class State : uint8_t {
    None,     // not serving an image: nothing to watch
    Present,  // the card being served is in
    Lost,     // an image read or write failed: looking for the card coming back
    Changed,  // a card came back, but not the one served (or its image changed)
};

const char* stateName(State s);  // "none", "present", "lost", "changed"

// Why a card that came back can't be served, or nullptr if it can.
const char* difference(const Identity& served, const Identity& now);

// How often to look for a lost card (a CMD0, under a millisecond), and how
// often to check that a changed card is still in (a CMD13).
constexpr uint32_t PROBE_MS = 250;
constexpr uint32_t CHANGED_CHECK_MS = 1000;

class Watch {
public:
    // Serving an image on the card that `id` describes.
    void serve(const Identity& id);

    State state() const { return state_; }
    // Image reads and writes are tried only while the card is present; while
    // it's lost or changed they fail at once.
    bool usable() const { return state_ == State::Present || state_ == State::None; }

    // An image read, write or flush failed (the card was pulled, or it
    // stopped answering). False if nothing changed (already lost, or none).
    bool failed(uint32_t now_ms);

    // Time to try the card: a probe while lost, a status check while changed.
    bool due(uint32_t now_ms);

    // A card answered and was mounted; `id` is it and its image. Present
    // again if it's the one served, else Changed (difference() says why).
    State cameBack(const Identity& id, uint32_t now_ms);
    // A card answered but its image couldn't be opened ("not found", ...).
    void cameBackWithout(const char* why, uint32_t now_ms);
    // While changed: that card was taken out. Lost again, so the right one
    // can come back.
    void gone(uint32_t now_ms);

    const char* why() const { return why_; }  // while changed
    uint32_t losses() const { return losses_; }
    uint32_t returns() const { return returns_; }

private:
    State state_ = State::None;
    Identity served_;
    uint32_t last_try_ms_ = 0;
    const char* why_ = "";
    uint32_t losses_ = 0, returns_ = 0;
};

}  // namespace media_watch
