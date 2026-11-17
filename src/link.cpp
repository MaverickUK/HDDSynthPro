// Core 0: the activity classifier, the activity link to an HDD Synth (UART0),
// seek timing's model and the link's loopback test (console `U`). Moved out
// of main.cpp (0.33.1-dev.7); main_state.h declares what the files share.
// Nothing here runs on core 1.
#include <Arduino.h>

#include <hardware/gpio.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include <cstring>

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

// Activity classification for the HDD Synth (activity.h), fed from the
// command events on their way to the trace. Prints an ACT line on each change.
activity::Classifier g_activity;
uint32_t g_activity_checked_us = 0;
bool g_activity_unprinted = false;

// The activity link to an HDD Synth (activity_link.h), on UART0. Core 0
// must never wait on it, so a sentence (at most 23 bytes) is written only
// when the 32-byte TX FIFO is empty; otherwise it stays due and goes a
// millisecond later, with whatever is current then. $HDA goes first; $HDE,
// where each media command takes the head, fills the gaps (at most one every
// cfg::LINK_HEAD_GAP_US, the newest).
//
// The other direction (GPIO 1, protocol version 2) brings the synth's $HDM:
// the modelled drive it's playing, for seek_timing = synth, and its answers to
// what SynthMinder asks it through the drive (synthminder.h). GPIO 1 is pulled
// up (a UART line idles high), so unconnected it reads idle, not noise.
activity_link::Sender g_link(cfg::LINK_HEARTBEAT_MS);
activity_link::HeadPacer g_head(cfg::LINK_HEAD_GAP_US);
activity_link::Op g_link_op = activity_link::Op::None;  // the last media command's
static LineAssembler g_link_rx;
seek_timing::SynthModel g_synth;
uint32_t g_link_rx_errors = 0;  // framing, parity, break or overrun

// What arrives is moved from the UART's 32-byte RX FIFO into this ring by an
// interrupt on core 0: a pack list relayed from the synth for SynthMinder is
// a few KB in one burst (the FIFO fills in 2.8 ms), more than a loop pass
// can be relied on to keep up with. serviceLinkRx() and the loopback test
// read the ring. A byte with an error (framing, parity, break or overrun) is
// counted in g_link_rx_errors and dropped.
static SpscRing<uint8_t, 1024> g_link_rx_ring;
uint32_t g_link_rx_lost = 0;  // bytes that found the ring full

static void linkRxInterrupt() {
    uart_hw_t* hw = uart_get_hw(uart0);
    while (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
        uint32_t dr = hw->dr;
        if (dr & (UART_UARTDR_FE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_OE_BITS))
            ++g_link_rx_errors;
        else if (!g_link_rx_ring.push(uint8_t(dr & 0xFF)))
            ++g_link_rx_lost;
    }
}

// uart_init resets the UART, interrupt enables and all: this sets them again.
static void startLinkUart() {
    uart_init(uart0, cfg::LINK_BAUD);          // 8N1
    uart_set_irq_enables(uart0, true, false);  // RX: the FIFO half full, or a pause after a byte
}

void beginLink() {
    startLinkUart();
    gpio_set_function(cfg::PIN_LINK_TX, GPIO_FUNC_UART);
    gpio_set_pulls(cfg::PIN_LINK_RX, true, false);  // a pull-up only (erratum E9)
    gpio_set_function(cfg::PIN_LINK_RX, GPIO_FUNC_UART);
    irq_set_exclusive_handler(UART0_IRQ, linkRxInterrupt);  // on core 0, which calls this: core 1 never sees it
    irq_set_enabled(UART0_IRQ, true);
}

// A sentence for the synth from SynthMinder (synthminder.h), sent by
// serviceLink() between $HDA and $HDE sentences once the TX FIFO is empty. It
// is at most synthminder::RELAY_LINE_MAX characters, so with its line ending
// it fits the FIFO and sending it never waits.
static char g_relay_line[synthminder::RELAY_LINE_MAX + 3];
static size_t g_relay_len = 0;

