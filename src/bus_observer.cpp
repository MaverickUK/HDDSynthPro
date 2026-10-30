#include "bus_observer.h"

#include <atomic>

#include <hardware/dma.h>
#include <hardware/pio.h>
#include <pico/time.h>

#include "bus_observer.pio.h"
#include "bus_sample.h"
#include "config.h"
#include "identify.h"
#include "sector_check.h"

namespace bus_observer {

static PIO s_pio;
static uint s_sm;
static int s_dma = -1;

// 4,096 samples (16 KB): at multiword DMA's rate 1,024 overflowed while core
// 0 was busy (0.17.0's soak missed 94,071 words). The ring wraps at its size,
// so it must be aligned to it.
static constexpr unsigned RING_BITS = 14;
static constexpr uint32_t RING_WORDS = (1u << RING_BITS) / 4;
alignas(1u << RING_BITS) static uint32_t s_ring[RING_WORDS];
static uint32_t s_read = 0;  // next word for core 0
static sector_check::Checker* s_checker = nullptr;
static uint32_t s_ring_max = 0;  // the most samples ever waiting (ringMax)
static uint64_t s_dropped = 0;   // samples skipped on purpose (dropped)

static uint32_t s_lost = 0;      // possible overruns (lost)
static bool s_full = false;      // full checks rather than spot checks (setFull)
static uint32_t s_looked_us = 0;  // when core 0 last read the ring's write position

// Fall this far behind and the waiting samples are dropped, all but the
// newest KEEP_WORDS: half the ring leaves 2,048 samples of margin for core
// 0's other work.
static constexpr uint32_t BEHIND_WORDS = RING_WORDS / 2;
static constexpr uint32_t KEEP_WORDS = 1024;
// Away longer than this and the ring may have been overrun: multiword DMA
// mode 2 strobes at most every 120 ns, so 4,096 samples take 490 us.
static constexpr uint32_t OVERRUN_US = 400;

// Whether an observer word is a read that ends a block of data: anything but
// the data register or a DMA strobe (no chip select), e.g. a status poll.
static bool endsData(uint32_t word) {
    uint32_t sample = word & ((1u << sector_check::SEQ_SHIFT) - 1);
    bus::Block block = bus::block(sample);
    return block != bus::Block::None && !(block == bus::Block::Command && bus::address(sample) == 0);
}

// Where DMA has written up to. Too far behind, or away long enough for an
// overrun, and all but the newest samples are dropped: poll() then finds the
// next sector start in those, and checks from there.
static void look(uint32_t& written) {
    uint32_t now = time_us_32();
    bool overrun = now - s_looked_us > OVERRUN_US;
    s_looked_us = now;
    written = (dma_hw->ch[s_dma].write_addr - uint32_t(uintptr_t(s_ring))) / 4;
    uint32_t waiting = (written - s_read) % RING_WORDS;
    if (waiting > s_ring_max) s_ring_max = waiting;
    // Full checks slow the drive instead, so the ring isn't overrun (the
    // 10-hour soak's high-water mark: 2,570), and nothing is dropped.
    if (s_full || (!overrun && waiting <= BEHIND_WORDS)) return;
    if (overrun && waiting > 0) ++s_lost;  // idle, nothing new: nothing lost
    uint32_t keep = waiting < KEEP_WORDS ? waiting : KEEP_WORDS;  // never what's been read
    s_dropped += waiting - keep;
    s_read = (written - keep) % RING_WORDS;
    s_checker->resync();  // dropped on purpose: not a gap
}
static constexpr unsigned SERVE_EVERY = 32;  // words checked between calls to `serve`

static std::atomic<bool> s_started{false};

void begin(uint32_t disk_sectors, const IdentifyInfo& identity) {
    static sector_check::Checker checker(disk_sectors);
    uint16_t identify[256];
    buildIdentify(identity, identify);
    checker.setIdentify(identify);
    s_checker = &checker;

    uint offset;
    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(
        &bus_observer_program, &s_pio, &s_sm, &offset, cfg::PIN_DD0,
        cfg::PIN_IORDY - cfg::PIN_DD0 + 1, true);
    hard_assert(ok);
    pio_sm_config c = bus_observer_program_get_default_config(offset);
    sm_config_set_in_pins(&c, cfg::PIN_DD0);
    sm_config_set_in_shift(&c, true, false, 32);  // `in` shifts right: new bits on top
    sm_config_set_jmp_pin(&c, cfg::PIN_DIOR_N);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(s_pio, s_sm, offset, &c);

    s_dma = dma_claim_unused_channel(true);
    dma_channel_config d = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, RING_BITS);
    channel_config_set_dreq(&d, pio_get_dreq(s_pio, s_sm, false));
    dma_channel_configure(s_dma, &d, s_ring, &s_pio->rxf[s_sm], dma_encode_endless_transfer_count(), true);

