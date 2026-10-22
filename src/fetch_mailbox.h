// Hands sector fetches from core 1 (the bus) to core 0 (the storage) and
// the results back, through two SPSC rings. Pure C++, host-tested.
//
// Core 1 posts each ata::Fetch once (by id) and passes results to
// Device::fetchDone(), which ignores stale ids. Core 0 takes requests in
// order, fills the buffer, and reports. The buffer belongs to the device,
// which leaves it alone (BSY) until the result arrives; the ring's
// release/acquire ordering makes the filled buffer visible to core 1.
#pragma once

#include <cstdint>

#include "ata_device.h"
#include "spsc_ring.h"

struct FetchResult {
    uint32_t id;
    bool ok;
};

class FetchMailbox {
public:
    // Core 1
    bool post(const ata::Fetch& fetch) { return requests_.push(fetch); }
    bool takeResult(FetchResult& result) { return results_.pop(result); }

    // Core 0
    bool takeRequest(ata::Fetch& fetch) { return requests_.pop(fetch); }
    bool postResult(const FetchResult& result) { return results_.push(result); }

private:
    SpscRing<ata::Fetch, 4> requests_;
    SpscRing<FetchResult, 4> results_;
};
