// Two buffers for core 0's one-off jobs: reading HDDSYNTH.INI at boot, console
// `E` (the file and its edited copy), a cue sheet, the zeros `I` and `K` fill
// a new image with. Each job once had 4 KB of its own for good (17.5 KB in
// all); none runs inside another, so they share these.
//
// The rules: core 0 only; a job owns a buffer only until it returns, keeps no
// pointer into it afterwards, and assumes nothing about what it holds on entry.
// A job that calls another gives it the buffer it isn't using itself.
#pragma once

#include <cstddef>
#include <cstdint>

namespace scratch {

constexpr size_t SIZE = 4096;
extern char a[SIZE];  // HDDSYNTH.INI's text (boot, `E`); a new image's zeros
extern char b[SIZE];  // `E`'s edited copy; a cue sheet (which boot's disk choice reads while `a` is done with)

}  // namespace scratch
