// Tests for the folder drive's volume (folder_disk): short names, and whole
// volumes read back through a small FAT16 reader that follows the MBR, the
// boot record, the folders and the FAT chains as DOS would.
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ata_device.h"
#include "check.h"
#include "folder_disk.h"
#include "identify.h"

using namespace folder_disk;

namespace {

// ---- A folder in memory ------------------------------------------------------

struct Node {
    std::string name;
    bool dir;
    uint64_t size;  // a file's; its bytes are pattern(n, offset) unless `text` is set
    std::string text;
    std::vector<int> children;
};

uint8_t pattern(int node, uint64_t offset) { return uint8_t((offset * 131 + uint64_t(node) * 7 + (offset >> 9)) & 0xFF); }

class MemoryFolder : public FolderSource {
public:
    MemoryFolder() { nodes.push_back({"", true, 0, "", {}}); }  // node 0: the folder served
    int add(int parent, const std::string& name, bool dir, uint64_t size = 0, const std::string& text = "") {
        nodes.push_back({name, dir, text.empty() ? size : text.size(), text, {}});
        int n = int(nodes.size()) - 1;
        nodes[size_t(parent)].children.push_back(n);
        return n;
    }
    bool list(uint16_t folder, const Entry* entries, Adder& add) override {
        int node = folder == ROOT ? 0 : int(entries[folder].locator);
        for (int c : nodes[size_t(node)].children) {
            const Node& n = nodes[size_t(c)];
            Item item{n.name.c_str(), n.dir, n.size, 0x5A21, 0x6000, uint32_t(c)};
            if (!add.add(item)) break;
        }
        return true;
    }
    bool read(uint16_t entry, const Entry* entries, uint32_t offset, uint8_t* out, uint32_t bytes) override {
        ++reads;
        int node = int(entries[entry].locator);
        const Node& n = nodes[size_t(node)];
        for (uint32_t i = 0; i < bytes; ++i)
            out[i] = n.text.empty() ? pattern(node, offset + i) : uint8_t(n.text[offset + i]);
        return true;
    }
    std::vector<Node> nodes;
    int reads = 0;
};

// ---- A small FAT16 reader -------------------------------------------------------

uint32_t get16(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { return get16(p) | (get16(p + 2) << 16); }

struct FatReader {
    Volume& v;
    uint32_t part, spc, reserved, fats, root_entries, fat_sectors, part_sectors;
    uint32_t fat_start, root_start, data_start, clusters;
    bool ok = true;

    explicit FatReader(Volume& vol) : v(vol) {
        uint8_t s[512];
        v.sector(0, s);
        ok = s[510] == 0x55 && s[511] == 0xAA && s[0x1BE + 4] == 0x06;
        part = get32(s + 0x1BE + 8);
        part_sectors = get32(s + 0x1BE + 12);
        v.sector(part, s);
        ok = ok && s[510] == 0x55 && s[511] == 0xAA && get16(s + 0x0B) == 512 && s[0x15] == 0xF8 &&
             std::memcmp(s + 0x36, "FAT16   ", 8) == 0 && s[0x26] == 0x29;
        spc = s[0x0D];
        reserved = get16(s + 0x0E);
        fats = s[0x10];
        root_entries = get16(s + 0x11);
        uint32_t total = get16(s + 0x13) ? get16(s + 0x13) : get32(s + 0x20);
        ok = ok && total == part_sectors && get32(s + 0x1C) == part;
        fat_sectors = get16(s + 0x16);
        fat_start = part + reserved;
        root_start = fat_start + fats * fat_sectors;
        data_start = root_start + root_entries * 32 / 512;
        clusters = (part + part_sectors - data_start) / spc;
    }
    uint32_t fat(uint32_t n) {
        uint8_t s[512];
        v.sector(fat_start + n / 256, s);
        uint8_t s2[512];
        v.sector(fat_start + fat_sectors + n / 256, s2);  // the second FAT
        if (std::memcmp(s, s2, 512) != 0) ok = false;
        return get16(s + (n % 256) * 2);
    }
    // A chain's clusters, from `first`; checks it ends cleanly.
    std::vector<uint32_t> chain(uint32_t first) {
        std::vector<uint32_t> out;
        for (uint32_t c = first; c >= 2 && c < 0xFFF8; c = fat(c)) {
            if (c >= clusters + 2 || out.size() > clusters) {
                ok = false;
                break;
            }
            out.push_back(c);
        }
        return out;
    }
    std::vector<uint8_t> readChain(uint32_t first, uint64_t bytes) {
        std::vector<uint8_t> data;
        for (uint32_t c : chain(first)) {
            for (uint32_t k = 0; k < spc && data.size() < bytes; ++k) {
                uint8_t s[512];
                v.sector(data_start + (c - 2) * spc + k, s);
                size_t n = size_t(bytes - data.size() < 512 ? bytes - data.size() : 512);
                data.insert(data.end(), s, s + n);
            }
        }
        return data;
    }
    struct DirEntry {
        std::string name;  // "NAME.EXT"
        uint8_t attr;
        uint32_t cluster, size;
    };
    std::vector<DirEntry> parse(const std::vector<uint8_t>& raw) {
        std::vector<DirEntry> out;
        for (size_t o = 0; o + 32 <= raw.size(); o += 32) {
            const uint8_t* p = raw.data() + o;
            if (p[0] == 0) break;
            if (p[0] == 0xE5 || (p[11] & 0x08)) continue;  // deleted, volume label
            std::string base(reinterpret_cast<const char*>(p), 8), ext(reinterpret_cast<const char*>(p + 8), 3);
            while (!base.empty() && base.back() == ' ') base.pop_back();
            while (!ext.empty() && ext.back() == ' ') ext.pop_back();
            out.push_back({ext.empty() ? base : base + "." + ext, p[11], get16(p + 0x1A), get32(p + 0x1C)});
        }
        return out;
    }
    std::vector<DirEntry> root() {
        std::vector<uint8_t> raw;
        for (uint32_t s = root_start; s < data_start; ++s) {
            uint8_t b[512];
            v.sector(s, b);
            raw.insert(raw.end(), b, b + 512);
        }
        return parse(raw);
    }
    std::vector<DirEntry> folder(uint32_t cluster) {
        return parse(readChain(cluster, uint64_t(chain(cluster).size()) * spc * 512));
    }
};

std::string name83(const char* long_name, std::vector<std::string> taken = {}) {
    char out[11];
    struct Ctx {
        std::vector<std::string>* taken;
    } ctx{&taken};
    shortName(long_name, out,
              [](const char n[11], void* c) {
                  for (auto& t : *static_cast<Ctx*>(c)->taken)
                      if (t == std::string(n, 11)) return true;
                  return false;
              },
              &ctx);
    return std::string(out, 11);
}

}  // namespace

TEST(test_folder_short_names) {
    CHECK_STR(name83("README.TXT"), "README  TXT");
    CHECK_STR(name83("readme.txt"), "README  TXT");  // case alone isn't lossy
    CHECK_STR(name83("Long File Name.text"), "LONGFI~1TEX");
    CHECK_STR(name83("a+b.c"), "A_B~1   C  ");
    CHECK_STR(name83("archive.tar.gz"), "ARCHIV~1GZ ");
    CHECK_STR(name83("MAKEFILE"), "MAKEFILE   ");
    CHECK_STR(name83(".hidden"), "HIDDEN     ");
    CHECK_STR(name83("..."), "_~1        ");
    CHECK_STR(name83("README.TXT", {"README  TXT"}), "README~1TXT");
    CHECK_STR(name83("longfilename.txt", {"LONGFI~1TXT", "LONGFI~2TXT"}), "LONGFI~3TXT");
    std::vector<std::string> nine;
    for (int i = 1; i <= 9; ++i) nine.push_back("LONGFI~" + std::to_string(i) + "TXT");
    CHECK_STR(name83("longfilename.txt", nine), "LONGF~10TXT");
}

TEST(test_folder_volume_reads_back) {
    MemoryFolder f;
    f.add(0, "readme.txt", false, 0, "Hello from the folder drive.\r\n");
    int games = f.add(0, "Games", true);
    f.add(games, "doom.wad", false, 4196020);
    int sub = f.add(games, "Saves", true);
    f.add(sub, "slot1.sav", false, 2048);  // exactly one cluster
    f.add(sub, "empty.txt", false, 0);
    f.add(0, "Empty Folder", true);
    f.add(0, "odd size.bin", false, 2049);
    std::vector<Entry> entries(64);
    Volume v;
    CHECK(v.build(f, entries.data(), uint16_t(entries.size()), "GAMES"));
    CHECK(v.truncated() == Truncated::No);
    CHECK_EQ(v.count(), 8u);
    CHECK_EQ(v.sectors() % (HEADS * SECTORS_PER_TRACK), 0u);  // whole cylinders
    CHECK_EQ(v.partSectors() % SECTORS_PER_TRACK, 0u);       // whole tracks, as FDISK makes them
    CHECK_EQ(v.partStart() + v.partSectors(), v.sectors());   // to the disk's end
    CHECK(v.clusters() >= 4085u && v.clusters() <= 65524u);   // FAT16 by its cluster count

    FatReader r(v);
    CHECK(r.ok);
    CHECK_EQ(r.clusters, v.clusters());
    auto root = r.root();
    CHECK_EQ(root.size(), 4u);
    std::map<std::string, FatReader::DirEntry> byName;
    for (auto& e : root) byName[e.name] = e;
    CHECK(byName.count("README.TXT") == 1);
    CHECK(byName.count("GAMES") == 1);
    CHECK(byName.count("EMPTYF~1") == 1);
    CHECK(byName.count("ODDSIZ~1.BIN") == 1);
    auto readme = r.readChain(byName["README.TXT"].cluster, byName["README.TXT"].size);
    CHECK_STR(std::string(readme.begin(), readme.end()), "Hello from the folder drive.\r\n");
    CHECK(byName["README.TXT"].attr & ATTR_READ_ONLY);

    auto gamesDir = r.folder(byName["GAMES"].cluster);
    CHECK_EQ(gamesDir.size(), 4u);  // ".", "..", DOOM.WAD, SAVES
    CHECK_STR(gamesDir[0].name, ".");
    CHECK_EQ(gamesDir[0].cluster, byName["GAMES"].cluster);
    CHECK_STR(gamesDir[1].name, "..");
    CHECK_EQ(gamesDir[1].cluster, 0u);  // the root
    CHECK_STR(gamesDir[2].name, "DOOM.WAD");
    CHECK_EQ(gamesDir[2].size, 4196020u);
    auto doom = r.readChain(gamesDir[2].cluster, gamesDir[2].size);
    CHECK_EQ(doom.size(), size_t(4196020));
    int doomNode = 3;
    bool same = true;
    for (size_t i = 0; i < doom.size(); ++i)
        if (doom[i] != pattern(doomNode, i)) {
            same = false;
            break;
        }
    CHECK(same);

    auto saves = r.folder(gamesDir[3].cluster);
    CHECK_EQ(saves.size(), 4u);
    CHECK_EQ(saves[1].cluster, byName["GAMES"].cluster);  // ".." is GAMES
    CHECK_STR(saves[3].name, "EMPTY.TXT");
    CHECK_EQ(saves[3].cluster, 0u);  // no data, no cluster
    CHECK_EQ(saves[3].size, 0u);
    auto slot = r.readChain(saves[2].cluster, saves[2].size);
    CHECK_EQ(slot.size(), 2048u);
    CHECK(r.ok);  // every chain ended cleanly, and the two FATs agree
}

TEST(test_folder_volume_cluster_size_grows) {
    MemoryFolder f;
    f.add(0, "big1.bin", false, 700u * 1024 * 1024);
    f.add(0, "big2.bin", false, 700u * 1024 * 1024);
    std::vector<Entry> entries(8);
    Volume v;
    CHECK(v.build(f, entries.data(), 8, "BIG"));
    CHECK_EQ(v.clusterBytes(), 32768u);  // 1.4 GB needs 32 KB clusters
    CHECK_EQ(v.partSectors() % SECTORS_PER_TRACK, 0u);
    FatReader r(v);
    CHECK(r.ok);
    auto root = r.root();
    CHECK_EQ(root.size(), 2u);
    // The last byte of the second file, through its chain's last cluster.
    auto chain = r.chain(root[1].cluster);
    CHECK_EQ(chain.size(), size_t(700u * 1024 * 1024 / 32768));
    uint8_t s[512];
    uint32_t last_lba = r.data_start + (chain.back() - 2) * r.spc + r.spc - 1;
    CHECK(v.sector(last_lba, s));
    CHECK_EQ(s[511], pattern(2, 700ull * 1024 * 1024 - 1));
    CHECK(r.ok);
}

TEST(test_folder_volume_size_budget) {
    // 2.5 GB of files: FAT16 holds 2 GB, so the third file is left out.
    MemoryFolder f;
    f.add(0, "one.bin", false, 900u * 1024 * 1024);
    f.add(0, "two.bin", false, 900u * 1024 * 1024);
    f.add(0, "three.bin", false, 700u * 1024 * 1024);
    f.add(0, "small.txt", false, 0, "fits");
    std::vector<Entry> entries(8);
    Volume v;
    CHECK(v.build(f, entries.data(), 8, "X"));
    CHECK(v.truncated() == Truncated::Size);
    FatReader r(v);
    auto root = r.root();
    CHECK_EQ(root.size(), 3u);
    CHECK_STR(root[2].name, "SMALL.TXT");  // smaller files after it still go in
    CHECK(r.ok);
}

TEST(test_folder_volume_entry_cap) {
    MemoryFolder f;
    int d = f.add(0, "many", true);
    for (int i = 0; i < 100; ++i) f.add(d, "file" + std::to_string(i) + ".txt", false, 0, "x");
    std::vector<Entry> entries(50);
    Volume v;
    CHECK(v.build(f, entries.data(), 50, "CAP"));
    CHECK(v.truncated() == Truncated::Entries);
    CHECK_EQ(v.count(), 50u);
    FatReader r(v);
    auto root = r.root();
    CHECK_EQ(root.size(), 1u);
    CHECK_EQ(r.folder(root[0].cluster).size(), size_t(2 + 49));
    CHECK(r.ok);
}

TEST(test_folder_volume_root_full) {
    MemoryFolder f;
    for (int i = 0; i < 600; ++i) f.add(0, "f" + std::to_string(i), false, 0, "y");
    std::vector<Entry> entries(1024);
    Volume v;
    CHECK(v.build(f, entries.data(), 1024, "ROOT"));
    CHECK(v.truncated() == Truncated::RootFull);
    CHECK_EQ(v.count(), uint16_t(ROOT_CHILDREN_MAX));
    FatReader r(v);
    CHECK_EQ(r.root().size(), size_t(ROOT_CHILDREN_MAX));
    CHECK(r.ok);
}

TEST(test_folder_volume_big_subfolder_spans_clusters) {
    // 300 entries in one folder: 302 x 32 bytes, five 2 KB clusters.
    MemoryFolder f;
    int d = f.add(0, "lots", true);
    for (int i = 0; i < 300; ++i) f.add(d, "n" + std::to_string(i) + ".dat", false, 0, "z" + std::to_string(i));
    std::vector<Entry> entries(512);
    Volume v;
    CHECK(v.build(f, entries.data(), 512, "LOTS"));
    FatReader r(v);
    auto root = r.root();
    CHECK_EQ(r.chain(root[0].cluster).size(), 5u);
    auto lots = r.folder(root[0].cluster);
    CHECK_EQ(lots.size(), 302u);
    CHECK_STR(lots[301].name, "N299.DAT");
    auto data = r.readChain(lots[301].cluster, lots[301].size);
    CHECK_STR(std::string(data.begin(), data.end()), "z299");
    CHECK(r.ok);
}

TEST(test_folder_disk_is_read_only) {
    MemoryFolder f;
    f.add(0, "a.txt", false, 0, "abc");
    std::vector<Entry> entries(4);
    Volume v;
    CHECK(v.build(f, entries.data(), 4, "RO"));
    FolderDisk disk(v);
    uint16_t words[256];
    CHECK(disk.read(0, words));
    CHECK_EQ(words[255], 0xAA55u);  // the MBR's signature, as bus words
    CHECK(!disk.write(100, words));
    CHECK(!disk.read(disk.sectors(), words));
}

TEST(test_folder_disk_as_device_1) {
    // The whole path: a master on a generated disk and the folder as device
    // 1, read through ATA commands as a host would, each fetch to its unit.
    MemoryFolder f;
    f.add(0, "notes.txt", false, 0, "Served from a folder, read-only.");
    f.add(0, "data.bin", false, 70000);
    std::vector<Entry> entries(16);
    Volume v;
    CHECK(v.build(f, entries.data(), 16, "SLAVE"));
    FolderDisk slave(v);
    GeneratedDisk master;
    const IdentifyInfo id0{"HDDSYNTH PROTO", "HSP-000001", "0.29.0", master.sectors()};
    const IdentifyInfo id1{"HDDSYNTH PRO S", "HSP-000001-S", "0.29.0", slave.sectors()};
    ata::Device d(id0, &id1);
    BlockSource* disks[2] = {&master, &slave};
    auto pump = [&] {
        ata::Fetch fe;
        if (d.dataOutHeld()) d.releaseDataOut();
        if (d.pendingFetch(fe))
            d.fetchDone(fe.id, fe.store ? disks[fe.unit]->write(fe.lba, fe.buffer) : disks[fe.unit]->read(fe.lba, fe.buffer));
    };
    auto wait = [&] {
        for (int i = 0; i < 1000 && (d.read(ata::Reg::AltStatusDevCtrl) & ata::ST_BSY); ++i) pump();
    };
    auto readSector = [&](uint32_t lba, uint8_t out[512]) {
        d.write(ata::Reg::SectorCount, 1);
        d.write(ata::Reg::LbaLow, lba & 0xFF);
        d.write(ata::Reg::LbaMid, (lba >> 8) & 0xFF);
        d.write(ata::Reg::LbaHigh, (lba >> 16) & 0xFF);
        d.write(ata::Reg::Device, 0xF0);  // LBA, device 1
        d.write(ata::Reg::StatusCommand, ata::CMD_READ_SECTORS);
        wait();
        if ((d.read(ata::Reg::StatusCommand) & (ata::ST_ERR | ata::ST_DRQ)) != ata::ST_DRQ) return false;
        for (int i = 0; i < 256; ++i) {
            uint16_t w = d.read(ata::Reg::Data);
            out[2 * i] = uint8_t(w);
            out[2 * i + 1] = uint8_t(w >> 8);
        }
        wait();
        return true;
    };
    // Every sector of the volume's metadata and files, as the host reads it.
    uint8_t got[512], want[512];
    bool same = true;
    for (uint32_t lba = 0; lba < 400; ++lba) {
        if (!readSector(lba, got)) {
            same = false;
            break;
        }
        v.sector(lba, want);
        if (std::memcmp(got, want, 512) != 0) same = false;
    }
    CHECK(same);
    // A write to the folder drive fails cleanly with ABRT; the master still writes.
    d.write(ata::Reg::SectorCount, 1);
    d.write(ata::Reg::Device, 0xF0);
    d.write(ata::Reg::StatusCommand, ata::CMD_WRITE_SECTORS);
    wait();
    for (int i = 0; i < 256; ++i) d.write(ata::Reg::Data, 0xBEEF);
    wait();
    CHECK(d.read(ata::Reg::StatusCommand) & ata::ST_ERR);
    CHECK_EQ(d.read(ata::Reg::ErrorFeatures) & ata::ER_ABRT, ata::ER_ABRT);
}

void runFolderDiskTests() {
    RUN(test_folder_disk_as_device_1);
    RUN(test_folder_short_names);
    RUN(test_folder_volume_reads_back);
    RUN(test_folder_volume_cluster_size_grows);
    RUN(test_folder_volume_size_budget);
    RUN(test_folder_volume_entry_cap);
    RUN(test_folder_volume_root_full);
    RUN(test_folder_volume_big_subfolder_spans_clusters);
    RUN(test_folder_disk_is_read_only);
}
