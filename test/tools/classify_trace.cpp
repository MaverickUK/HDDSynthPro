// Replays a recorded trace through the activity classifier (activity.h),
// to tune its thresholds on real workloads without the hardware.
//
//   .pio/native-tests/classify_trace < host/runs/<run>/trace.txt
//
// Reads CMD and DONE lines (the drive build's trace format), ticks the
// classifier every millisecond of trace time as the firmware does, and
// prints the ACT lines it would have produced, then the time in each state.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "activity.h"

int main() {
    activity::Classifier c;
    uint64_t time_in[3] = {};
    bool started = false;
    uint32_t now = 0;
    std::string line;
    char out[64];

    auto advance = [&](uint32_t to) {
        if (!started) {
            now = to;
            started = true;
            return;
        }
        while (int32_t(to - now) > 0) {
            uint32_t step = (to - now) < 1000 ? (to - now) : 1000;
            now += step;
            if (c.update(now)) std::fwrite(out, 1, activity::formatActivity(now, c, out, sizeof out), stdout);
            time_in[int(c.state())] += step;
        }
    };

    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string kind;
        unsigned long t = 0;
        in >> kind >> t;
        if (kind == "CMD") {
            std::string cmd_hex, name;
            unsigned long lba = 0, count = 0;
            in >> cmd_hex >> name >> lba >> count;
            uint8_t cmd = uint8_t(std::stoul(cmd_hex, nullptr, 16));
            if (!activity::isMediaCommand(cmd)) continue;
            advance(uint32_t(t));
            bool seek_only = (cmd >= 0x10 && cmd <= 0x1F) || (cmd >= 0x70 && cmd <= 0x7F);
            c.onCommand(uint32_t(t), uint32_t(lba), seek_only ? 0 : uint32_t(count));
        } else if (kind == "DONE") {
            std::string cmd_hex;
            in >> cmd_hex;
            if (!activity::isMediaCommand(uint8_t(std::stoul(cmd_hex, nullptr, 16)))) continue;
            advance(uint32_t(t));
            c.onCommandEnd(uint32_t(t));
        }
    }
    advance(now + 1000000);  // let it settle to Idle

    uint64_t total = time_in[0] + time_in[1] + time_in[2];
    std::printf("# %lu sequential, %lu random commands; time IDLE %.1f%% SEQ %.1f%% RAND %.1f%%\n",
                (unsigned long)c.sequentialCommands(), (unsigned long)c.randomCommands(),
                total ? 100.0 * time_in[0] / total : 0, total ? 100.0 * time_in[1] / total : 0,
                total ? 100.0 * time_in[2] / total : 0);
    return 0;
}
