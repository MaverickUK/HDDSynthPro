// CD images as a CUE sheet and its BIN (docs/atapi.md): the CD-ROM drive
// serves the first track's data, if it is a data track, from the BIN's raw
// frames. Audio tracks after it are left out (the drive has no audio out),
// so a mixed-mode game's data works, without its music. Pure C++, host-tested.
//
//   FILE "GAME.BIN" BINARY
//     TRACK 01 MODE1/2352
//       INDEX 01 00:00:00
//     TRACK 02 AUDIO
//       INDEX 00 32:10:40     <- the data track ends here
//       INDEX 01 32:12:40
#pragma once

#include <cstddef>
#include <cstdint>

#include "block_source.h"

namespace cue_sheet {

constexpr size_t MAX_TEXT = 1536;  // what is read of a sheet: track 1 and where track 2 starts are near its top
constexpr size_t MAX_FILE_NAME = 64;
constexpr uint32_t FRAME_BYTES = 2048;  // a data frame, as the drive serves it

// Where the data track's 2,048-byte frames are in the BIN.
struct Track {
    uint32_t stride = 0;       // bytes per frame in the file: 2048, 2336 or 2352
    uint32_t data_offset = 0;  // the data's place in each: 0, 8 (MODE2/2336), 16 (MODE1/2352), 24 (MODE2/2352)
    uint32_t first_frame = 0;  // INDEX 01 of track 1, in the file's frames
    uint32_t end_frame = 0;    // where the next track in the same file starts; 0: the file's end
};

struct Sheet {
    char file[MAX_FILE_NAME + 1] = {};  // the BIN's name, without any folder
    Track track;
    uint8_t tracks = 0;  // tracks in the text parsed, for the log
    const char* error = nullptr;  // why parse() failed
};

// Whether `name` ends in .CUE (any case).
bool isCueName(const char* name);

// Reads the sheet. False, with `error`, for anything it can't serve: no FILE,
// a first track that's audio, of a mode it doesn't know or empty, a data file
// that isn't BINARY, a name too long. Track 1 ends where the next track in its
// file starts (its first INDEX), else at the file's end.
bool parse(const char* text, size_t len, Sheet& out);

// The data track's frames in a BIN of `file_bytes`.
uint32_t frames(const Track& t, uint64_t file_bytes);

}  // namespace cue_sheet

// The data track as a disk of 512-byte sectors, four to a frame, as the
// CD-ROM drive reads its discs (atapi::SECTORS_PER_FRAME).
class RawTrackDisk : public BlockSource {
public:
    explicit RawTrackDisk(ImageFile& file) : file_(file) {}
    // Which track, after the file is (re)opened.
    void setTrack(const cue_sheet::Track& t) {
        track_ = t;
        sectors_ = cue_sheet::frames(t, file_.size()) * 4;
    }
    uint32_t sectors() const override { return sectors_; }
    bool read(uint32_t lba, uint16_t words[256]) override {
        if (lba >= sectors_) return false;
        uint64_t frame = uint64_t(track_.first_frame) + lba / 4;
        uint64_t offset = frame * track_.stride + track_.data_offset + (lba % 4) * 512u;
        return file_.readAt(offset, words, 512);
    }

private:
    ImageFile& file_;
    cue_sheet::Track track_;
    uint32_t sectors_ = 0;
};