    pio_sm_set_enabled(s_pio, s_sm, true);
    s_started.store(true, std::memory_order_release);
}

void poll(Print& out, bool check, bool check_identify, void (*serve)()) {
    if (!s_started.load(std::memory_order_acquire)) return;
    static bool checking = false;
    if (!check && !check_identify) {
        // Nothing to check: discard, which costs core 0 next to nothing.
        s_read = (dma_hw->ch[s_dma].write_addr - uint32_t(uintptr_t(s_ring))) / 4;
        s_looked_us = time_us_32();
        checking = false;
        return;
    }
    if (!checking) s_checker->resync();  // words were discarded: not a gap
    checking = true;
    uint32_t written;
    look(written);
    // A bounded batch, so fetches are never kept waiting for long.
    for (unsigned n = 0; s_read != written && n < 512; ++n) {
        if (serve && !s_full && n % SERVE_EVERY == SERVE_EVERY - 1) {
            serve();
            look(written);
        }
        // Out of step: pass over data reads, at a few cycles a word, to the
        // next sector start or the next read that ends the data.
        while (!s_checker->aligned()) {
            if ((written - s_read) % RING_WORDS < 4) break;  // wait for more
            uint32_t w[4];
            for (unsigned k = 0; k < 4; ++k) w[k] = s_ring[(s_read + k) % RING_WORDS];
            if (endsData(w[0])) break;  // observe() aligns on it
            if (!endsData(w[1]) && !endsData(w[2]) && !endsData(w[3]) &&
                s_checker->isSectorStart(uint16_t(w[0]), uint16_t(w[1]), uint16_t(w[2]), uint16_t(w[3]))) {
                s_checker->alignAt();
                break;
            }
            s_read = (s_read + 1) % RING_WORDS;
            ++s_dropped;
        }
        if (!s_checker->aligned() && !endsData(s_ring[s_read])) break;
        if (s_read == written) break;
        uint32_t word = s_ring[s_read];
        s_read = (s_read + 1) % RING_WORDS;
        if (!s_checker->observe(word, check)) continue;
        const sector_check::Mismatch& m = s_checker->mismatch();
        if (m.lba == sector_check::IDENTIFY_LBA) {
            out.printf("BUSERR %lu IDENTIFY word=%u seen=%04x want=%04x bad_words=%u bits_lost=%u bits_gained=%u\n",
                       (unsigned long)time_us_32(), unsigned(m.first_word), unsigned(m.seen), unsigned(m.want),
                       unsigned(m.bad_words), unsigned(m.bits_lost), unsigned(m.bits_gained));
            continue;
        }
        out.printf("BUSERR %lu lba=%lu word=%u seen=%04x want=%04x bad_words=%u bits_lost=%u bits_gained=%u\n",
                   (unsigned long)time_us_32(), (unsigned long)m.lba, unsigned(m.first_word),
                   unsigned(m.seen), unsigned(m.want), unsigned(m.bad_words), unsigned(m.bits_lost),
                   unsigned(m.bits_gained));
    }
}

uint32_t sectorsChecked() { return s_checker ? s_checker->sectorsChecked() : 0; }
uint32_t sectorsBad() { return s_checker ? s_checker->sectorsBad() : 0; }
uint32_t sectorsSkipped() { return s_checker ? s_checker->sectorsSkipped() : 0; }
uint32_t identifyChecked() { return s_checker ? s_checker->identifyChecked() : 0; }
uint32_t identifyBad() { return s_checker ? s_checker->identifyBad() : 0; }
uint32_t gaps() { return s_checker ? s_checker->gaps() : 0; }
uint64_t dmaWords() { return s_checker ? s_checker->dmaWords() : 0; }
uint32_t ringMax() { return s_ring_max; }
uint32_t ringSize() { return RING_WORDS; }
uint64_t dropped() { return s_dropped; }
uint32_t lost() { return s_lost; }
void setFull(bool full) { s_full = full; }
bool full() { return s_full; }

void resetCounts() {
    if (s_checker) s_checker->resetCounts();
    s_ring_max = 0;
    s_dropped = 0;
    s_lost = 0;
}

}  // namespace bus_observer
