// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tapto {

// In-memory view of a single config file.
//
// The file's raw lines are kept verbatim (including comments and blank lines)
// so that load → set/unset → save is a surgical operation: only the specific
// line(s) for the key being touched are modified, everything else in the file
// is left exactly as the user wrote it.
class Config {
public:
    using Entry = std::pair<std::string, std::string>;

    // Load a config file. A missing file yields a Config seeded with the
    // default header comment so save() produces a recognizable file.
    static Config load(const std::filesystem::path& path);

    // Return the value for key (last occurrence in the file wins).
    // Returns nullopt if key is not present.
    std::optional<std::string> get(const std::string& key) const;

    // Insert or update a key. All other lines (comments, blank lines, other
    // keys, their order) are preserved.
    void set(const std::string& key, const std::string& value);

    // Remove all lines for key. Returns true if at least one was removed.
    bool unset(const std::string& key);

    // Parsed key/value pairs (comments and blank lines excluded).
    // For a key that appears more than once, the last value wins; the position
    // in the returned vector is the first occurrence of that key.
    std::vector<Entry> entries() const;

    // Write the file to disk, creating parent directories as needed.
    // Comments, blank lines, and line order are preserved exactly.
    // Throws on I/O failure.
    void save(const std::filesystem::path& path) const;

private:
    // Raw lines of the file (without trailing newlines). Storing the lines
    // rather than a key/value table is what lets set/unset make surgical
    // edits without touching any other line the user may have added by hand.
    std::vector<std::string> lines_;
};

} // namespace tapto
