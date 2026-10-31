#include "image_parts.h"

#include <cstdio>
#include <cstring>

namespace image_parts {

bool partName(const char* base, unsigned n, char* out, size_t size) {
    if (n == 0) return std::snprintf(out, size, "%s", base) < int(size);
    if (n > 99) return false;
    const char* dot = std::strrchr(base, '.');
    size_t stem = dot ? size_t(dot - base) : std::strlen(base);
    return std::snprintf(out, size, "%.*s.I%02u", int(stem), base, n) < int(size);
}

}  // namespace image_parts
