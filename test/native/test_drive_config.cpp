// Host-side tests for the settings file (drive_config.*): parsing as editors
// write it, mistakes reported but never fatal, and edits that keep the
// user's comments.
#include <cstring>
#include <string>

#include "check.h"
#include "drive_config.h"

using drive_config::Config;
using drive_config::Mode;

static Config parsed(const std::string& text) {
    Config c;
    drive_config::parse(text.data(), text.size(), c);
    return c;
}

static std::string edited(const std::string& text, const char* key, const char* value) {
    char out[drive_config::MAX_FILE];
    size_t n = drive_config::setKey(text.data(), text.size(), key, value, out, sizeof out);
    return std::string(out, n);
}

TEST(test_config_defaults) {
    Config c = parsed("");
    CHECK_STR(std::string(c.image), "HDDSYNTH.IMG");
    CHECK(c.mode == Mode::Auto);
    CHECK_EQ(c.warning_count, size_t(0));
}

TEST(test_config_default_file_is_the_defaults) {
    const char* f = drive_config::defaultFile();
    Config c = parsed(f);
    CHECK_STR(std::string(c.image), "HDDSYNTH.IMG");
    CHECK(c.mode == Mode::Auto);
    CHECK_EQ(c.warning_count, size_t(0));
    CHECK(std::strlen(f) < drive_config::MAX_FILE);
}

TEST(test_config_reads_both_keys) {
    Config c = parsed("image = GAMES.IMG\nmode = pio0\n");
    CHECK_STR(std::string(c.image), "GAMES.IMG");
    CHECK(c.mode == Mode::Pio0);
    CHECK_EQ(c.warning_count, size_t(0));
}

TEST(test_config_as_notepad_writes_it) {
    // A UTF-8 byte-order mark, CRLF line endings, odd spacing and case,
    // comments, a section header and a quoted value.
    Config c = parsed("\xEF\xBB\xBF# settings\r\n[drive]\r\n  IMAGE=\"Work Disk.img\"   # mine\r\n"
                      "Mode\t=  AUTO ; not a comment here\r\n; a comment line\r\n");
    CHECK_STR(std::string(c.image), "Work Disk.img");
    CHECK_EQ(c.warning_count, size_t(1));  // "AUTO ; not..." isn't a mode
    c = parsed("\xEF\xBB\xBFmode = AUTO\r\nimage = DISK#1.IMG # the # in the name stays\r\n");
    CHECK(c.mode == Mode::Auto);
    CHECK_STR(std::string(c.image), "DISK#1.IMG");
    CHECK_EQ(c.warning_count, size_t(0));
}

TEST(test_config_mistakes_are_reported_and_skipped) {
    Config c = parsed("colour = blue\nimage = ../etc/x.img\nmode = turbo\njust words\n");
    CHECK_STR(std::string(c.image), "HDDSYNTH.IMG");  // the bad name is ignored
    CHECK(c.mode == Mode::Auto);
    CHECK_EQ(c.warning_count, size_t(4));
    CHECK(std::strstr(c.warnings[0], "line 1") && std::strstr(c.warnings[0], "colour"));
    CHECK(std::strstr(c.warnings[1], "line 2"));
    CHECK(std::strstr(c.warnings[2], "turbo"));
    CHECK(std::strstr(c.warnings[3], "line 4"));
    c = parsed("image =\nimage = HDDSYNTH.INI\n");  // empty, and the settings file itself
    CHECK_STR(std::string(c.image), "HDDSYNTH.IMG");
    CHECK_EQ(c.warning_count, size_t(3));  // two bad names, and image set twice
}

TEST(test_config_last_setting_counts) {
    Config c = parsed("image = A.IMG\nimage = B.IMG\n");
    CHECK_STR(std::string(c.image), "B.IMG");
    CHECK_EQ(c.warning_count, size_t(1));
}

TEST(test_config_modes) {
    CHECK(parsed("mode = udma5").mode == Mode::Udma5);
    CHECK_EQ(parsed("mode = udma5").warning_count, size_t(1));  // not supported yet
    CHECK(parsed("mode = MDMA2").mode == Mode::Mdma2);
    CHECK(parsed("mode = pio0").mode == Mode::Pio0);
    CHECK_EQ(parsed("mode = pio0").warning_count, size_t(0));
    CHECK(drive_config::modeSupported(Mode::Auto) && drive_config::modeSupported(Mode::Pio0));
    CHECK(!drive_config::modeSupported(Mode::Pio4));
    CHECK_STR(std::string(drive_config::modeName(Mode::Udma6)), "udma6");
}

TEST(test_config_many_mistakes_keep_the_first_few) {
    std::string text;
    for (int i = 0; i < 10; ++i) text += "bad" + std::to_string(i) + " = 1\n";
    Config c = parsed(text);
    CHECK_EQ(c.warning_count, size_t(10));
    CHECK(std::strstr(c.warnings[drive_config::MAX_WARNINGS - 1], "bad3"));
}

TEST(test_config_set_key_keeps_comments) {
    std::string f = drive_config::defaultFile();
    std::string g = edited(f, "image", "GAMES.IMG");
    CHECK_STR(std::string(parsed(g).image), "GAMES.IMG");
    CHECK(g.find("# The disk image to serve") != std::string::npos);  // comments kept
    CHECK(g.find("image = GAMES.IMG\r\n") != std::string::npos);      // line ending kept
    CHECK_EQ(g.size(), f.size() - std::strlen("HDDSYNTH.IMG") + std::strlen("GAMES.IMG"));
    // A trailing comment and the key's own spelling stay; quotes are replaced.
    CHECK_STR(edited("Image = \"Old.img\"  # mine\n", "image", "New.img"), "Image = New.img  # mine\n");
    CHECK_STR(edited("mode=pio0\n", "mode", "auto"), "mode=auto\n");
}

TEST(test_config_set_key_adds_a_missing_one) {
    CHECK_STR(edited("image = A.IMG\n", "mode", "pio0"), "image = A.IMG\nmode = pio0\n");
    CHECK_STR(edited("image = A.IMG\r\n", "mode", "pio0"), "image = A.IMG\r\nmode = pio0\r\n");
    CHECK_STR(edited("image = A.IMG", "mode", "pio0"), "image = A.IMG\nmode = pio0\n");  // no final newline
    CHECK_STR(edited("", "image", "A.IMG"), "image = A.IMG\n");
    // The line that counts (the last) is the one changed.
    CHECK_STR(edited("image = A\nimage = B\n", "image", "C"), "image = A\nimage = C\n");
}

TEST(test_config_set_key_refuses) {
    char out[8];
    CHECK_EQ(drive_config::setKey("", 0, "colour", "blue", out, sizeof out), size_t(0));  // unknown key
    CHECK_EQ(drive_config::setKey("", 0, "image", "A_LONG_NAME.IMG", out, sizeof out), size_t(0));  // no room
}

void runDriveConfigTests() {
    RUN(test_config_defaults);
    RUN(test_config_default_file_is_the_defaults);
    RUN(test_config_reads_both_keys);
    RUN(test_config_as_notepad_writes_it);
    RUN(test_config_mistakes_are_reported_and_skipped);
    RUN(test_config_last_setting_counts);
    RUN(test_config_modes);
    RUN(test_config_many_mistakes_keep_the_first_few);
    RUN(test_config_set_key_keeps_comments);
    RUN(test_config_set_key_adds_a_missing_one);
    RUN(test_config_set_key_refuses);
}
