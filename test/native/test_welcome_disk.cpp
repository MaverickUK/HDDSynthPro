// Host-side tests for the welcome disk (welcome_disk.*): the partition table
// and FAT16 volume a computer finds on it, files read back by following the
// FAT as an operating system would, and the README.TXT text it carries.
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "drive_config.h"
#include "welcome_disk.h"
#include "welcome_text.h"

using namespace welcome_disk;

static constexpr uint16_t DATE = (2026 - 1980) << 9 | 10 << 5 | 28;

static uint32_t get16(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

// The disk's bytes, as the host sees them over ATA.
struct Reader {
    WelcomeDisk& disk;
    std::vector<uint8_t> sector(uint32_t lba) {
        uint16_t words[256];
        std::vector<uint8_t> bytes(512, 0xEE);
        if (!disk.read(lba, words)) return {};
        for (int i = 0; i < 256; ++i) {
            bytes[2 * i] = uint8_t(words[i]);
            bytes[2 * i + 1] = uint8_t(words[i] >> 8);
        }
        return bytes;
    }
};

// A minimal FAT16 reader: the volume the MBR's first partition holds.
struct Volume {
    Reader& r;
    uint32_t start, per_cluster, fat, root, root_sectors, data;

    explicit Volume(Reader& reader) : r(reader) {
        auto mbr = r.sector(0);
        start = get32(&mbr[0x1BE + 8]);
        auto boot = r.sector(start);
        per_cluster = boot[0x0D];
        fat = start + get16(&boot[0x0E]);
        root = fat + boot[0x10] * get16(&boot[0x16]);
        root_sectors = get16(&boot[0x11]) * 32 / 512;
        data = root + root_sectors;
    }

    uint32_t fatEntry(uint32_t n) { return get16(&r.sector(fat + n / 256)[n % 256 * 2]); }

    // The root folder entry named `name83` ("README  TXT"), or empty.
    std::vector<uint8_t> entry(const char* name83) {
        for (uint32_t s = 0; s < root_sectors; ++s) {
            auto sec = r.sector(root + s);
            for (int e = 0; e < 16; ++e) {
                const uint8_t* p = &sec[32 * e];
                if (std::memcmp(p, name83, 11) == 0) return std::vector<uint8_t>(p, p + 32);
            }
        }
        return {};
    }

    // A file's contents, by its FAT chain; "<missing>" if it isn't there.
    std::string file(const char* name83) {
        auto e = entry(name83);
        if (e.empty()) return "<missing>";
        uint32_t size = get32(&e[0x1C]);
        std::string out;
        for (uint32_t c = get16(&e[0x1A]); c >= 2 && c < 0xFFF8; c = fatEntry(c)) {
            for (uint32_t s = 0; s < per_cluster; ++s) {
                auto sec = r.sector(data + (c - 2) * per_cluster + s);
                out.append(sec.begin(), sec.end());
            }
            if (out.size() > 1u << 20) return "<chain loops>";
        }
        if (out.size() < size) return "<chain too short>";
        out.resize(size);
        return out;
    }
};

TEST(test_welcome_layout) {
    // 64 MB as 130 cylinders of 16 heads and 63 sectors; the partition
    // fills cylinders 0-129 after the MBR's track.
    CHECK_EQ(SECTORS, 131072u);
    CHECK_EQ(CYLINDERS, 130u);
    CHECK_EQ(PART_START, 63u);
    CHECK_EQ(PART_START + PART_SECTORS, 130u * 16 * 63);
    CHECK_EQ(FAT_SECTORS, 128u);
    CHECK_EQ(DATA_START, 63u + 1 + 2 * 128 + 32);
    CHECK_EQ(CLUSTERS, 32672u);
}

TEST(test_welcome_partition_table) {
    WelcomeDisk disk(nullptr, 0, DATE);
    Reader r{disk};
    auto mbr = r.sector(0);
    CHECK_EQ(mbr[510], 0x55);
    CHECK_EQ(mbr[511], 0xAA);
    CHECK_EQ(mbr[0], 0xCD);  // int 18h: not bootable
    CHECK_EQ(get32(&mbr[0x1B8]), VOLUME_ID);
    const uint8_t* p = &mbr[0x1BE];
    CHECK_EQ(p[0], 0x00);
    CHECK_EQ(p[4], 0x06);
    // CHS: start cylinder 0, head 1, sector 1; end cylinder 129, head 15, sector 63.
    CHECK_EQ(p[1], 1);
    CHECK_EQ(p[2], 1);
    CHECK_EQ(p[3], 0);
    CHECK_EQ(p[5], 15);
    CHECK_EQ(p[6], 63);
    CHECK_EQ(p[7], 129);
    CHECK_EQ(get32(&p[8]), PART_START);
    CHECK_EQ(get32(&p[12]), PART_SECTORS);
    for (int i = 0x1CE; i < 0x1FE; ++i) CHECK_EQ(mbr[i], 0);  // the other three entries
}

TEST(test_welcome_boot_record) {
    WelcomeDisk disk(nullptr, 0, DATE);
    Reader r{disk};
    auto b = r.sector(PART_START);
    CHECK_EQ(b[0], 0xEB);
    CHECK_EQ(b[2], 0x90);
    CHECK_EQ(get16(&b[0x0B]), 512u);
    CHECK_EQ(b[0x0D], 4);
    CHECK_EQ(get16(&b[0x0E]), 1u);
    CHECK_EQ(b[0x10], 2);
    CHECK_EQ(get16(&b[0x11]), 512u);
    CHECK_EQ(get16(&b[0x13]), 0u);  // over 65535 sectors: the 32-bit count instead
    CHECK_EQ(b[0x15], 0xF8);
    CHECK_EQ(get16(&b[0x16]), FAT_SECTORS);
    CHECK_EQ(get16(&b[0x18]), 63u);
    CHECK_EQ(get16(&b[0x1A]), 16u);
    CHECK_EQ(get32(&b[0x1C]), PART_START);
    CHECK_EQ(get32(&b[0x20]), PART_SECTORS);
    CHECK_EQ(b[0x26], 0x29);
    CHECK_STR(std::string(reinterpret_cast<const char*>(&b[0x2B]), 11), "HDDSYNTHPRO");
    CHECK_STR(std::string(reinterpret_cast<const char*>(&b[0x36]), 8), "FAT16   ");
    CHECK_EQ(b[510], 0x55);
    CHECK_EQ(b[511], 0xAA);
}

TEST(test_welcome_files_read_back) {
    // A file over two clusters, an empty one and a one-byte one.
    std::string big(5000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char('a' + i % 26);
    const File files[] = {
        {"BIG.TXT", big.data(), uint32_t(big.size())},
        {"EMPTY", "", 0},
        {"ONE.B", "!", 1},
    };
    WelcomeDisk disk(files, 3, DATE);
    Reader r{disk};
    Volume v(r);
    CHECK_STR(v.file("BIG     TXT"), big);
    CHECK_STR(v.file("EMPTY      "), "");
    CHECK_STR(v.file("ONE     B  "), "!");
    // BIG.TXT is clusters 2-4, ONE.B cluster 5; the empty file has none.
    CHECK_EQ(v.fatEntry(0), 0xFFF8u);
    CHECK_EQ(v.fatEntry(2), 3u);
    CHECK_EQ(v.fatEntry(4), 0xFFFFu);
    CHECK_EQ(v.fatEntry(5), 0xFFFFu);
    CHECK_EQ(v.fatEntry(6), 0u);
    CHECK_EQ(get16(&v.entry("EMPTY      ")[0x1A]), 0u);
    CHECK_EQ(get16(&v.entry("BIG     TXT")[0x18]), uint32_t(DATE));
    // The second FAT is a copy of the first.
    for (uint32_t s = 0; s < FAT_SECTORS; s += 37)
        CHECK(r.sector(FAT_START + s) == r.sector(FAT_START + FAT_SECTORS + s));
    // The volume label comes first in the root folder.
    auto root = r.sector(ROOT_START);
    CHECK_STR(std::string(reinterpret_cast<const char*>(&root[0]), 11), "HDDSYNTHPRO");
    CHECK_EQ(root[11], 0x08);
}

TEST(test_welcome_edges) {
    WelcomeDisk disk(nullptr, 0, DATE);
    uint16_t words[256];
    CHECK(disk.read(SECTORS - 1, words));
    CHECK(!disk.read(SECTORS, words));
    // Past the partition, and free clusters, read as zeros.
    bool zeros = true;
    for (uint16_t w : words) zeros = zeros && w == 0;
    CHECK(zeros);
    CHECK(disk.read(DATA_START + 100, words));
    zeros = true;
    for (uint16_t w : words) zeros = zeros && w == 0;
    CHECK(zeros);
}

TEST(test_welcome_disk_as_served) {
    // What main.cpp serves: README.TXT and the settings file's defaults.
    const File files[] = {
        {"README.TXT", welcome_text::README, welcome_text::README_SIZE},
        {drive_config::FILE_NAME, drive_config::defaultFile(), uint32_t(std::strlen(drive_config::defaultFile()))},
    };
    WelcomeDisk disk(files, 2, welcome_text::FAT_DATE);
    Reader r{disk};
    Volume v(r);
    CHECK_STR(v.file("README  TXT"), std::string(welcome_text::README, welcome_text::README_SIZE));
    CHECK_STR(v.file("HDDSYNTHINI"), drive_config::defaultFile());
}

TEST(test_welcome_readme_text) {
    // scripts/embed_welcome.py's promises: ASCII, CRLF, 80 columns, the version filled in.
    std::string text(welcome_text::README, welcome_text::README_SIZE);
    CHECK(text.size() > 100);
    CHECK(text.size() < 256 * 1024);
    CHECK(text.find("{VERSION}") == std::string::npos);
    CHECK(text.size() >= 2 && text.compare(text.size() - 2, 2, "\r\n") == 0);
    size_t line_start = 0;
    bool ok = true;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n') {
            ok = ok && i > 0 && text[i - 1] == '\r' && i - 1 - line_start <= 80;
            line_start = i + 1;
        } else if (c != '\r') {
            ok = ok && c >= 0x20 && c <= 0x7E;
        }
    }
    CHECK(ok);
}

TEST(test_overlay_set_base) {
    // main.cpp's one RAM overlay, put on the disk chosen at boot.
    GeneratedDisk generated;
    WelcomeDisk welcome(nullptr, 0, DATE);
    RamOverlayDisk<2> overlay(welcome);
    uint16_t a[256], b[256];
    overlay.read(PART_START, a);
    welcome.read(PART_START, b);
    CHECK(std::memcmp(a, b, sizeof a) == 0);
    overlay.setBase(generated);
    overlay.read(PART_START, a);
    generated.read(PART_START, b);
    CHECK(std::memcmp(a, b, sizeof a) == 0);
}

void runWelcomeDiskTests() {
    RUN(test_welcome_layout);
    RUN(test_welcome_partition_table);
    RUN(test_welcome_boot_record);
    RUN(test_welcome_files_read_back);
    RUN(test_welcome_edges);
    RUN(test_welcome_disk_as_served);
    RUN(test_welcome_readme_text);
    RUN(test_overlay_set_base);
}
