// Host-side unit tests for the platform-independent firmware code: bus
// sample decoding, the trace format and coalescer, the SPSC ring (including a
// two-thread test that mirrors core 1 / core 0) and the IDENTIFY block.
//
// Run: test/run_native_tests.sh
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "bus_sample.h"
#include "config.h"
#include "identify.h"
#include "spsc_ring.h"
#include "trace_format.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++checks;                                                                \
        auto _a = (a);                                                           \
        auto _b = (b);                                                           \
        if (!(_a == _b)) {                                                       \
            ++failures;                                                          \
            std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                        #a, #b, (long long)_a, (long long)_b);                   \
        }                                                                        \
    } while (0)
#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        ++checks;                                                                \
        std::string _a = (a);                                                    \
        std::string _b = (b);                                                    \
        if (_a != _b) {                                                          \
            ++failures;                                                          \
            std::printf("  FAIL %s:%d:\n    got:  \"%s\"\n    want: \"%s\"\n",    \
                        __FILE__, __LINE__, _a.c_str(), _b.c_str());             \
        }                                                                        \
    } while (0)

#define TEST(name) static void name()
#define RUN(name)                                     \
    do {                                              \
        std::printf("%s\n", #name);                   \
        name();                                       \
    } while (0)

// ---- Helpers --------------------------------------------------------------

// Builds a pin sample (bit n = GPIO PIN_DD0 + n) for an access. Idle lines
// are high, as the host leaves them.
static uint32_t sample(bool read, bool write, bool cs0, bool cs1, unsigned addr, uint16_t data) {
    uint32_t s = data;
    s |= (addr & 7u) << bus::bitOf(cfg::PIN_DA0);
    if (!cs0) s |= bus::maskOf(cfg::PIN_CS0_N);
    if (!cs1) s |= bus::maskOf(cfg::PIN_CS1_N);
    if (!read) s |= bus::maskOf(cfg::PIN_DIOR_N);
    if (!write) s |= bus::maskOf(cfg::PIN_DIOW_N);
    s |= bus::maskOf(cfg::PIN_IORDY) | bus::maskOf(cfg::PIN_RESET_N);
    return s;
}

static std::string format(const TraceRun& run) {
    char buf[128];
    size_t n = formatRun(run, buf, sizeof buf);
    return std::string(buf, n);
}

// ---- Bus samples ----------------------------------------------------------

TEST(test_pin_layout) {
    // The PIO program drops 22 bits then reads DIOR#, DIOW#: they must be
    // bits 22 and 23 of a sample, and DD/DA/CS# must sit below them.
    CHECK_EQ(bus::bitOf(cfg::PIN_DIOR_N), 22u);
    CHECK_EQ(bus::bitOf(cfg::PIN_DIOW_N), 23u);
    CHECK_EQ(bus::bitOf(cfg::PIN_DA0), 16u);
    CHECK_EQ(bus::bitOf(cfg::PIN_CS0_N), 19u);
    CHECK_EQ(bus::bitOf(cfg::PIN_CS1_N), 20u);
    // Everything in one PIO window (GPIO 0-31), clear of GPIO 2/3 and the LED.
    CHECK(cfg::BUS_FIRST_PIN >= 4);
    CHECK(cfg::BUS_LAST_PIN <= 31);
    CHECK(cfg::PIN_BOARD_LED == 25);
}

TEST(test_decode_status_read) {
    uint32_t s = sample(true, false, true, false, 7, 0x0050);
    CHECK(bus::op(s) == bus::Op::Read);
    CHECK(bus::block(s) == bus::Block::Command);
    CHECK_EQ(bus::address(s), 7u);
    CHECK_EQ(bus::data(s), 0x0050);
    CHECK_STR(bus::registerName(bus::block(s), 7, false), "STATUS");
    CHECK_STR(bus::registerName(bus::block(s), 7, true), "COMMAND");
}

TEST(test_decode_control_block) {
    uint32_t s = sample(false, true, false, true, 6, 0x0004);
    CHECK(bus::op(s) == bus::Op::Write);
    CHECK(bus::block(s) == bus::Block::Control);
    CHECK_STR(bus::registerName(bus::Block::Control, 6, true), "DEVCTRL");
    CHECK_STR(bus::registerName(bus::Block::Control, 6, false), "ALTSTATUS");
    CHECK_STR(bus::registerName(bus::Block::Control, 2, false), "-");
    CHECK_STR(bus::registerName(bus::Block::None, 7, false), "-");
}

TEST(test_decode_invalid_combinations) {
    CHECK(bus::op(sample(true, true, true, false, 0, 0)) == bus::Op::Both);
    CHECK(bus::block(sample(true, false, true, true, 0, 0)) == bus::Block::Both);
    CHECK(bus::block(sample(true, false, false, false, 0, 0)) == bus::Block::None);
}

// ---- Trace format and coalescer -------------------------------------------

TEST(test_format_bus_run) {
    uint32_t s = sample(true, false, true, false, 7, 0x0050);
    TraceRun run{TraceEvent{1000, s, TraceKind::Bus}, 3, 1012};
    char want[96];
    std::snprintf(want, sizeof want, "EV 1000 RD CMD 7 STATUS 0050 x3 12 %08lx\n", (unsigned long)s);
    CHECK_STR(format(run), want);
}

TEST(test_format_other_kinds) {
    CHECK_STR(format(TraceRun{TraceEvent{5, 0, TraceKind::ResetAssert}, 1, 5}), "RESET 5 ASSERT\n");
    CHECK_STR(format(TraceRun{TraceEvent{9, 0, TraceKind::ResetRelease}, 1, 9}), "RESET 9 RELEASE\n");
    CHECK_STR(format(TraceRun{TraceEvent{7, 0, TraceKind::Stall}, 1, 7}), "STALL 7\n");
}

TEST(test_format_truncates) {
    uint32_t s = sample(true, false, true, false, 7, 0x0050);
    char buf[10];
    size_t n = formatRun(TraceRun{TraceEvent{1, s, TraceKind::Bus}, 1, 1}, buf, sizeof buf);
    CHECK_EQ(n, sizeof buf - 1);
    CHECK_EQ(buf[n], '\0');
}

TEST(test_coalescer_merges_identical_accesses) {
    TraceCoalescer c;
    TraceRun done;
    uint32_t status = sample(true, false, true, false, 7, 0x0080);
    CHECK(!c.add(TraceEvent{10, status, TraceKind::Bus}, done));
    // IORDY/INTRQ changes don't split a run.
    CHECK(!c.add(TraceEvent{11, status ^ bus::maskOf(cfg::PIN_INTRQ), TraceKind::Bus}, done));
    CHECK(!c.add(TraceEvent{12, status, TraceKind::Bus}, done));
    // A different value ends it.
    uint32_t ready = sample(true, false, true, false, 7, 0x0050);
    CHECK(c.add(TraceEvent{20, ready, TraceKind::Bus}, done));
    CHECK_EQ(done.count, 3u);
    CHECK_EQ(done.first.time_us, 10u);
    CHECK_EQ(done.last_us, 12u);
    CHECK(c.flush(done));
    CHECK_EQ(done.count, 1u);
    CHECK_EQ(done.first.pins, ready);
    CHECK(!c.flush(done));
}

TEST(test_coalescer_never_merges_resets) {
    TraceCoalescer c;
    TraceRun done;
    CHECK(!c.add(TraceEvent{1, 0, TraceKind::ResetAssert}, done));
    CHECK(c.add(TraceEvent{2, 0, TraceKind::ResetAssert}, done));
    CHECK_EQ(done.count, 1u);
    CHECK(c.add(TraceEvent{3, 0, TraceKind::Stall}, done));
    CHECK(done.first.kind == TraceKind::ResetAssert);
}

// ---- SPSC ring ------------------------------------------------------------

TEST(test_ring_basic) {
    SpscRing<int, 4> r;
    int v = 0;
    CHECK(!r.pop(v));
    for (int i = 0; i < 4; ++i) CHECK(r.push(i));
    CHECK(!r.push(99));  // full
    CHECK_EQ(r.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        CHECK(r.pop(v));
        CHECK_EQ(v, i);
    }
    CHECK(!r.pop(v));
}

TEST(test_ring_two_threads) {
    // Mirrors core 1 producing trace events and core 0 consuming them.
    static SpscRing<TraceEvent, 256> r;
    const uint32_t total = 200000;
    std::thread producer([&] {
        for (uint32_t i = 0; i < total;) {
            if (r.push(TraceEvent{i, i * 3u, TraceKind::Bus})) ++i;
        }
    });
    uint32_t expected = 0;
    bool in_order = true;
    while (expected < total) {
        TraceEvent e;
        if (!r.pop(e)) continue;
        if (e.time_us != expected || e.pins != expected * 3u) in_order = false;
        ++expected;
    }
    producer.join();
    CHECK(in_order);
    CHECK_EQ(expected, total);
}

// ---- IDENTIFY -------------------------------------------------------------

static std::string ataString(const uint16_t* w, unsigned words) {
    std::string s;
    for (unsigned i = 0; i < words; ++i) {
        s += char(w[i] >> 8);
        s += char(w[i] & 0xFF);
    }
    return s;
}

TEST(test_ata_string_byte_order) {
    uint16_t w[3];
    putAtaString(w, 3, "ABC");
    CHECK_EQ(w[0], 0x4142);  // 'A' in the high byte
    CHECK_EQ(w[1], 0x4320);  // 'C' then a space pad
    CHECK_EQ(w[2], 0x2020);
    putAtaString(w, 2, "TOOLONGSTRING");
    CHECK_STR(ataString(w, 2), "TOOL");
}

TEST(test_default_geometry) {
    Geometry g = defaultGeometry(131072);  // 64 MB
    CHECK_EQ(g.heads, 16);
    CHECK_EQ(g.sectors_per_track, 63);
    CHECK_EQ(g.cylinders, 130);
    CHECK(g.sectors() <= 131072u);

    g = defaultGeometry(512);  // 256 KB RAM disk
    CHECK_EQ(g.heads, 2);
    CHECK_EQ(g.sectors_per_track, 16);
    CHECK_EQ(g.sectors(), 512u);

    g = defaultGeometry(0x0FFFFFFF);  // the LBA28 maximum caps at 16383 cylinders
    CHECK_EQ(g.cylinders, 16383);
}

TEST(test_identify_fields) {
    uint16_t w[256];
    buildIdentify(IdentifyInfo{"HDDSYNTH PROTO", "HSP-000001", "0.1.0", 131072}, w);
    CHECK_STR(ataString(&w[27], 20), "HDDSYNTH PROTO                          ");
    CHECK_STR(ataString(&w[10], 10), "HSP-000001          ");
    CHECK_STR(ataString(&w[23], 4), "0.1.0   ");
    CHECK_EQ(w[0], 0x0040);
    CHECK_EQ(w[1], 130);
    CHECK_EQ(w[3], 16);
    CHECK_EQ(w[6], 63);
    CHECK_EQ(uint32_t(w[60]) | (uint32_t(w[61]) << 16), 131072u);
    CHECK_EQ(uint32_t(w[57]) | (uint32_t(w[58]) << 16), 130u * 16u * 63u);
    CHECK(w[49] & (1u << 9));    // LBA
    CHECK(w[49] & (1u << 11));   // IORDY
    CHECK(!(w[49] & (1u << 8)));  // no DMA
    CHECK_EQ(w[47] & 0xFF, 0);   // no READ/WRITE MULTIPLE
    CHECK_EQ(w[63], 0);          // no multiword DMA
    CHECK_EQ(w[64], 0);          // no PIO 3/4
    CHECK(!(w[53] & 4));         // word 88 (UDMA) not valid
    CHECK_EQ(w[88], 0);
    CHECK(!(w[83] & (1u << 10)));  // no 48-bit
}

TEST(test_identify_checksum) {
    uint16_t w[256];
    buildIdentify(IdentifyInfo{"HDDSYNTH PROTO", "HSP-000001", "0.1.0", 512}, w);
    CHECK_EQ(w[255] & 0xFF, 0xA5);
    uint8_t sum = 0;
    for (int i = 0; i < 256; ++i) sum += uint8_t(w[i] & 0xFF) + uint8_t(w[i] >> 8);
    CHECK_EQ(sum, 0);
}

int main() {
    RUN(test_pin_layout);
    RUN(test_decode_status_read);
    RUN(test_decode_control_block);
    RUN(test_decode_invalid_combinations);
    RUN(test_format_bus_run);
    RUN(test_format_other_kinds);
    RUN(test_format_truncates);
    RUN(test_coalescer_merges_identical_accesses);
    RUN(test_coalescer_never_merges_resets);
    RUN(test_ring_basic);
    RUN(test_ring_two_threads);
    RUN(test_ata_string_byte_order);
    RUN(test_default_geometry);
    RUN(test_identify_fields);
    RUN(test_identify_checksum);
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