bool relayToSynth(const char* line, size_t len) {
    if (!g_link_on || len > synthminder::RELAY_LINE_MAX) return false;
    memcpy(g_relay_line, line, len);
    g_relay_line[len] = '\r';
    g_relay_line[len + 1] = '\n';
    g_relay_len = len + 2;
    return true;
}

// For STATUS's `link`: off, up (the synth's $HDM heard lately) or lost.
const char* linkState() {
    if (!g_link_on) return "off";
    return g_synth.heard(millis()) ? "up" : "lost";
}

// A head target for $HDE: the command's first sector, from `at_us` (when its
// seek starts).
void postHeadTarget(uint32_t lba, uint32_t sectors, activity_link::Op op, uint64_t at_us) {
    activity_link::HeadTarget t;
    t.pos = activity_link::headPosition(lba, g_disk->sectors());
    t.op = op;
    t.sectors = sectors;
    g_head.post(t, at_us);
}

static bool loopbackSending();

void serviceLink() {
    if (!g_link_on || loopbackSending()) return;
    if (g_relay_len) {  // SynthMinder's first: the PC is waiting for the answer
        if (uart_get_hw(uart0)->fr & UART_UARTFR_TXFE_BITS) {
            uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(g_relay_line), g_relay_len);  // fits: never waits
            g_relay_len = 0;
        }
        return;
    }
    activity_link::Fields f;
    f.busy = g_busy;
    f.state = g_activity.state();
    f.op = g_link_op;
    uint32_t now = millis();
    uint64_t now_us = time_us_64();
    activity_link::Next next = activity_link::nextSentence(g_link, f, now, g_head, now_us);
    if (next == activity_link::Next::Nothing) return;
    if (!(uart_get_hw(uart0)->fr & UART_UARTFR_TXFE_BITS)) return;  // not empty yet
    char line[activity_link::HEAD_SENTENCE_MAX];
    size_t n = next == activity_link::Next::Activity ? activity_link::formatSentence(f, line, sizeof line)
                                                     : activity_link::formatHeadSentence(g_head.target(), line, sizeof line);
    if (n == 0) return;
    uart_write_blocking(uart0, reinterpret_cast<const uint8_t*>(line), n);  // fits: never waits
    if (next == activity_link::Next::Activity)
        g_link.sent(f, now);
    else
        g_head.sent(now_us);
}

// Authentic seek timing (seek_timing.h): HDDSYNTH.INI's seek_timing, which
// console `H` turns off and on until a restart. The model is a preset, or
// the synth's from $HDM; serveFetches() holds each result until its time.
seek_timing::Scheduler g_seek;
static SeekModel g_seek_preset;
bool g_seek_on = true;

void beginSeekTiming() {
    uint32_t sectors = g_disk->sectors();
    g_seek.setGeometry(sectors, startGeometry(driveIdentity()).sectors_per_track);
    if (g_config.seek_timing.source == seek_timing::Source::Preset) {
        DriveParams p;
        drivePreset(g_config.seek_timing.preset, p);
        g_seek_preset = seekModelOf(p);
    }
}

// The model to follow now, or null.
const SeekModel* seekModel(uint32_t now_ms) {
    if (!g_seek_on) return nullptr;
    switch (g_config.seek_timing.source) {
        case seek_timing::Source::Synth: return g_synth.model(now_ms);
        case seek_timing::Source::Preset: return &g_seek_preset;
        default: return nullptr;
    }
}

