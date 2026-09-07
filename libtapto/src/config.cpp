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
    static const char* ws = " \t\r\n";
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

} // namespace

Config Config::load(const fs::path& path) {
    Config cfg;
    std::ifstream in(path);
    if (!in) return cfg; // missing file -> empty config

    std::string line;
    while (std::getline(in, line)) {
        std::string text = trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') continue;

        size_t eq = text.find('=');
        if (eq == std::string::npos) continue; // ignore malformed lines

        std::string key = trim(text.substr(0, eq));
        std::string value = trim(text.substr(eq + 1));
        if (!key.empty()) cfg.set(key, value);
    }
    return cfg;
}

std::optional<std::string> Config::get(const std::string& key) const {
    for (const auto& entry : entries_) {
        if (entry.first == key) return entry.second;
    }
    return std::nullopt;
}

void Config::set(const std::string& key, const std::string& value) {
    std::string k = strip_newlines(key);
    std::string v = strip_newlines(value);
    for (auto& entry : entries_) {
        if (entry.first == k) {
            entry.second = v;
            return;
        }
    }
    entries_.emplace_back(k, v);
}

bool Config::unset(const std::string& key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first == key) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
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

    std::string text = "# tapto-code\n";
    for (const auto& entry : entries_) {
        text += entry.first + " = " + entry.second + "\n";
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
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write config file: " + path.string());
    }
    out << text;
#endif
}

} // namespace tapto
