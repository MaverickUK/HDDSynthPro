#include "bus_observer.h"

#include <atomic>

#include <hardware/dma.h>
#include <hardware/pio.h>
#include <pico/time.h>

#include "bus_observer.pio.h"
#include "config.h"
#include "sector_check.h"

namespace bus_observer {

static PIO s_pio;
static uint s_sm;
static int s_dma = -1;

// 1024 words. The ring wraps at its size, so it must be aligned to it.
static constexpr unsigned RING_BITS = 12;
static constexpr uint32_t RING_WORDS = (1u << RING_BITS) / 4;
alignas(1u << RING_BITS) static uint32_t s_ring[RING_WORDS];
static uint32_t s_read = 0;  // next word for core 0

static sector_check::Checker* s_checker = nullptr;
static std::atomic<bool> s_started{false};

void begin(uint32_t disk_sectors) {
    static sector_check::Checker checker(disk_sectors);
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

void poll(Print& out, bool check) {
    if (!s_started.load(std::memory_order_acquire)) return;
    uint32_t written = (dma_hw->ch[s_dma].write_addr - uint32_t(uintptr_t(s_ring))) / 4;
    // A bounded batch, so fetches are never kept waiting for long.
    for (unsigned n = 0; s_read != written && n < 512; ++n) {
        uint32_t word = s_ring[s_read];
        s_read = (s_read + 1) % RING_WORDS;
        if (!check || !s_checker->observe(word)) continue;
        const sector_check::Mismatch& m = s_checker->mismatch();
        out.printf("BUSERR %lu lba=%lu word=%u seen=%04x want=%04x bad_words=%u bits_lost=%u bits_gained=%u\n",
                   (unsigned long)time_us_32(), (unsigned long)m.lba, unsigned(m.first_word),
                   unsigned(m.seen), unsigned(m.want), unsigned(m.bad_words), unsigned(m.bits_lost),
                   unsigned(m.bits_gained));
    }
}

uint32_t sectorsChecked() { return s_checker ? s_checker->sectorsChecked() : 0; }
uint32_t sectorsBad() { return s_checker ? s_checker->sectorsBad() : 0; }
uint32_t sectorsSkipped() { return s_checker ? s_checker->sectorsSkipped() : 0; }
uint32_t gaps() { return s_checker ? s_checker->gaps() : 0; }

void resetCounts() {
    if (s_checker) s_checker->resetCounts();
}

}  // namespace bus_observer