// A model as "voice 5400rpm 6000cyl stroke=19.0ms track=2.0ms", for S and s.
static void describeModel(const SeekModel* m, char* out, size_t size) {
    if (!m) {
        std::snprintf(out, size, "none");
        return;
    }
    if (m->actuator == Actuator::Stepper)
        std::snprintf(out, size, "stepper %ldrpm %ldcyl step=%ld.%ldms zoned=%d", lroundf(m->rpm), lroundf(m->cylinders),
                      lroundf(m->stepMs * 10) / 10, lroundf(m->stepMs * 10) % 10, int(m->zoned));
    else
        std::snprintf(out, size, "voice %ldrpm %ldcyl stroke=%ld.%ldms track=%ld.%ldms zoned=%d", lroundf(m->rpm),
                      lroundf(m->cylinders), lroundf(m->fullStrokeMs * 10) / 10, lroundf(m->fullStrokeMs * 10) % 10,
                      lroundf(m->trackMs * 10) / 10, lroundf(m->trackMs * 10) % 10, int(m->zoned));
}

// For STATUS's `seek_timing`: "off", or the setting and the model's speed,
// e.g. "synth 5400 rpm", "voice97 5400 rpm", "synth (no model)".
void seekTimingSummary(char* out, size_t size) {
    if (!g_seek_on || g_config.seek_timing.source == seek_timing::Source::Off) {
        std::snprintf(out, size, "off");
        return;
    }
    const SeekModel* m = seekModel(millis());
    if (m)
        std::snprintf(out, size, "%s %ld rpm", seek_timing::settingName(g_config.seek_timing), lroundf(m->rpm));
    else
        std::snprintf(out, size, "%s (no model)", seek_timing::settingName(g_config.seek_timing));
}

// Console `S`'s line for seek timing.
void printSeekConfig() {
    uint32_t now = millis();
    char model[80];
    const SeekModel* m = g_config.seek_timing.source == seek_timing::Source::Synth ? g_synth.model(now)
                         : g_config.seek_timing.source == seek_timing::Source::Preset ? &g_seek_preset
                                                                                     : nullptr;
    describeModel(m, model, sizeof model);
    Serial.printf("CONFIG seek_timing=%s%s model=%s\n", seek_timing::settingName(g_config.seek_timing),
                  g_seek_on ? "" : " (off: console H)", model);
    if (g_config.seek_timing.source == seek_timing::Source::Synth || g_synth.valid()) {
        if (g_synth.heard(now))
            Serial.printf("CONFIG synth $HDM %lu ms ago: %s\n", (unsigned long)(now - g_synth.lastMs()),
                          g_synth.model(now) ? "a modelled drive" : "a recorded pack (no model)");
        else
            Serial.printf("CONFIG synth: no $HDM in the last %lu s (GPIO 1 wired to the synth's J6 pin 4?)\n",
                          (unsigned long)(seek_timing::MODEL_TIMEOUT_MS / 1000));
    }
}

// The loopback test (console `U`): with a jumper from GPIO 0 (TX) to GPIO 1
// (RX), read back for a second what the link sends ($HDA and $HDE), plus one
// $HDM the test sends itself, as the synth would, so both directions'
// parsers are checked. Polled on every loop pass, so core 0 never waits on
// it and the 32-byte RX FIFO (2.8 ms at 115200 baud) can't overflow.
constexpr uint32_t LOOPBACK_MS = 1000;  // five heartbeats
static struct {
    enum class Phase : uint8_t { Off, Starting, Listening, Finishing } phase = Phase::Off;
    uint32_t until_ms = 0;
    uint32_t sent_before = 0;
    uint32_t head_sent_before = 0;
    uint32_t bytes = 0;
    uint32_t errors = 0;  // framing, parity, break or overrun
    uint32_t valid = 0, head_valid = 0, rejected = 0;
    activity_link::Fields last;
    LineAssembler rx;
    uint32_t errors_before = 0;  // g_link_rx_errors at the start
    // The $HDM the test sends (longer than the TX FIFO, so a piece at a time),
    // and whether it came back.
    char model_line[SENTENCE_MAX_LINE + 4];
    size_t model_len = 0, model_sent = 0;
    bool model_back = false, model_wrong = false;
} g_loopback;

