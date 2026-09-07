// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/config.h"

#include <fstream>
#include <stdexcept>

#ifndef _WIN32
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace tapto {

namespace {

std::string trim(const std::string& s) {
    static const char* ws = " \t\r";
    size_t begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return std::string();
    size_t end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// Replace CR/LF with spaces. The config format is one "key = value" per line,
// so a newline in a key or value would corrupt the file on save.
std::string strip_newlines(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

// Split `line` into (key, value) if it is a well-formed "key = value" line,
// otherwise return false. Comment lines (first non-blank char is '#' or ';'),
// blank lines, and lines without an equals sign are not key/value lines.
bool parse_line(const std::string& line, std::string& key_out, std::string& value_out) {
    std::string text = trim(line);
    if (text.empty() || text[0] == '#' || text[0] == ';') return false;
    size_t eq = text.find('=');
    if (eq == std::string::npos) return false;
    key_out = trim(text.substr(0, eq));
    if (key_out.empty()) return false;
    value_out = trim(text.substr(eq + 1));
    return true;
}

} // namespace

Config Config::load(const fs::path& path) {
    Config cfg;
    std::ifstream in(path);
    if (!in) {
        // Missing file: seed with the header comment so save() yields a
        // recognizable, non-empty config (matching the previous behaviour).
        cfg.lines_.push_back("# tapto-code");
        return cfg;
    }

    // Keep every line verbatim — comments, blank lines, and ordering included.
    // Only a trailing CR (from CRLF files) is dropped, so on Windows the file
    // round-trips to clean LF endings.
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        cfg.lines_.push_back(line);
    }
    return cfg;
}

std::optional<std::string> Config::get(const std::string& key) const {
    // Last occurrence in the file wins (the previous "update in place / append"
    // semantics made the last value authoritative).
    for (size_t i = lines_.size(); i-- > 0;) {
        std::string k, v;
        if (parse_line(lines_[i], k, v) && k == key) return v;
    }
    return std::nullopt;
}

void Config::set(const std::string& key, const std::string& value) {
    std::string k = trim(strip_newlines(key));
    std::string v = strip_newlines(value);
    if (k.empty()) return;

    // Rewrite the last matching line in place so the rest of the file is left
    // untouched; if the key isn't present yet, append a new line to the end.
    for (size_t i = lines_.size(); i-- > 0;) {
        std::string lk, lv;
        if (parse_line(lines_[i], lk, lv) && lk == k) {
            lines_[i] = k + " = " + v;
            return;
        }
    }
    lines_.push_back(k + " = " + v);
}

bool Config::unset(const std::string& key) {
    std::vector<std::string> kept;
    kept.reserve(lines_.size());
    bool removed = false;
    for (const auto& line : lines_) {
        std::string k, v;
        if (parse_line(line, k, v) && k == key) {
            removed = true;
            continue; // drop this line
        }
        kept.push_back(line);
    }
    if (removed) lines_ = std::move(kept);
    return removed;
}

std::vector<Config::Entry> Config::entries() const {
    std::vector<Entry> out;
    for (const auto& line : lines_) {
        std::string k, v;
        if (!parse_line(line, k, v)) continue; // skip comments / blank / malformed
        bool found = false;
        for (auto& e : out) {
            if (e.first == k) {
                e.second = v; // last value wins
                found = true;
                break;
            }
        }
        if (!found) out.emplace_back(k, v);
    }
    return out;
}

// The store may hold a literal API key, so on POSIX the file is created
// owner-only (0600) and its directory 0700 — the default umask would leave
// ~/.tapto/config world-readable on a shared machine. The file is opened with
// the mode up front rather than chmod'ed after writing, so the key is never on
// disk readable for even a moment; an existing file is tightened too. On
// Windows the user's profile directory is already ACL'd to the user.
void Config::save(const fs::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
#ifndef _WIN32
        fs::permissions(path.parent_path(), fs::perms::owner_all, ec);
#endif
    }

    // Write the stored lines verbatim so comments, blank lines, and ordering
    // made by hand in the file survive a save.
    std::string text;
    for (const auto& line : lines_) {
        text += line;
        text += '\n';
    }

#ifndef _WIN32
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw std::runtime_error("cannot write config file: " + path.string());
    }
    (void)::fchmod(fd, 0600);
    size_t off = 0;
    while (off < text.size()) {
        ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n < 0) {
            ::close(fd);
            throw std::runtime_error("cannot write config file: " + path.string());
        }
        off += static_cast<size_t>(n);
    }
    ::close(fd);
#else
    // Binary, so Windows does not turn the LF that load() normalised to back
    // into CRLF: the file is meant to round-trip byte for byte.
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write config file: " + path.string());
    }
    out << text;
#endif
}

} // namespace tapto
