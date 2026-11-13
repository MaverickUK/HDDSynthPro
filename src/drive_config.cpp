#include "drive_config.h"

#include "identify.h"  // MAX_PIO_MODE

#include <cctype>
#include <cstdio>
#include <cstring>

namespace drive_config {

namespace {

const char* const MODE_NAMES[] = {"auto",  "pio0",  "pio1",  "pio2",  "pio3",  "pio4",
                                  "mdma0", "mdma1", "mdma2", "udma0", "udma1", "udma2",
                                  "udma3", "udma4", "udma5", "udma6"};
constexpr size_t MODE_COUNT = sizeof MODE_NAMES / sizeof MODE_NAMES[0];

// A piece of the file's text: [begin, end).
struct Span {
    const char* begin;
    const char* end;
    size_t size() const { return size_t(end - begin); }
    bool empty() const { return begin == end; }
};

bool isSpace(char c) { return c == ' ' || c == '\t'; }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

Span trim(Span s) {
    while (!s.empty() && isSpace(*s.begin)) ++s.begin;
    while (!s.empty() && isSpace(s.end[-1])) --s.end;
    return s;
}

bool equalsNoCase(Span s, const char* word) {
    size_t n = std::strlen(word);
    if (s.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (lower(s.begin[i]) != word[i]) return false;
    return true;
}

// Where a line's comment starts: `#` at the start or after a space or tab,
// or `;` at the start (as in other INI files). `end` if there is none, so a
// file name with # in it, like DISK#1.IMG, still works.
const char* commentStart(Span line) {
    Span t = trim(line);
    if (!t.empty() && *t.begin == ';') return t.begin;
    for (const char* p = line.begin; p < line.end; ++p)
        if (*p == '#' && (p == line.begin || isSpace(p[-1]))) return p;
    return line.end;
}

// One line of the file, taken apart.
struct Line {
    Span whole;    // without the line ending
    Span key;      // trimmed; empty if the line sets nothing
    Span value;    // trimmed, without the comment
    bool has_equals = false;
    bool blank = false;  // nothing but space and comment
};

Line split(Span whole) {
    Line l;
    l.whole = whole;
    Span content = trim(Span{whole.begin, commentStart(whole)});
    if (content.empty()) {
        l.blank = true;
        return l;
    }
    const char* eq = static_cast<const char*>(std::memchr(content.begin, '=', content.size()));
    if (!eq) return l;
    l.has_equals = true;
    l.key = trim(Span{content.begin, eq});
    l.value = trim(Span{eq + 1, content.end});
    // "quoted values" may be quoted
    if (l.value.size() >= 2 && *l.value.begin == '"' && l.value.end[-1] == '"') {
        ++l.value.begin;
        --l.value.end;
    }
    return l;
}

// Calls `fn(line_number, Line)` for each line of `text`, after a UTF-8
// byte-order mark if there is one.
template <typename Fn>
void forEachLine(const char* text, size_t len, Fn fn) {
    const char* p = text;
    const char* end = text + len;
    if (len >= 3 && uint8_t(p[0]) == 0xEF && uint8_t(p[1]) == 0xBB && uint8_t(p[2]) == 0xBF) p += 3;
    int number = 1;
    while (p < end) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
        const char* line_end = nl ? nl : end;
        Span whole{p, line_end};
        // CRLF, and any extra CRs (a file ending in a bare CR, finished by setKey, had CR CR LF)
        while (!whole.empty() && whole.end[-1] == '\r') --whole.end;
        fn(number++, split(whole));
        p = nl ? nl + 1 : end;
    }
}

void warn(Config& c, const char* format, int line, Span s, const char* extra = "") {
    if (c.warning_count < MAX_WARNINGS) {
        int shown = int(s.size() > 32 ? 32 : s.size());
        std::snprintf(c.warnings[c.warning_count], MAX_WARNING, format, line, shown, s.begin, extra);
    }
    ++c.warning_count;
}

bool validImageName(Span v) {
    if (v.empty() || v.size() > MAX_NAME) return false;
    for (const char* p = v.begin; p < v.end; ++p) {
        char ch = *p;
        if (uint8_t(ch) < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|')
            return false;
    }
    return !equalsNoCase(v, "hddsynth.ini");
}

bool parseMode(Span v, Mode& out) {
    for (size_t i = 0; i < MODE_COUNT; ++i) {
        if (equalsNoCase(v, MODE_NAMES[i])) {
            out = Mode(i);
            return true;
        }
    }
    return false;
}

}  // namespace

const char* modeName(Mode m) { return size_t(m) < MODE_COUNT ? MODE_NAMES[size_t(m)] : "?"; }

static bool isPio(Mode m) { return m >= Mode::Pio0 && m <= Mode::Pio4; }
static bool isMdma(Mode m) { return m >= Mode::Mdma0 && m <= Mode::Mdma2; }

bool modeSupported(Mode m) {
    return m == Mode::Auto || (isPio(m) && uint8_t(m) - uint8_t(Mode::Pio0) <= MAX_PIO_MODE) ||
           (isMdma(m) && uint8_t(m) - uint8_t(Mode::Mdma0) <= MAX_MDMA_MODE);
}

uint8_t pioModeOffered(Mode m) {
    // A DMA mode offers the fastest PIO mode too, for the commands that use it.
    if (!isPio(m) || !modeSupported(m)) return AUTO_PIO_MODE;
    return uint8_t(uint8_t(m) - uint8_t(Mode::Pio0));
}

uint8_t mdmaModeOffered(Mode m, bool dmarq_wired, bool auto_dma) {
    if (!dmarq_wired) return NO_DMA;
    if (m == Mode::Auto) return auto_dma ? MAX_MDMA_MODE : NO_DMA;
    if (!isMdma(m) || !modeSupported(m)) return NO_DMA;
    return uint8_t(uint8_t(m) - uint8_t(Mode::Mdma0));
}

bool dmaRefusedForWiring(Mode m, bool dmarq_wired) {
    return !dmarq_wired && isMdma(m) && modeSupported(m);
}

void defaults(Config& out) {
    std::snprintf(out.device0, sizeof out.device0, "%s", DEFAULT_IMAGE);
    out.device1[0] = 0;
    std::snprintf(out.cdrom, sizeof out.cdrom, "%s", DEFAULT_CDROM);
    out.mode = Mode::Auto;
    out.dma = DEFAULT_DMA;
    out.write_behind = DEFAULT_WRITE_BEHIND;
    out.read_ahead = DEFAULT_READ_AHEAD;
    out.activity_link = DEFAULT_ACTIVITY_LINK;
    out.isa = DEFAULT_ISA;
    out.early_answers = DEFAULT_EARLY_ANSWERS;
    out.iordy = DEFAULT_IORDY;
    out.register_table = DEFAULT_REGISTER_TABLE;
    out.geometry = Geometry{};
    out.seek_timing = seek_timing::Setting{};
    out.warning_count = 0;
}

namespace {

// The on/off settings: their keys and where each goes.
struct Switch {
    const char* key;
    bool Config::*field;
};
const Switch SWITCHES[] = {
    {"dma", &Config::dma},
    {"write_behind", &Config::write_behind},
    {"read_ahead", &Config::read_ahead},
    {"activity_link", &Config::activity_link},
    {"isa", &Config::isa},
    {"early_answers", &Config::early_answers},
    {"iordy", &Config::iordy},
    {"register_table", &Config::register_table},
};
constexpr size_t SWITCH_COUNT = sizeof SWITCHES / sizeof SWITCHES[0];

// on/off, yes/no, true/false, 1/0, in any case.
bool parseOnOff(Span v, bool& on) {
    static const char* const ON[] = {"on", "yes", "true", "1"};
    static const char* const OFF[] = {"off", "no", "false", "0"};
    for (const char* w : ON)
        if (equalsNoCase(v, w)) return on = true, true;
    for (const char* w : OFF)
        if (equalsNoCase(v, w)) return on = false, true;
    return false;
}

// auto, or C/H/S: cylinders 1-16383, heads 1-16, sectors 1-63 (IDENTIFY's limits).
bool parseGeometry(Span v, Geometry& g) {
    if (equalsNoCase(v, "auto")) return g = Geometry{}, true;
    uint32_t n[3] = {0, 0, 0};
    unsigned field = 0;
    bool digits = false;
    for (const char* p = v.begin; p != v.end; ++p) {
        if (*p >= '0' && *p <= '9') {
            n[field] = n[field] * 10 + unsigned(*p - '0');
            if (n[field] > 99999) return false;
            digits = true;
        } else if (*p == '/' && digits && field < 2) {
            ++field;
            digits = false;
        } else {
            return false;
        }
    }
    if (field != 2 || !digits) return false;
    if (n[0] < 1 || n[0] > 16383 || n[1] < 1 || n[1] > 16 || n[2] < 1 || n[2] > 63) return false;
    g = Geometry{uint16_t(n[0]), uint16_t(n[1]), uint16_t(n[2])};
    return true;
}

}  // namespace

bool isKey(const char* key) {
    Span k{key, key + std::strlen(key)};
    if (equalsNoCase(k, "device0") || equalsNoCase(k, "image") || equalsNoCase(k, "device1") ||
        equalsNoCase(k, "mode") || equalsNoCase(k, "seek_timing") || equalsNoCase(k, "geometry") ||
        equalsNoCase(k, "cdrom"))
        return true;
    for (const Switch& s : SWITCHES)
        if (equalsNoCase(k, s.key)) return true;
    return false;
}

void parse(const char* text, size_t len, Config& out) {
    defaults(out);
    // `image` is device0's old name: device0 counts over it, whichever comes first.
    bool seen_device0 = false, seen_image = false, seen_device1 = false, seen_mode = false,
         seen_seek_timing = false, seen_geometry = false, seen_cdrom = false;
    auto setDevice0 = [&](int n, const Line& l) {
        if (!validImageName(l.value)) {
            warn(out, "line %d: device0 '%.*s' isn't a file name in the root folder; ignored%s", n, l.value);
            return;
        }
        std::snprintf(out.device0, sizeof out.device0, "%.*s", int(l.value.size()), l.value.begin);
    };
    bool seen_switch[SWITCH_COUNT] = {};
    forEachLine(text, len, [&](int n, const Line& l) {
        if (l.blank) return;
        if (!l.has_equals) {
            Span t = trim(l.whole);
            if (!t.empty() && *t.begin == '[') return;  // an INI [section]: not used, harmless
            warn(out, "line %d: '%.*s' isn't key = value; ignored%s", n, t);
            return;
        }
        if (equalsNoCase(l.key, "device0")) {
            if (seen_device0) warn(out, "line %d: '%.*s' sets device0 again; this one counts%s", n, l.key);
            else if (seen_image) warn(out, "line %d: '%.*s' replaces image, its old name%s", n, l.key);
            seen_device0 = true;
            setDevice0(n, l);
        } else if (equalsNoCase(l.key, "image")) {
            if (seen_device0) {
                warn(out, "line %d: '%.*s' is device0's old name; device0 counts%s", n, l.key);
                return;
            }
            if (seen_image) warn(out, "line %d: '%.*s' sets image again; this one counts%s", n, l.key);
            seen_image = true;
            setDevice0(n, l);
        } else if (equalsNoCase(l.key, "device1")) {
            if (seen_device1) warn(out, "line %d: '%.*s' sets device1 again; this one counts%s", n, l.key);
            seen_device1 = true;
            if (equalsNoCase(l.value, NO_IMAGE) || l.value.empty()) {
                out.device1[0] = 0;
                return;
            }
            if (!validImageName(l.value)) {
                warn(out, "line %d: device1 '%.*s' isn't a file or folder name in the root folder; ignored%s", n,
                     l.value);
                return;
            }
            std::snprintf(out.device1, sizeof out.device1, "%.*s", int(l.value.size()), l.value.begin);
        } else if (equalsNoCase(l.key, "cdrom")) {
            if (seen_cdrom) warn(out, "line %d: '%.*s' sets cdrom again; this one counts%s", n, l.key);
            seen_cdrom = true;
            if (!validImageName(l.value)) {
                warn(out, "line %d: cdrom '%.*s' isn't a file name in the root folder; ignored%s", n, l.value);
                return;
            }
            std::snprintf(out.cdrom, sizeof out.cdrom, "%.*s", int(l.value.size()), l.value.begin);
        } else if (equalsNoCase(l.key, "mode")) {
            if (seen_mode) warn(out, "line %d: '%.*s' sets mode again; this one counts%s", n, l.key);
            seen_mode = true;
            Mode m;
            if (!parseMode(l.value, m)) {
                warn(out, "line %d: mode '%.*s' isn't a transfer mode; using auto%s", n, l.value);
                out.mode = Mode::Auto;
                return;
            }
            out.mode = m;
            if (!modeSupported(m))
                warn(out, "line %d: mode %.*s isn't supported yet%s", n, l.value, "; using auto (PIO 0)");
        } else if (equalsNoCase(l.key, "seek_timing")) {
            if (seen_seek_timing) warn(out, "line %d: '%.*s' is set again; this one counts%s", n, l.key);
            seen_seek_timing = true;
            if (!seek_timing::parseSetting(l.value.begin, l.value.size(), out.seek_timing))
                warn(out, "line %d: seek_timing '%.*s' isn't off, synth or a preset; ignored%s", n, l.value);
        } else if (equalsNoCase(l.key, "geometry")) {
            if (seen_geometry) warn(out, "line %d: '%.*s' is set again; this one counts%s", n, l.key);
            seen_geometry = true;
            if (!parseGeometry(l.value, out.geometry)) {
                warn(out, "line %d: geometry '%.*s' isn't auto or C/H/S (1-16383/1-16/1-63); using auto%s", n,
                     l.value);
                out.geometry = Geometry{};
            }
        } else {
            for (size_t i = 0; i < SWITCH_COUNT; ++i) {
                if (!equalsNoCase(l.key, SWITCHES[i].key)) continue;
                if (seen_switch[i]) warn(out, "line %d: '%.*s' is set again; this one counts%s", n, l.key);
                seen_switch[i] = true;
                bool on;
                if (!parseOnOff(l.value, on)) {
                    warn(out, "line %d: '%.*s' isn't on or off; ignored%s", n, l.value);
                    return;
                }
                out.*SWITCHES[i].field = on;
                return;
            }
            warn(out, "line %d: unknown key '%.*s'; ignored%s", n, l.key);
        }
    });
}

// A key's current name: `image` is device0's old one.
static const char* currentName(Span key) {
    if (equalsNoCase(key, "image") || equalsNoCase(key, "device0")) return "device0";
    return nullptr;
}

size_t setKey(const char* text, size_t len, const char* key, const char* value, char* out, size_t size) {
    if (!isKey(key)) return 0;
    Span wanted{key, key + std::strlen(key)};
    const char* renamed = currentName(wanted);  // non-null for a key with an old name
    if (renamed) key = renamed;
    // The line that counts is the one to change: the last one setting `key`,
    // or for device0, its last line under the new name, else under the old.
    const char* target_value_begin = nullptr;
    const char* target_value_end = nullptr;
    Span target_key{nullptr, nullptr};
    bool target_new_name = false;
    bool crlf = false;
    forEachLine(text, len, [&](int, const Line& l) {
        if (l.whole.end < text + len && *l.whole.end == '\r') crlf = true;
        if (!l.has_equals) return;
        bool matches = renamed ? currentName(l.key) == renamed : equalsNoCase(l.key, key);
        if (!matches) return;
        bool new_name = equalsNoCase(l.key, key);
        if (target_new_name && !new_name) return;  // the new name counts over the old
        target_new_name = new_name;
        target_key = l.key;
        // Replace from just after "=" and its spaces to the end of the
        // value (quotes included), so the key's spelling and the comment stay.
        const char* eq = static_cast<const char*>(std::memchr(l.whole.begin, '=', l.whole.size()));
        const char* v = eq + 1;
        while (v < l.value.begin && isSpace(*v) && *v != '"') ++v;
        target_value_begin = v;
        target_value_end = l.value.end;
        if (target_value_end < l.whole.end && *target_value_end == '"') ++target_value_end;
    });
    const char* nl = crlf ? "\r\n" : "\n";
    int n;
    if (target_value_begin && !target_new_name && renamed) {
        // A line under the old name: rewritten with the new one.
        n = std::snprintf(out, size, "%.*s%s%.*s%s%.*s", int(target_key.begin - text), text, key,
                          int(target_value_begin - target_key.end), target_key.end, value,
                          int(text + len - target_value_end), target_value_end);
    } else if (target_value_begin) {
        n = std::snprintf(out, size, "%.*s%s%.*s", int(target_value_begin - text), text, value,
                          int(text + len - target_value_end), target_value_end);
    } else {
        bool ends_with_newline = len == 0 || text[len - 1] == '\n';
        // A last line ending in a bare CR gets only its LF (CR CR LF left its
        // value with a CR on the end, 14 Nov 2026)
        const char* finish = ends_with_newline ? "" : text[len - 1] == '\r' ? "\n" : nl;
        n = std::snprintf(out, size, "%.*s%s%s = %s%s", int(len), text, finish, key, value, nl);
    }
    return (n > 0 && size_t(n) < size) ? size_t(n) : 0;
}

namespace {

// True unless `name` is `none`, in any case.
bool notNone(const char* name) {
    const char* b = NO_IMAGE;
    for (; *name && *b; ++name, ++b)
        if (std::tolower(static_cast<unsigned char>(*name)) != *b) return true;
    return *name || *b;  // a different length: some other name
}

}  // namespace

bool servesImage(const Config& c) { return notNone(c.device0); }

bool servesDisc(const Config& c) { return notNone(c.cdrom); }

bool hasDevice1(const Config& c) { return c.device1[0] != 0; }

// `cdrom`, or a disc's file name (.ISO or .CUE): a CD-ROM drive.
static bool namesCdrom(const char* device) {
    Span name{device, device + std::strlen(device)};
    if (equalsNoCase(name, "cdrom")) return true;
    if (name.size() <= 4) return false;
    Span ext{name.end - 4, name.end};
    return equalsNoCase(ext, ".iso") || equalsNoCase(ext, ".cue");
}

// The disc a CD-ROM drive named by `device` serves.
static const char* discFor(const Config& c, const char* device) {
    Span name{device, device + std::strlen(device)};
    return equalsNoCase(name, "cdrom") ? c.cdrom : device;
}

bool device0IsCdrom(const Config& c) { return namesCdrom(c.device0); }
const char* device0Disc(const Config& c) { return discFor(c, c.device0); }
bool device1IsCdrom(const Config& c) { return namesCdrom(c.device1); }
const char* device1Disc(const Config& c) { return discFor(c, c.device1); }

const char* defaultFile() {
    return "# HDDSynthPro settings. Changes take effect when the drive restarts.\r\n"
           "# One setting per line, as key = value; # starts a comment.\r\n"
           "\r\n"
           "# The disk image to serve as the first drive (device 0): a file in the\r\n"
           "# card's root folder. If it isn't there, the drive uses HDDSYNTH.IMG, or\r\n"
           "# else its welcome disk. none: the built-in test disk (64 MB; writes are\r\n"
           "# lost when it restarts). cdrom (or an .ISO or .CUE file's name): a CD-ROM\r\n"
           "# drive instead, alone on the cable. Older files call this setting image.\r\n"
           "device0 = HDDSYNTH.IMG\r\n"
           "\r\n"
           "# A second drive (device 1), so a PC sees two hard disks: an image, or a\r\n"
           "# folder in the root folder, served read-only. cdrom (or an .ISO or .CUE\r\n"
           "# file's name): a CD-ROM drive. none: no second drive.\r\n"
           "device1 = none\r\n"
           "\r\n"
           "# The transfer mode to offer the host: auto (the fastest both sides\r\n"
           "# support), or at most one of pio0-pio4, mdma0-mdma2, udma0-udma6.\r\n"
           "# This firmware supports pio0-pio3 and mdma0-mdma2; others mean auto.\r\n"
           "# DMA needs DMARQ wired (IDE pin 21); without it the drive offers PIO.\r\n"
           "mode = auto\r\n"
           "\r\n"
           "# Whether auto may offer multiword DMA, where DMARQ is wired: on or off.\r\n"
           "dma = off\r\n"
           "\r\n"
           "# Acknowledge each written sector while it's being stored: about 50%\r\n"
           "# faster writes to the card; a power cut mid-store loses that sector.\r\n"
           "write_behind = on\r\n"
           "\r\n"
           "# Fetch the next sector while the host reads this one.\r\n"
           "read_ahead = on\r\n"
           "\r\n"
           "# Tell an HDD Synth what the drive is doing (GPIO 0, docs/activity-link.md).\r\n"
           "activity_link = on\r\n"
           "\r\n"
           "# Hold each command for as long as a modelled drive would take (the seek,\r\n"
           "# the platter coming round, the transfer), so the computer runs at that\r\n"
           "# drive's pace: off; synth (the drive the HDD Synth is playing); or a\r\n"
           "# preset: stepper85, voice91, voice97 or fdb05.\r\n"
           "seek_timing = off\r\n"
           "\r\n"
           "# on for a 16-bit ISA IDE card (286, 386, many 486s): the drive lets go\r\n"
           "# of IORDY if it has held it for 23 us, as these cards pass it to the\r\n"
           "# whole ISA bus. Leave off for USB adapters and later controllers.\r\n"
           "isa = off\r\n"
           "\r\n"
           "# The drive has each register's value ready as soon as it's addressed,\r\n"
           "# for controllers that don't wait for IORDY (some PCI ones while the BIOS\r\n"
           "# detects the drive); it still holds IORDY for those that do. On suits\r\n"
           "# every PC so far.\r\n"
           "early_answers = on\r\n"
           "\r\n"
           "# off for a controller that checks IORDY before it starts each access (an\r\n"
           "# Amiga 1200's): the drive never touches IORDY. Use with early_answers = on.\r\n"
           "iordy = on\r\n"
           "\r\n"
           "# Experimental: every read answered at once by the drive's own hardware,\r\n"
           "# with no wait for its processor (fastest through USB adapters, and in\r\n"
           "# time for controllers that never wait). The hard disk only, without DMA;\r\n"
           "# isa = on overrides it. Not yet tried on retro PCs.\r\n"
           "register_table = off\r\n"
           "\r\n"
           "# The CHS geometry the drive reports, which a BIOS's auto-detect picks up:\r\n"
           "# auto (the one the image's partition table was made with, else 16 heads\r\n"
           "# and 63 sectors), or cylinders/heads/sectors, e.g. geometry = 756/13/51.\r\n"
           "geometry = auto\r\n"
           "\r\n"
           "# The CD-ROM drive's disc (device0 = cdrom or device1 = cdrom): an ISO,\r\n"
           "# or a CUE sheet and its BIN (the data track), in the root folder, read-only.\r\n"
           "# none: the built-in test disc.\r\n"
           "cdrom = HDDSYNTH.ISO\r\n";
}

}  // namespace drive_config
