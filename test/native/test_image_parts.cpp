// Tests for the split image part names (image_parts).
#include <cstring>

#include "check.h"
#include "image_parts.h"

using image_parts::partName;

TEST(test_image_part_names) {
    char name[32];
    CHECK(partName("A1200.IMG", 0, name, sizeof name));
    CHECK_STR(name, "A1200.IMG");
    CHECK(partName("A1200.IMG", 1, name, sizeof name));
    CHECK_STR(name, "A1200.I01");
    CHECK(partName("A1200.IMG", 12, name, sizeof name));
    CHECK_STR(name, "A1200.I12");
    CHECK(partName("DISK", 3, name, sizeof name));  // no extension: one added
    CHECK_STR(name, "DISK.I03");
    CHECK(partName("my.disk.img", 2, name, sizeof name));  // the last dot's extension
    CHECK_STR(name, "my.disk.I02");
    CHECK(!partName("A1200.IMG", 100, name, sizeof name));
    char tiny[8];
    CHECK(!partName("A1200.IMG", 1, tiny, sizeof tiny));  // doesn't fit
}

void runImagePartsTests() { RUN(test_image_part_names); }
