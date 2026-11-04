#include "cue_sheet.h"

#include <cctype>
#include <cstring>

namespace cue_sheet {

namespace {

struct Cursor {
    const char* p;
    const char* end;
};

void skipSpaces(Cursor& c) {
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t')) ++c.p;
}

// The next word, or a "quoted string" without its quotes, into `out`.
// False if there's none, or it doesn't fit.
bool word(Cursor& c, char* out, size_t size) {
    skipSpaces(c);
    if (c.p >= c.end) return false;
    size_t n = 0;
    if (*c.p == '"') {
        ++c.p;
        while (c.p < c.end && *c.p != '"') {
            if (n + 1 >= size) return false;
            out[n++] = *c.p++;
        }
        if (c.p < c.end) ++c.p;  // the closing quote
    } else {
        while (c.p < c.end && *c.p != ' ' && *c.p != '\t') {
            if (n + 1 >= size) return false;
            out[n++] = *c.p++;
        }
    }
    out[n] = '\0';
    return n > 0;
}

bool same(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::toupper(static_cast<unsigned char>(*a)) != std::toupper(static_cast<unsigned char>(*b))) return false;
    return *a == *b;
}

// mm:ss:ff (75 frames a second) as frames; false if it isn't one.
bool msf(const char* s, uint32_t& frames) {
    unsigned part[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(*s))) return false;
        unsigned v = 0;
        while (std::isdigit(static_cast<unsigned char>(*s))) {
            v = v * 10 + unsigned(*s++ - '0');
            if (v > 99999) return false;
        }
        part[i] = v;
        if (i < 2 && *s++ != ':') return false;
    }
    if (*s || part[1] > 59 || part[2] > 74) return false;
    frames = (part[0] * 60 + part[1]) * 75 + part[2];
    return true;
}

bool mode(const char* m, Track& t) {
    struct Known {
        const char* name;
        uint32_t stride, data_offset;
    };
    static const Known known[] = {
        {"MODE1/2048", 2048, 0},   // the data alone, as an ISO
        {"MODE1/2352", 2352, 16},  // sync (12), header (4), data, EDC/ECC (288)
        {"MODE2/2352", 2352, 24},  // XA form 1: sync, header, subheader (8), data, EDC/ECC
        {"MODE2/2336", 2336, 8},   // the same without sync and header
    };
    for (const Known& k : known)
        if (same(m, k.name)) {
            t.stride = k.stride;
            t.data_offset = k.data_offset;
            return true;
        }
    return false;
}

}  // namespace

bool isCueName(const char* name) {
    size_t n = std::strlen(name);
    return n > 4 && same(name + n - 4, ".cue");
}

bool parse(const char* text, size_t len, Sheet& out) {
    out = Sheet();
    const char* p = text;
    const char* end = text + len;
    if (len >= 3 && std::memcmp(p, "\xEF\xBB\xBF", 3) == 0) p += 3;  // a UTF-8 BOM

    bool in_track1 = false, after_track1 = false;
    bool done = false;  // the data track is settled: only tracks are counted from here
    while (p < end) {
        const char* eol = p;
        while (eol < end && *eol != '\n' && *eol != '\r') ++eol;
        Cursor c{p, eol};
        p = eol;
        while (p < end && (*p == '\n' || *p == '\r')) ++p;

        char key[16];
        if (!word(c, key, sizeof key)) continue;
        if (done) {
            if (same(key, "TRACK")) ++out.tracks;
            continue;
        }
        if (same(key, "FILE")) {
            if (in_track1 || after_track1) {  // the next track is in another file: track 1 runs to its file's end
                done = true;
                continue;
            }
            char name[MAX_FILE_NAME + 1], type[16];
            if (!word(c, name, sizeof name)) {
                out.error = "a FILE name too long";
                return false;
            }
            const char* base = name;
            for (const char* s = name; *s; ++s)
                if (*s == '/' || *s == '\\') base = s + 1;
            std::strcpy(out.file, base);
            if (!word(c, type, sizeof type) || !same(type, "BINARY")) {
                out.error = "the data isn't in a BINARY file";
                return false;
            }
        } else if (same(key, "TRACK")) {
            char number[8], kind[16];
            if (!word(c, number, sizeof number) || !word(c, kind, sizeof kind)) {
                out.error = "a TRACK line it can't read";
                return false;
            }
            ++out.tracks;
            if (out.tracks == 1) {
                if (!out.file[0]) {
                    out.error = "no FILE before the first TRACK";
                    return false;
                }
                if (same(kind, "AUDIO")) {
                    out.error = "the first track is audio: no data to serve";
                    return false;
                }
                if (!mode(kind, out.track)) {
                    out.error = "a track mode it doesn't know";
                    return false;
                }
                in_track1 = true;
            } else {
                in_track1 = false;
                after_track1 = true;
            }
        } else if (same(key, "INDEX")) {
            char number[8], time[16];
            uint32_t at = 0;
            if (!word(c, number, sizeof number) || !word(c, time, sizeof time) || !msf(time, at)) {
                out.error = "an INDEX line it can't read";
                return false;
            }
            if (in_track1 && same(number, "01")) {
                out.track.first_frame = at;
            } else if (after_track1 && !out.track.end_frame) {
                out.track.end_frame = at;  // the next track's first index (its pregap, if it has one)
                done = true;
            }
        }
        // REM, CATALOG, TITLE, PERFORMER, FLAGS, PREGAP (not in the file), POSTGAP: nothing to do
    }
    if (!out.tracks) {
        out.error = out.file[0] ? "no TRACK" : "no FILE";
        return false;
    }
    if (out.track.end_frame && out.track.end_frame <= out.track.first_frame) {
        out.error = "the data track is empty";
        return false;
    }
    return true;
}

uint32_t frames(const Track& t, uint64_t file_bytes) {
    if (!t.stride) return 0;
    uint64_t in_file = file_bytes / t.stride;
    uint64_t end = t.end_frame && t.end_frame < in_file ? t.end_frame : in_file;
    return end > t.first_frame ? uint32_t(end - t.first_frame) : 0;
}

}  // namespace cue_sheet