static bool loopbackSending() {
    return g_loopback.phase != decltype(g_loopback)::Phase::Off && g_loopback.model_sent < g_loopback.model_len;
}

void startLoopback() {
    if (g_loopback.phase != decltype(g_loopback)::Phase::Off) return;
    if (!g_link_on) {
        Serial.println("# loopback: the link is off (L turns it on)");
        return;
    }
    g_loopback.phase = decltype(g_loopback)::Phase::Starting;
    Serial.printf("# loopback: reading GPIO %u for %lu ms (needs a jumper from GPIO %u)\n",
                  unsigned(cfg::PIN_LINK_RX), (unsigned long)LOOPBACK_MS, unsigned(cfg::PIN_LINK_TX));
}

// The voice97 preset, as the synth would send it.
static size_t loopbackModelLine(char* out, size_t size) {
    DriveParams p;
    drivePreset("voice97", p);
    SeekModel m = seekModelOf(p);
    char body[SENTENCE_MAX_LINE];
    if (!formatModelBody(&m, body, sizeof body)) return 0;
    return formatSentence(out, size, body);
}

static void loopbackLine(const char* line, size_t len) {
    activity_link::Fields f;
    activity_link::HeadTarget t;
    SeekModel m;
    bool present = false;
    if (activity_link::parseLine(line, len, f)) {
        ++g_loopback.valid;
        g_loopback.last = f;
    } else if (activity_link::parseHeadLine(line, len, t)) {
        ++g_loopback.head_valid;
    } else if (parseModelSentence(line, len, m, present)) {
        // Back as it went: the same sentence, formatted again from what was read.
        char body[SENTENCE_MAX_LINE], again[SENTENCE_MAX_LINE + 4];
        size_t n = formatModelBody(present ? &m : nullptr, body, sizeof body);
        size_t k = n ? formatSentence(again, sizeof again, body) : 0;
        bool same = k == g_loopback.model_len && memcmp(again, g_loopback.model_line, k) == 0;
        if (same && !g_loopback.model_back) g_loopback.model_back = true;
        else g_loopback.model_wrong = true;
    } else {
        ++g_loopback.rejected;
    }
}

// Reads what has arrived (the ring the RX interrupt fills), without waiting.
// The interrupt counts errors, and drops their bytes.
static void drainLoopback() {
    g_loopback.errors = g_link_rx_errors - g_loopback.errors_before;
    uint8_t c;
    while (g_link_rx_ring.pop(c)) {
        ++g_loopback.bytes;
        switch (g_loopback.rx.feed(char(c))) {
            case LineAssembler::Result::Line: loopbackLine(g_loopback.rx.line(), g_loopback.rx.length()); break;
            case LineAssembler::Result::Dropped: ++g_loopback.rejected; break;
            default: break;
        }
    }
}

// Sends what fits of the test's $HDM, without waiting.
static void feedLoopbackModel() {
    uart_hw_t* hw = uart_get_hw(uart0);
    while (g_loopback.model_sent < g_loopback.model_len && !(hw->fr & UART_UARTFR_TXFF_BITS))
        hw->dr = uint8_t(g_loopback.model_line[g_loopback.model_sent++]);
}

