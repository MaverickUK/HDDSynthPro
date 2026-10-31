// Images split across files, for disks over FAT32's 4 GB file limit: the
// image `NAME.IMG` continues in `NAME.I01`, `NAME.I02` and so on, served as
// one disk (sd_card maps every part's clusters into its extent map). Pure
// C++, host-tested.
#pragma once

#include <cstddef>

namespace image_parts {

// The most parts an image may have (99 would fit the names; 8 parts of
// 4 GB reach 32 GB, past what an LBA28 drive can address anyway).
constexpr unsigned MAX_PARTS = 8;

// The name of part `n` of image `base`: part 0 is `base` itself; part n >= 1
// is `base` with its extension replaced by I01, I02... (or .I01 added to a
// name without one). False if it doesn't fit `size` bytes with its NUL.
bool partName(const char* base, unsigned n, char* out, size_t size);

}  // namespace image_parts
