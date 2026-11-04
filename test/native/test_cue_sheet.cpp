// Tests for CUE sheets and the raw data track (cue_sheet).
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "cue_sheet.h"

using cue_sheet::Sheet;

static bool parse(const char* text, Sheet& s) { return cue_sheet::parse(text, std::strlen(text), s); }

TEST(test_cue_one_data_track) {
    Sheet s;
    CHECK(parse("FILE \"DOOM.BIN\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n", s));
    CHECK_STR(s.file, "DOOM.BIN");
    CHECK_EQ(s.track.stride, 2352u);
    CHECK_EQ(s.track.data_offset, 16u);
    CHECK_EQ(s.track.first_frame, 0u);
    CHECK_EQ(s.track.end_frame, 0u);  // to the file's end
    CHECK_EQ(s.tracks, 1);
    CHECK_EQ(cue_sheet::frames(s.track, 2352ull * 1000), 1000u);
    CHECK_EQ(cue_sheet::frames(s.track, 2352ull * 1000 + 100), 1000u);  // a part frame at the end is left out
}

TEST(test_cue_mixed_mode_stops_at_the_audio) {
    // A game with its music as audio tracks in the same BIN: the data ends
    // where track 2's pregap starts (INDEX 00).
    const char* text =
        "REM GENRE Game\r\n"
        "FILE \"GAME (Disc 1).bin\" BINARY\r\n"
        "  TRACK 01 MODE1/2352\r\n"
        "    INDEX 01 00:00:00\r\n"
        "  TRACK 02 AUDIO\r\n"
        "    INDEX 00 32:10:40\r\n"
        "    INDEX 01 32:12:40\r\n"
        "  TRACK 03 AUDIO\r\n"
        "    INDEX 01 35:00:00\r\n";
    Sheet s;
    CHECK(parse(text, s));
    CHECK_STR(s.file, "GAME (Disc 1).bin");
    CHECK_EQ(s.track.end_frame, (32u * 60 + 10) * 75 + 40);
    CHECK_EQ(s.tracks, 3);
    CHECK_EQ(cue_sheet::frames(s.track, 2352ull * 200000), (32u * 60 + 10) * 75 + 40);
}

TEST(test_cue_tracks_in_their_own_files) {
    // Redump's layout: one file per track. Track 1 is all of the first file.
    const char* text =
        "FILE \"GAME (Track 1).bin\" BINARY\n"
        "  TRACK 01 MODE1/2352\n"
        "    INDEX 01 00:00:00\n"
        "FILE \"GAME (Track 2).bin\" BINARY\n"
        "  TRACK 02 AUDIO\n"
        "    INDEX 00 00:00:00\n"
        "    INDEX 01 00:02:00\n";
    Sheet s;
    CHECK(parse(text, s));
    CHECK_STR(s.file, "GAME (Track 1).bin");
    CHECK_EQ(s.track.end_frame, 0u);
    CHECK_EQ(s.tracks, 2);
}

TEST(test_cue_modes) {
    Sheet s;
    CHECK(parse("FILE A.ISO BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n", s));
    CHECK_EQ(s.track.stride, 2048u);
    CHECK_EQ(s.track.data_offset, 0u);
    CHECK_STR(s.file, "A.ISO");  // unquoted
    CHECK(parse("FILE \"A.BIN\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\n", s));
    CHECK_EQ(s.track.stride, 2352u);
    CHECK_EQ(s.track.data_offset, 24u);
    CHECK(parse("file \"a.bin\" binary\n track 01 mode2/2336\n index 01 00:00:00\n", s));  // any case
    CHECK_EQ(s.track.stride, 2336u);
    CHECK_EQ(s.track.data_offset, 8u);
}

TEST(test_cue_details) {
    Sheet s;
    // A UTF-8 BOM, a folder in the name, an index past the start.
    CHECK(parse("\xEF\xBB\xBF" "FILE \"C:\\Games\\Myst\\MYST.BIN\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:02:00\n", s));
    CHECK_STR(s.file, "MYST.BIN");
    CHECK_EQ(s.track.first_frame, 150u);
    CHECK_EQ(cue_sheet::frames(s.track, 2352ull * 1000), 850u);
    CHECK(parse("FILE \"games/x.bin\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n", s));
    CHECK_STR(s.file, "x.bin");
    // An index number without its leading zero, as some tools write it.
    CHECK(parse("FILE x.bin BINARY\nTRACK 1 MODE1/2352\nINDEX 1 00:00:30\n", s));
    CHECK_EQ(s.track.first_frame, 30u);
    // PREGAP isn't in the file, so it changes nothing.
    CHECK(parse("FILE x.bin BINARY\nTRACK 01 MODE1/2352\nPREGAP 00:02:00\nINDEX 01 00:00:00\n", s));
    CHECK_EQ(s.track.first_frame, 0u);
}