void serviceLoopback() {
    using Phase = decltype(g_loopback)::Phase;
    uart_hw_t* hw = uart_get_hw(uart0);
    bool sending = hw->fr & UART_UARTFR_BUSY_BITS;  // a sentence still on the wire
    uint32_t now = millis();
    switch (g_loopback.phase) {
        case Phase::Off: return;
        case Phase::Starting:
            // Between sentences, so listening starts at the beginning of one.
            // Resetting the UART then (nothing is being sent) clears anything
            // half received and any error left from before.
            if (sending) return;
            startLinkUart();
            {
                uint8_t c;
                while (g_link_rx_ring.pop(c)) {}  // anything from before
            }
            g_loopback.rx = LineAssembler();
            g_loopback.bytes = g_loopback.errors = 0;
            g_loopback.errors_before = g_link_rx_errors;
            g_loopback.valid = g_loopback.head_valid = g_loopback.rejected = 0;
            g_loopback.model_back = g_loopback.model_wrong = false;
            g_loopback.model_len = loopbackModelLine(g_loopback.model_line, sizeof g_loopback.model_line);
            g_loopback.model_sent = 0;
            g_loopback.sent_before = g_link.count();
            g_loopback.head_sent_before = g_head.count();
            g_loopback.until_ms = now + LOOPBACK_MS;
            g_loopback.phase = Phase::Listening;
            feedLoopbackModel();  // first, before the link's next sentence
            return;
        case Phase::Listening:
            feedLoopbackModel();
            drainLoopback();
            if (int32_t(now - g_loopback.until_ms) >= 0) g_loopback.phase = Phase::Finishing;
            return;
        case Phase::Finishing:
            if (sending) return;  // let the last sentence finish arriving
            drainLoopback();
            break;
    }

    uint32_t sent = g_link.count() - g_loopback.sent_before;
    uint32_t head_sent = g_head.count() - g_loopback.head_sent_before;
    bool last_matches = g_loopback.valid && g_loopback.last == g_link.last();
    const char* model = g_loopback.model_wrong ? "bad" : g_loopback.model_back ? "ok" : "missing";
    bool pass = g_loopback.valid == sent && g_loopback.head_valid == head_sent && !g_loopback.rejected &&
                !g_loopback.errors && last_matches && g_loopback.model_back && !g_loopback.model_wrong;
    const char* result = g_loopback.bytes == 0 ? "no-data" : pass ? "pass" : "fail";
    char last[activity_link::SENTENCE_MAX] = "-";
    if (g_loopback.valid) {
        size_t n = activity_link::formatSentence(g_loopback.last, last, sizeof last);
        if (n >= 2) last[n - 2] = '\0';  // without the \r\n
    }
    Serial.printf("LOOPBACK %s sent=%lu valid=%lu rejected=%lu errors=%lu bytes=%lu last=%s head_sent=%lu "
                  "head_valid=%lu model=%s\n",
                  result, (unsigned long)sent, (unsigned long)g_loopback.valid, (unsigned long)g_loopback.rejected,
                  (unsigned long)g_loopback.errors, (unsigned long)g_loopback.bytes, last, (unsigned long)head_sent,
                  (unsigned long)g_loopback.head_valid, model);
    if (g_loopback.bytes == 0)
        Serial.printf("# loopback: nothing arrived; is GPIO %u jumpered to GPIO %u?\n", unsigned(cfg::PIN_LINK_TX),
                      unsigned(cfg::PIN_LINK_RX));
    g_loopback.phase = Phase::Off;
}

// The synth's side of the link, read on every loop pass from the ring the RX
// interrupt fills: $HDM lines, and its answers to SynthMinder's requests. The
// loopback test reads the ring itself while it runs.
void serviceLinkRx() {
    if (g_loopback.phase != decltype(g_loopback)::Phase::Off) return;
    uint32_t now = millis();
    uint8_t c;
    while (g_link_rx_ring.pop(c)) {
        switch (g_link_rx.feed(char(c))) {
            case LineAssembler::Result::Line: {
                // $HDM to the seek model (and SynthMinder, which can ask for
                // it); the rest ($HDS, $HDP, $HDK: answers to its relays) to
                // SynthMinder only, so they don't count as rejected models
                // (hdm_rejected rose with every relay)
                const char* line = g_link_rx.line();
                const size_t len = g_link_rx.length();
                if (len >= 4 && std::strncmp(line, "$HDM", 4) == 0) g_synth.onLine(line, len, now);
                synthMinderLinkLine(line, len);
                break;
            }
            case LineAssembler::Result::Dropped: g_synth.onDropped(); break;
            default: break;
        }
    }
}

#endif  // FIRMWARE_SLICE != 0
