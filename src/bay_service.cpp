// Core 0: SynthMinder's requests (synthminder.h, docs/synthminder-protocol.md).
// Core 1 hands each 8Dh's sector over as a Fetch with `message` set:
// serveFetches() (main.cpp) passes them here. A SEND's request is answered at
// once ($SB...) or relayed to the synth ($HD..., link.cpp); a RECEIVE is
// answered at once, or held, BSY, until the synth's answer is in or 500 ms
// have passed. Nothing here runs on core 1.
#include <Arduino.h>

#include <hardware/gpio.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "bus_sample.h"
#include "config.h"
#include "core1_ram.h"
#include "drive_device.h"
#include "short_test.h"
#include "stack_watch.h"
#include "spsc_ring.h"
#include "trace_format.h"
#include "version.h"

#if FIRMWARE_SLICE == 0
#include "ide_listen.h"
#else
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/structs/busctrl.h>
#include <hardware/uart.h>

#include "access_log.h"
#include "activity.h"
#include "auto_bus.h"
#include "activity_link.h"
#include "drive_config.h"
#include "drive_params.h"
#include "drive_timing.h"
#include "identify.h"
#include "seek_timing.h"
#include "sentence.h"
#include "ata_device.h"
#include "hdcd_device.h"
#include <new>
#include <type_traits>
#include "block_source.h"
#include "cue_sheet.h"
#include "bus_observer.h"
#include <hardware/watchdog.h>
#include "fetch_mailbox.h"
#include "ide_bus.h"
#include "ide_table.h"
#include "ide_dma.h"
#include "iocs16.h"
#include "bus_timing.h"
#include "command_history.h"
#include "pio_plan.h"
#include "mmc.h"
#include <hardware/structs/m33.h>
#include "iordy_timer.h"
#include "sd_card.h"
#include "scratch.h"
#include "synthminder.h"
#endif

#include "main_state.h"

#if FIRMWARE_SLICE != 0

namespace {

// The card's root folder, as $SBF lines: the images SynthMinder can offer
// (8.3 names: .IMG a hard disk, .ISO or .CUE a CD-ROM's disc), in
// alphabetical order. Listed afresh for each FILES (a few ms), so it is never
// out of date.
struct FileEntry {
    char name[13];
    uint32_t size_kb;
    char kind;
};
constexpr size_t FILES_MAX = 32;
FileEntry g_files[FILES_MAX];
size_t g_file_count = 0;

bool is83(const char* name) {
    size_t n = strlen(name);
    const char* dot = strchr(name, '.');
    if (n == 0 || n > 12) return false;
    if (!dot) return n <= 8;
    size_t base = size_t(dot - name), ext = n - base - 1;
    return base >= 1 && base <= 8 && ext >= 1 && ext <= 3 && !strchr(dot + 1, '.') && !strchr(name, ' ');
}

char kindOf(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    if (strcasecmp(dot, ".IMG") == 0) return 'H';
    if (strcasecmp(dot, ".ISO") == 0 || strcasecmp(dot, ".CUE") == 0) return 'C';
    return 0;
}

void addFileEntry(const char* name, uint64_t size, void*) {
    char kind = kindOf(name);
    if (!kind || !is83(name) || g_file_count == FILES_MAX) return;
    // Insertion in alphabetical order (a few dozen files at most).
    size_t at = g_file_count;
    while (at > 0 && strcasecmp(g_files[at - 1].name, name) > 0) {
        g_files[at] = g_files[at - 1];
        --at;
    }
    FileEntry& e = g_files[at];
    std::snprintf(e.name, sizeof e.name, "%s", name);
    e.size_kb = uint32_t((size + 1023) / 1024);
    e.kind = kind;
    ++g_file_count;
}

void listFiles(synthminder::Reply& reply) {
    g_file_count = 0;
    if (!sd_card::listRoot(addFileEntry, nullptr)) return;  // no card: no files
    for (size_t i = 0; i < g_file_count; ++i)
        synthminder::addFile(reply, unsigned(i), unsigned(g_file_count), g_files[i].name, g_files[i].size_kb,
                             g_files[i].kind);
}

class BayPlatform : public synthminder::Platform {
public:
    void answer(const synthminder::BayRequest& r, synthminder::Reply& reply) override {
        switch (r.query) {
            case synthminder::BayQuery::Status: bayStatus(reply); break;
            case synthminder::BayQuery::Files: listFiles(reply); break;
            case synthminder::BayQuery::Disc: synthminder::addAck(reply, "DISC", bayChangeDisc(r.key)); break;
            case synthminder::BayQuery::Set: synthminder::addAck(reply, "SET", baySet(r.key, r.value)); break;
            case synthminder::BayQuery::Settings: baySettings(reply); break;
        }
    }
    bool relay(const char* line, size_t len) override { return relayToSynth(line, len); }
};

BayPlatform g_platform;
synthminder::Service g_service(g_platform);

// A RECEIVE waiting for the synth's answer: its fetch, and the command count
// when it arrived. If another command has started since (the PC reset the
// drive and moved on), its buffer may hold someone else's data by now, so the
// late answer is dropped without touching it.
ata::Fetch g_waiting;
uint32_t g_waiting_starts = 0;

uint8_t* bytesOf(const ata::Fetch& f) {
    // The device keeps a sector's words in bus order, low byte first, as
    // SynthMinder's IDE.PAS moves them with PortW: on this little-endian
    // core the buffer reads as the sector's bytes.
    return reinterpret_cast<uint8_t*>(f.buffer);
}

}  // namespace

// Read by serveFetches() (in RAM) on every pass, so the code here (in flash)
// runs only while a RECEIVE is held.
bool g_synthminder_waiting = false;

bool synthMinderFetch(const ata::Fetch& fetch, bool& ok) {
    ok = true;
    uint32_t now = millis();
    g_synthminder_waiting = false;  // a new fetch: core 1 has given up on any earlier one
    if (fetch.store) {
        g_service.onSend(bytesOf(fetch), now);
        return true;
    }
    if (g_service.onReceive(bytesOf(fetch), now)) return true;
    g_waiting = fetch;
    g_synthminder_waiting = true;
    g_waiting_starts = g_command_starts.load(std::memory_order_relaxed);
    return false;
}

bool synthMinderReady(uint32_t& id) {
    if (!g_synthminder_waiting) return false;
    if (g_command_starts.load(std::memory_order_relaxed) != g_waiting_starts) {  // the PC moved on
        g_synthminder_waiting = false;
        return false;
    }
    if (!g_service.onReceive(bytesOf(g_waiting), millis())) return false;
    g_synthminder_waiting = false;
    id = g_waiting.id;
    return true;
}

void synthMinderLinkLine(const char* line, size_t len) { g_service.onLinkLine(line, len); }

void printSynthMinderStats(Print& out) {
    out.printf(" sm_requests=%lu sm_relays=%lu sm_timeouts=%lu link_rx_lost=%lu", (unsigned long)g_service.requests(),
               (unsigned long)g_service.relays(), (unsigned long)g_service.timeouts(), (unsigned long)linkRxLost());
}

#endif  // FIRMWARE_SLICE != 0