TEST(test_cue_refused) {
    Sheet s;
    CHECK(!parse("FILE \"MUSIC.BIN\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", s));
    CHECK_STR(s.error, "the first track is audio: no data to serve");
    CHECK(!parse("FILE \"A.WAV\" WAVE\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n", s));
    CHECK_STR(s.error, "the data isn't in a BINARY file");
    CHECK(!parse("FILE \"A.BIN\" BINARY\nTRACK 01 CDG\nINDEX 01 00:00:00\n", s));
    CHECK_STR(s.error, "a track mode it doesn't know");
    CHECK(!parse("TRACK 01 MODE1/2352\nINDEX 01 00:00:00\n", s));
    CHECK_STR(s.error, "no FILE before the first TRACK");
    CHECK(!parse("REM nothing here\n", s));
    CHECK_STR(s.error, "no FILE");
    CHECK(!parse("FILE A.BIN BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:61:00\n", s));
    CHECK_STR(s.error, "an INDEX line it can't read");
    CHECK(!parse("FILE A.BIN BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:02:00\nTRACK 02 AUDIO\nINDEX 01 00:01:00\n", s));
    CHECK_STR(s.error, "the data track is empty");
    std::string long_name = "FILE \"" + std::string(70, 'A') + ".BIN\" BINARY\n";
    CHECK(!parse(long_name.c_str(), s));
    CHECK_STR(s.error, "a FILE name too long");
}

TEST(test_cue_names) {
    CHECK(cue_sheet::isCueName("GAME.CUE"));
    CHECK(cue_sheet::isCueName("game.cue"));
    CHECK(!cue_sheet::isCueName("GAME.ISO"));
    CHECK(!cue_sheet::isCueName(".CUE"));
    CHECK(!cue_sheet::isCueName("CUE"));
}

namespace {
// A BIN in memory: raw frames whose data bytes say which frame and byte they are.
struct MemoryFile : ImageFile {
    std::vector<uint8_t> bytes;
    int reads = 0;
    uint64_t size() const override { return bytes.size(); }
    bool readAt(uint64_t offset, void* data, size_t n) override {
        ++reads;
        if (offset + n > bytes.size()) return false;
        std::memcpy(data, bytes.data() + offset, n);
        return true;
    }
    bool writeAt(uint64_t, const void*, size_t) override { return false; }
};

uint8_t dataByte(uint32_t frame, uint32_t i) { return uint8_t(frame * 7 + i * 13 + (i >> 8)); }

MemoryFile rawBin(uint32_t frames, uint32_t stride, uint32_t offset) {
    MemoryFile f;
    f.bytes.assign(size_t(frames) * stride, 0xEE);  // sync, header, EDC/ECC: anything but the data
    for (uint32_t fr = 0; fr < frames; ++fr)
        for (uint32_t i = 0; i < 2048; ++i) f.bytes[size_t(fr) * stride + offset + i] = dataByte(fr, i);
    return f;
}
}  // namespace

TEST(test_raw_track_disk_reads_the_data) {
    MemoryFile bin = rawBin(10, 2352, 16);
    RawTrackDisk disk(bin);
    cue_sheet::Track t;
    t.stride = 2352;
    t.data_offset = 16;
    t.first_frame = 2;
    t.end_frame = 9;
    disk.setTrack(t);
    CHECK_EQ(disk.sectors(), 7u * 4);  // frames 2-8, four sectors each
    uint16_t words[256];
    bool all = true;
    for (uint32_t lba = 0; lba < disk.sectors(); ++lba) {
        CHECK(disk.read(lba, words));
        const uint8_t* b = reinterpret_cast<const uint8_t*>(words);
        for (uint32_t i = 0; i < 512; ++i)
            if (b[i] != dataByte(2 + lba / 4, (lba % 4) * 512 + i)) all = false;
    }
    CHECK(all);
    CHECK(!disk.read(disk.sectors(), words));
}

TEST(test_raw_track_disk_modes) {
    for (auto layout : {std::pair<uint32_t, uint32_t>{2048, 0}, {2336, 8}, {2352, 24}}) {
        MemoryFile bin = rawBin(5, layout.first, layout.second);
        RawTrackDisk disk(bin);
        cue_sheet::Track t;
        t.stride = layout.first;
        t.data_offset = layout.second;
        disk.setTrack(t);
        CHECK_EQ(disk.sectors(), 20u);
        uint16_t words[256];
        CHECK(disk.read(19, words));  // the last quarter of the last frame
        const uint8_t* b = reinterpret_cast<const uint8_t*>(words);
        CHECK_EQ(b[0], dataByte(4, 1536));
        CHECK_EQ(b[511], dataByte(4, 2047));
    }
}

void runCueSheetTests() {
    RUN(test_cue_one_data_track);
    RUN(test_cue_mixed_mode_stops_at_the_audio);
    RUN(test_cue_tracks_in_their_own_files);
    RUN(test_cue_modes);
    RUN(test_cue_details);
    RUN(test_cue_refused);
    RUN(test_cue_names);
    RUN(test_raw_track_disk_reads_the_data);
    RUN(test_raw_track_disk_modes);
}
