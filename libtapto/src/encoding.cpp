// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/encoding.h"

#include "tapto/log.h"

#include <string>

namespace {

void appendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Convert a (BOM-stripped) UTF-16 payload, big- or little-endian, to UTF-8,
// pairing surrogates into the code points they represent.
std::string utf16ToUtf8(const char* data, size_t bytes, bool bigEndian) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(data);
    std::string out;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        unsigned int hi = b[i + (bigEndian ? 0 : 1)];
        unsigned int lo = b[i + (bigEndian ? 1 : 0)];
        unsigned int u = (hi << 8) | lo;

        unsigned int cp;
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < bytes) {
            unsigned int lo2 = b[i + 2 + (bigEndian ? 1 : 0)];
            unsigned int hi2 = b[i + 2 + (bigEndian ? 0 : 1)];
            unsigned int u2 = (hi2 << 8) | lo2;
            if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                cp = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00);
                i += 2; // consume the low surrogate unit as well
            } else {
                cp = 0xFFFD;
            }
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            cp = 0xFFFD; // lone surrogate
        } else {
            cp = u;
        }
        appendUtf8(out, cp);
    }
    return out;
}

// Strip any byte sequence that is not well-formed UTF-8, substituting U+FFFD.
std::string replaceInvalidUtf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const size_t n = in.size();
    auto is_cont = [&](size_t k) {
        return k < n && (static_cast<unsigned char>(in[k]) & 0xC0) == 0x80;
    };

    size_t i = 0;
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            out.push_back(in[i]);
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2 && is_cont(i + 1)) {
            out.append(in, i, 2);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && is_cont(i + 1) && is_cont(i + 2)) {
            out.append(in, i, 3);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && c <= 0xF4 && is_cont(i + 1) && is_cont(i + 2) && is_cont(i + 3)) {
            out.append(in, i, 4);
            i += 4;
        } else {
            out += "\xEF\xBF\xBD"; // U+FFFD
            i += 1;
        }
    }
    return out;
}

} // namespace

namespace tapto {

std::string sanitizeUtf8(const std::string& in) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();

    if (n >= 4 && b[0] == 0xFF && b[1] == 0xFE && b[2] == 0x00 && b[3] == 0x00) {
        // UTF-32 LE BOM — body is UTF-16 LE pairs in little-endian order.
        return utf16ToUtf8(in.data() + 4, n - 4, /*bigEndian=*/false);
    }
    if (n >= 4 && b[0] == 0x00 && b[1] == 0x00 && b[2] == 0xFE && b[3] == 0xFF) {
        return utf16ToUtf8(in.data() + 4, n - 4, /*bigEndian=*/true);
    }
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        // UTF-16 LE BOM.
        return utf16ToUtf8(in.data() + 2, n - 2, /*bigEndian=*/false);
    }
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        // UTF-16 BE BOM.
        return utf16ToUtf8(in.data() + 2, n - 2, /*bigEndian=*/true);
    }
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        // UTF-8 BOM: valid UTF-8 but meaningless in body text; drop it.
        return replaceInvalidUtf8(in.substr(3));
    }

    return replaceInvalidUtf8(in);
}

std::string sanitizeToolResult(const std::string& result, const std::string& tool_name) {
    std::string clean = sanitizeUtf8(result);
    if (clean.size() != result.size()) {
        mclog("WARN: tool '" + tool_name + "' returned non-UTF-8 content (BOM or invalid bytes); normalized to UTF-8");
    }
    return clean;
}

} // namespace tapto
