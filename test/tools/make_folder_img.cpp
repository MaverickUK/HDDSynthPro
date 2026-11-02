// Writes the disk the folder drive would serve for a directory, to see it
// as a computer will and check it with real FAT tools (fsck.fat, mtools,
// Windows) before trying it on a retro PC (docs/two-drives.md).
// Usage: make_folder_img <directory> <out.img> [max entries, default 1024]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "folder_disk.h"

namespace fs = std::filesystem;
using namespace folder_disk;

// The directory's files and folders, sorted by name (as a listing would be);
// hidden ones (a leading dot) left out, as the drive leaves out the card's.
class DirectorySource : public FolderSource {
public:
    explicit DirectorySource(const fs::path& root) { paths_.push_back(root); }
    bool list(uint16_t folder, const Entry* entries, Adder& add) override {
        const fs::path& dir = paths_[folder == ROOT ? 0 : entries[folder].locator];
        std::vector<fs::directory_entry> items;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().filename().string()[0] != '.') items.push_back(e);
        if (ec) return false;
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.path() < b.path(); });
        for (const auto& e : items) {
            paths_.push_back(e.path());
            std::string name = e.path().filename().string();
            bool dir_ = e.is_directory();
            Item item{name.c_str(), dir_, dir_ ? 0 : uint64_t(e.file_size()), DATE, TIME,
                      uint32_t(paths_.size() - 1)};
            if (!add.add(item)) break;
        }
        return true;
    }
    bool read(uint16_t entry, const Entry* entries, uint32_t offset, uint8_t* out, uint32_t bytes) override {
        uint32_t locator = entries[entry].locator;
        if (locator != open_locator_) {
            file_.close();
            file_.open(paths_[locator], std::ios::binary);
            open_locator_ = locator;
        }
        file_.clear();
        file_.seekg(offset);
        file_.read(reinterpret_cast<char*>(out), bytes);
        return file_.gcount() == std::streamsize(bytes);
    }

private:
    static constexpr uint16_t DATE = ((2026 - 1980) << 9) | (11 << 5) | 2;  // 2 Nov 2026
    static constexpr uint16_t TIME = 12 << 11;                               // 12:00
    std::vector<fs::path> paths_;
    std::ifstream file_;
    uint32_t open_locator_ = 0xFFFFFFFF;
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: make_folder_img <directory> <out.img> [max entries]\n");
        return 2;
    }
    unsigned capacity = argc > 3 ? unsigned(std::atoi(argv[3])) : 1024;
    std::vector<Entry> entries(capacity);
    DirectorySource source{fs::path(argv[1])};
    Volume volume;
    std::string label = fs::path(argv[1]).filename().string();
    if (!volume.build(source, entries.data(), uint16_t(capacity), label.c_str())) {
        std::fprintf(stderr, "make_folder_img: couldn't build a volume from %s\n", argv[1]);
        return 1;
    }
    static const char* why[] = {"", " (cut short: too many entries)", " (cut short: the root folder is full)",
                                " (cut short: over 2 GB)"};
    std::printf("%u entries, %u clusters of %u bytes, %lu sectors%s\n", unsigned(volume.count()),
                unsigned(volume.clusters()), unsigned(volume.clusterBytes()), (unsigned long)volume.sectors(),
                why[int(volume.truncated())]);
    std::ofstream out(argv[2], std::ios::binary);
    uint8_t sector[SECTOR_BYTES];
    for (uint32_t lba = 0; lba < volume.sectors(); ++lba) {
        if (!volume.sector(lba, sector)) {
            std::fprintf(stderr, "make_folder_img: couldn't read sector %lu\n", (unsigned long)lba);
            return 1;
        }
        out.write(reinterpret_cast<const char*>(sector), sizeof sector);
    }
    return out ? 0 : 1;
}
